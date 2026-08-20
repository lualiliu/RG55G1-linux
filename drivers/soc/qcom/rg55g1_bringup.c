// SPDX-License-Identifier: GPL-2.0-only
/*
 * RG55G1 bring-up: force-disable stock-dtbo-reenabled nodes before
 * of_platform_populate, especially everything after qcom,smem.
 */

#include <linux/init.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

static void rg55g1_force_disabled_node(struct device_node *np)
{
	struct property *pp;
	char *val;

	if (!np || !of_device_is_available(np))
		return;

	val = kstrdup("disabled", GFP_KERNEL);
	if (!val)
		return;

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(val);
		return;
	}

	pp->name = "status";
	pp->length = strlen(val) + 1;
	pp->value = val;
	of_update_property(np, pp);
}

static void rg55g1_force_disabled(const char *path)
{
	struct device_node *np = of_find_node_by_path(path);

	if (!np)
		return;
	rg55g1_force_disabled_node(np);
	pr_debug("rg55g1: force-disabled %s\n", path);
	of_node_put(np);
}

static int __init rg55g1_bringup_sanitize_dt(void)
{
	static const char *const paths[] = {
		"/soc/ssusb@a600000",
		"/soc/hsphy@88e3000",
		"/soc/ssphy@88e8000",
		"/soc/kgsl-smmu@3da0000",
		"/soc/apps-smmu@15000000",
		"/soc/qcom,scmi",
		"/soc/interrupt-controller@b220000",
		"/soc/qcom,cpufreq-hw",
		"/soc/slim@3340000",
		"/soc/qcom_scm",
		/* rpmh-rsc: disp EINVAL (-22); apps defers on cmd-db — skip both */
		"/soc/rsc@af20000",
		"/soc/rsc@17a00000",
		"/reserved-memory/ramoops_region",
	};
	struct device_node *soc, *child;
	bool past_smem = false;
	int i, n = 0;

	pr_debug("rg55g1: sanitizing DT before of_platform\n");

	for (i = 0; i < ARRAY_SIZE(paths); i++)
		rg55g1_force_disabled(paths[i]);

	/*
	 * Stock dtbo re-enables SDHCI/UFS/SPMI/I2C/etc. Nuke every /soc
	 * sibling after qcom,smem so bring-up can finish initcalls.
	 */
	soc = of_find_node_by_path("/soc");
	if (!soc) {
		pr_err("rg55g1: /soc not found\n");
		return 0;
	}

	for_each_child_of_node(soc, child) {
		const char *name = of_node_full_name(child);

		if (name && strstr(name, "qcom,smem")) {
			past_smem = true;
			continue;
		}
		if (!past_smem)
			continue;

		if (!of_device_is_available(child))
			continue;

		rg55g1_force_disabled_node(child);
		n++;
	}
	of_node_put(soc);

	pr_debug("rg55g1: disabled %d enabled post-smem /soc children\n", n);
	return 0;
}
early_initcall(rg55g1_bringup_sanitize_dt);
