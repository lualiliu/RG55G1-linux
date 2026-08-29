// SPDX-License-Identifier: GPL-2.0
/*
 * RG55G1 MSM display bring-up: register skipped LV6 clock/DRM drivers and
 * populate MDSS → DPU → DSI → panel when of_platform_populate is blocked.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem.h>
#include <drm/drm_mipi_dsi.h>

#if IS_ENABLED(CONFIG_DRM_MSM)
#include "../../gpu/drm/msm/msm_drv.h"
#endif

#include "rg55g1_bringup.h"
#include "rg55g1_splash.h"

extern void driver_deferred_probe_trigger(void);
extern void rg55g1_dump_deferred_pending(void);
extern bool rg55g1_block_deferred;
extern bool rg55g1_preserve_abl_display;
extern bool rg55g1_abl_panel_ready;
extern bool rg55g1_skip_mdss_populate;

#define RG55G1_DISPCC_PHYS	0xaf00000UL
#define RG55G1_DISPCC_SIZE	0x20000UL
#define RG55G1_MDSS_CORE_BCR	0x8000
#define RG55G1_MDSS_CORE_GDSC	0x9000

/* ABL leaves MDSS powered; don't let dispcc probe power-cycle it. */
static void rg55g1_preserve_display_hw(void)
{
	void __iomem *base;
	u32 gdscr, bcr;

	base = ioremap(RG55G1_DISPCC_PHYS, RG55G1_DISPCC_SIZE);
	if (!base)
		return;

	gdscr = readl(base + RG55G1_MDSS_CORE_GDSC);
	if (!(gdscr & BIT(31)))
		writel(gdscr | BIT(31), base + RG55G1_MDSS_CORE_GDSC);

	bcr = readl(base + RG55G1_MDSS_CORE_BCR);
	if (bcr & BIT(0))
		writel(bcr & ~BIT(0), base + RG55G1_MDSS_CORE_BCR);

	iounmap(base);
}

static void rg55g1_splash_resync(void)
{
	if (rg55g1_splash_info && rg55g1_splash_info->fbops &&
	    rg55g1_splash_info->fbops->fb_sync)
		rg55g1_splash_info->fbops->fb_sync(rg55g1_splash_info);
}

#define rg55_msm_step(s) do { \
	rg55g1_status((s), 0x00ff8000); \
	pr_emerg("rg55g1: msm display: %s\n", (s)); \
} while (0)

static struct device_node *rg55g1_find_dispcc_node(void);
static struct device_node *rg55g1_find_mdss_node(void);

static struct device_node *rg55g1_find_dispcc_node(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "qcom,sm4450-dispcc");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,ravelin-dispcc");
	return np;
}

static struct device_node *rg55g1_find_mdss_node(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "qcom,sm4450-mdss");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,ravelin-mdss");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,mdss");
	return np;
}

static void rg55g1_enable_mdss_tree(struct device_node *mdss_np)
{
	struct device_node *child, *grand;

	if (!mdss_np)
		return;

	rg55g1_force_node_okay(mdss_np);
	for_each_child_of_node(mdss_np, child) {
		rg55g1_force_node_okay(child);
		for_each_child_of_node(child, grand)
			rg55g1_force_node_okay(grand);
	}
}

static int rg55g1_create_platform_dev(struct device_node *np)
{
	struct platform_device *pdev;

	if (!np || !of_device_is_available(np))
		return -ENODEV;

	pdev = of_platform_device_create(np, NULL, NULL);
	if (pdev)
		return 0;

	pdev = of_find_device_by_node(np);
	if (pdev) {
		put_device(&pdev->dev);
		return 0;
	}

	return -ENOMEM;
}

static void rg55g1_msm_unblock_deferred_probe(void)
{
	bool saved = rg55g1_block_deferred;

	rg55g1_block_deferred = false;
	driver_deferred_probe_trigger();
	msleep(100);
	driver_deferred_probe_trigger();
	rg55g1_block_deferred = saved;
}

static int rg55g1_ensure_gcc(void)
{
	struct device_node *np;
	int ret = 0;

	rg55_msm_step("MSM-GCC");

	np = of_find_compatible_node(NULL, NULL, "qcom,sm4450-gcc");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,ravelin-gcc");
	if (!np)
		return -ENODEV;

	rg55g1_force_node_okay(np);
	ret = rg55g1_create_platform_dev(np);
	of_node_put(np);
	if (ret)
		return ret;

	msleep(50);
	rg55g1_msm_unblock_deferred_probe();
	return 0;
}

static int rg55g1_ensure_dispcc(void)
{
	struct device_node *np;
	int ret;

	rg55_msm_step("MSM-DISPCC-REG");

#if IS_ENABLED(CONFIG_SM_DISPCC_4450)
	ret = rg55g1_dispcc_driver_register();
	if (ret) {
		pr_emerg("rg55g1: dispcc driver register failed: %d\n", ret);
		return ret;
	}
#endif

	/*
	 * Briefly keep ABL GDSC/BCR while DISPCC first attaches; quiesce in
	 * dispcc probe stops INTF timing and clears these flags for modeset.
	 */
	rg55g1_preserve_abl_display = true;
	rg55g1_preserve_display_hw();

	np = rg55g1_find_dispcc_node();
	if (!np) {
		rg55g1_status("DISPCC-NO", 0x00ff0000);
		return -ENODEV;
	}

	rg55g1_force_node_okay(np);
	rg55_msm_step("MSM-DISPCC-PROBE");
	ret = rg55g1_create_platform_dev(np);
	of_node_put(np);
	if (ret) {
		rg55g1_status("DISPCC-FAIL", 0x00ff0000);
		return ret;
	}

	msleep(100);
	rg55g1_msm_unblock_deferred_probe();
	/* Keep ABL splash until KMS is ready — quiesce happens on KMS-OK. */
	rg55g1_status("DISPCC-OK", 0x0000ff00);
	pr_emerg("rg55g1: dispcc probe done (ABL splash still active)\n");
	return 0;
}

static bool rg55g1_pdev_bound(struct device_node *np)
{
	struct platform_device *pdev;
	bool bound;

	if (!np)
		return false;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return false;

	bound = !!pdev->dev.driver;
	put_device(&pdev->dev);
	return bound;
}

static bool rg55g1_mdss_bound(struct device_node *mdss_np)
{
	return rg55g1_pdev_bound(mdss_np);
}

static bool rg55g1_dispcc_bound(void)
{
	struct device_node *np;
	bool bound;

	np = rg55g1_find_dispcc_node();
	if (!np)
		return false;

	bound = rg55g1_pdev_bound(np);
	of_node_put(np);
	return bound;
}

static int rg55g1_ensure_smem(void)
{
	struct device_node *np;
	int ret;

	if (qcom_smem_is_available())
		return 0;

	rg55_msm_step("MSM-SMEM");

	np = of_find_node_by_path("/soc/hwlock");
	if (np) {
		rg55g1_force_node_okay(np);
		ret = rg55g1_create_platform_dev(np);
		of_node_put(np);
		if (ret)
			pr_emerg("rg55g1: hwlock create failed: %d\n", ret);
	}

	np = of_find_node_by_path("/soc/qcom,smem");
	if (!np)
		return -ENODEV;

	rg55g1_force_node_okay(np);
	ret = rg55g1_create_platform_dev(np);
	of_node_put(np);
	if (ret)
		return ret;

	msleep(50);
	rg55g1_msm_unblock_deferred_probe();
	msleep(50);

	if (!qcom_smem_is_available()) {
		pr_emerg("rg55g1: SMEM still unavailable after populate\n");
		return -EPROBE_DEFER;
	}

	pr_emerg("rg55g1: SMEM ready for UBWC/MDSS\n");
	return 0;
}

static int rg55g1_ensure_msm_kms(void)
{
	int ret = 0;

#if IS_BUILTIN(CONFIG_DRM_MSM)
	ret = rg55g1_msm_kms_register();
	if (ret)
		return ret;
#elif IS_MODULE(CONFIG_DRM_MSM)
	/*
	 * request_module() at fs_initcall is too early (no module loader /
	 * msm.ko in initramfs). RG55G1 must build CONFIG_DRM_MSM=y.
	 */
	pr_emerg("rg55g1: CONFIG_DRM_MSM=m — rebuild with CONFIG_DRM_MSM=y for MSM populate\n");
	return -ENODEV;
#else
	return -ENODEV;
#endif

	/*
	 * Panel uses module_init/device_initcall which RG55G1 skips. Without
	 * panel attach, msm_dsi never component_add() and KMS never binds.
	 */
#if IS_ENABLED(CONFIG_DRM_PANEL_FOCALTECH_FT7131M)
	ret = rg55g1_ft7131m_driver_register();
	if (ret)
		pr_emerg("rg55g1: FT7131M panel driver register failed: %d\n", ret);
	else
		pr_emerg("rg55g1: FT7131M panel driver registered\n");
#endif
	return ret;
}

static const char * const rg55g1_mdss_child_compat[] = {
	"qcom,sm4450-dsi-phy-4nm",
	"qcom,sm4450-dpu",
	"qcom,sm4450-dsi-ctrl",
};

static int rg55g1_attach_mdss_child(struct platform_device *mdss,
				    struct device_node *child)
{
	struct platform_device *pdev;
	int ret;

	if (!child || !of_device_is_available(child))
		return -ENODEV;

	rg55g1_force_node_okay(child);

	pdev = of_find_device_by_node(child);
	if (!pdev) {
		pdev = of_platform_device_create(child, NULL, &mdss->dev);
		if (!pdev)
			return -ENOMEM;
	}

	if (pdev->dev.driver) {
		put_device(&pdev->dev);
		return 0;
	}

	ret = device_attach(&pdev->dev);
	if (!ret && !pdev->dev.driver) {
		pr_emerg("rg55g1: attach %pOF: no platform driver (MSM KMS?)\n",
			 child);
		ret = -ENODEV;
	} else if (ret) {
		pr_emerg("rg55g1: attach %pOF -> %d\n", child, ret);
	}
	put_device(&pdev->dev);
	return ret;
}

static void rg55g1_msm_bringup_children(struct device_node *mdss_np)
{
	struct platform_device *mdss;
	size_t i;
	int ret;

	mdss = of_find_device_by_node(mdss_np);
	if (!mdss)
		return;

	for (i = 0; i < ARRAY_SIZE(rg55g1_mdss_child_compat); i++) {
		struct device_node *child;

		child = of_get_compatible_child(mdss_np, rg55g1_mdss_child_compat[i]);
		if (!child)
			continue;

		ret = rg55g1_attach_mdss_child(mdss, child);
		if (ret)
			pr_emerg("rg55g1: attach %pOF -> %d\n", child, ret);
		of_node_put(child);
	}

	put_device(&mdss->dev);
}

#if IS_ENABLED(CONFIG_DRM_MSM)
static bool rg55g1_msm_drm_registered(struct device_node *mdss_np)
{
	struct device_node *dpu_np;
	struct platform_device *pdev;
	struct msm_drm_private *priv;
	bool registered = false;

	dpu_np = of_get_compatible_child(mdss_np, "qcom,sm4450-dpu");
	if (!dpu_np)
		dpu_np = of_get_compatible_child(mdss_np, "qcom,ravelin-dpu");
	if (!dpu_np)
		return false;

	pdev = of_find_device_by_node(dpu_np);
	of_node_put(dpu_np);
	if (!pdev)
		return false;

	priv = platform_get_drvdata(pdev);
	if (priv && priv->dev && priv->dev->registered)
		registered = true;

	put_device(&pdev->dev);
	return registered;
}
#else
static bool rg55g1_msm_drm_registered(struct device_node *mdss_np)
{
	return false;
}
#endif

static bool rg55g1_phy_clk_provider_ready(struct device_node *mdss_np)
{
	struct device_node *phy_np;
	struct platform_device *phy;
	bool ready = false;

	phy_np = of_get_compatible_child(mdss_np, "qcom,sm4450-dsi-phy-4nm");
	if (!phy_np)
		return false;

	phy = of_find_device_by_node(phy_np);
	of_node_put(phy_np);
	if (!phy)
		return false;

	if (phy->dev.driver)
		ready = true;

	put_device(&phy->dev);
	return ready;
}

static int rg55g1_ensure_mdss_phy(struct device_node *mdss_np)
{
	struct platform_device *mdss;
	struct device_node *phy_np;
	int ret = -EPROBE_DEFER;
	int i;

	if (!mdss_np)
		return -ENODEV;

	if (rg55g1_phy_clk_provider_ready(mdss_np))
		return 0;

	if (!rg55g1_dispcc_bound()) {
		pr_emerg("rg55g1: DSI PHY waiting for DISPCC\n");
		return -EPROBE_DEFER;
	}

	rg55_msm_step("MSM-PHY");

	rg55g1_skip_mdss_reset = true;
	rg55g1_skip_mdss_populate = true;

	if (!rg55g1_mdss_bound(mdss_np)) {
		ret = rg55g1_create_platform_dev(mdss_np);
		if (ret)
			return ret;
	}

	mdss = of_find_device_by_node(mdss_np);
	if (!mdss)
		return -ENODEV;

	phy_np = of_get_compatible_child(mdss_np, "qcom,sm4450-dsi-phy-4nm");
	if (!phy_np) {
		put_device(&mdss->dev);
		return -ENODEV;
	}

	for (i = 0; i < 30; i++) {
		ret = rg55g1_attach_mdss_child(mdss, phy_np);
		if (!ret && rg55g1_phy_clk_provider_ready(mdss_np)) {
			pr_emerg("rg55g1: DSI PHY clk provider ready\n");
			of_node_put(phy_np);
			put_device(&mdss->dev);
			return 0;
		}
		rg55g1_msm_unblock_deferred_probe();
		msleep(100);
	}

	of_node_put(phy_np);
	put_device(&mdss->dev);
	pr_emerg("rg55g1: DSI PHY not ready (attach=%d)\n", ret);
	rg55g1_dump_deferred_pending();
	return ret ? ret : -EPROBE_DEFER;
}

static void rg55g1_dump_mdss_children(struct device_node *mdss_np)
{
	size_t i;
	struct device_node *dsi_np, *panel_np;
	struct platform_device *pdev;
	struct device *panel_dev;

	for (i = 0; i < ARRAY_SIZE(rg55g1_mdss_child_compat); i++) {
		struct device_node *child;

		child = of_get_compatible_child(mdss_np, rg55g1_mdss_child_compat[i]);
		if (!child)
			continue;
		pdev = of_find_device_by_node(child);
		if (!pdev)
			pr_emerg("rg55g1: child %s: no pdev\n",
				 rg55g1_mdss_child_compat[i]);
		else {
			pr_emerg("rg55g1: child %s: driver=%s\n",
				 rg55g1_mdss_child_compat[i],
				 pdev->dev.driver ? pdev->dev.driver->name : "(none)");
			put_device(&pdev->dev);
		}
		of_node_put(child);
	}

	dsi_np = of_get_compatible_child(mdss_np, "qcom,sm4450-dsi-ctrl");
	if (!dsi_np)
		return;
	panel_np = of_get_compatible_child(dsi_np, "focaltech,ft7131m");
	of_node_put(dsi_np);
	if (!panel_np) {
		pr_emerg("rg55g1: FT7131M panel DT node missing\n");
		return;
	}
	panel_dev = bus_find_device_by_of_node(&mipi_dsi_bus_type, panel_np);
	of_node_put(panel_np);
	if (!panel_dev)
		pr_emerg("rg55g1: FT7131M panel: no mipi_dsi device\n");
	else {
		pr_emerg("rg55g1: FT7131M panel: driver=%s\n",
			 panel_dev->driver ? panel_dev->driver->name : "(none)");
		put_device(panel_dev);
	}
}

static int rg55g1_msm_wait_drm(struct device_node *mdss_np)
{
	int wait;

	rg55_msm_step("MSM-KMS-WAIT");
	for (wait = 0; wait < 40; wait++) {
		if (wait == 0 || wait == 10 || wait == 20 || wait == 30)
			rg55g1_msm_bringup_children(mdss_np);
		rg55g1_msm_unblock_deferred_probe();
		msleep(200);
		rg55g1_splash_resync();

		if (rg55g1_msm_drm_registered(mdss_np)) {
			rg55g1_status("KMS-OK", 0x0000ff00);
			pr_emerg("rg55g1: MSM KMS registered (/dev/dri/card msm)\n");
			return 0;
		}
	}

	rg55g1_status("KMS-DEFER", 0x00ff8000);
	pr_emerg("rg55g1: MSM KMS not registered yet\n");
	rg55g1_dump_mdss_children(mdss_np);
	rg55g1_dump_deferred_pending();
	rg55g1_splash_resync();
	return -EPROBE_DEFER;
}

static int rg55g1_msm_display_proceed(void)
{
	struct device_node *mdss_np;
	struct platform_device *pdev;
	int ret, wait;

#if IS_ENABLED(CONFIG_DRM_MSM)
	rg55_msm_step("MSM-KMS-REG");
	ret = rg55g1_ensure_msm_kms();
	if (ret)
		return ret;
#endif

	mdss_np = rg55g1_find_mdss_node();
	if (!mdss_np) {
		rg55g1_status("MDSS-NO", 0x00ff0000);
		pr_emerg("rg55g1: no MDSS DT node\n");
		return -ENODEV;
	}

	rg55_msm_step("MSM-MDSS-EN");
	rg55g1_enable_mdss_tree(mdss_np);
	pr_emerg("rg55g1: mdss DT tree enabled\n");

	/* ABL scanout must survive MDSS platform probe (no reset/populate). */
	rg55g1_skip_mdss_reset = true;
	rg55g1_skip_mdss_populate = true;

	rg55_msm_step("MSM-MDSS-CREATE");
	if (rg55g1_create_platform_dev(mdss_np)) {
		rg55g1_skip_mdss_reset = false;
		rg55g1_skip_mdss_populate = false;
		of_node_put(mdss_np);
		rg55g1_status("MDSS-FAIL", 0x00ff0000);
		return -ENOMEM;
	}
	pr_emerg("rg55g1: mdss platform device created\n");

	rg55_msm_step("MSM-MDSS-WAIT");

	pdev = of_find_device_by_node(mdss_np);
	if (pdev && !pdev->dev.driver)
		(void)device_attach(&pdev->dev);

	for (wait = 0; wait < 50; wait++) {
		rg55g1_msm_unblock_deferred_probe();
		msleep(200);

		if (pdev && pdev->dev.driver)
			break;
		if (!pdev) {
			pdev = of_find_device_by_node(mdss_np);
			if (pdev && !pdev->dev.driver)
				(void)device_attach(&pdev->dev);
		} else if (!pdev->dev.driver) {
			(void)device_attach(&pdev->dev);
		}

		if (pdev && pdev->dev.driver)
			break;
	}

	if (pdev && pdev->dev.driver) {
		rg55g1_status("MSM-OK", 0x0000ff00);
		pr_emerg("rg55g1: MSM MDSS bound driver=%s\n",
			 pdev->dev.driver->name);
		put_device(&pdev->dev);

		ret = rg55g1_msm_wait_drm(mdss_np);
		if (ret)
			rg55g1_splash_resync();
		of_node_put(mdss_np);
		return ret;
	}
	if (pdev)
		put_device(&pdev->dev);

	if (rg55g1_mdss_bound(mdss_np)) {
		ret = rg55g1_msm_wait_drm(mdss_np);
		if (ret)
			rg55g1_splash_resync();
		of_node_put(mdss_np);
		return ret;
	}

	rg55g1_status("MSM-DEFER", 0x00ff8000);
	pr_emerg("rg55g1: MDSS created but not bound yet\n");
	rg55g1_dump_deferred_pending();
	of_node_put(mdss_np);
	/* Keep ABL splash until MSM binds — do not clear preserve_abl_display. */
	rg55g1_splash_resync();
	return -EPROBE_DEFER;
}

int rg55g1_msm_display_retry(void)
{
	struct device_node *mdss_np;
	int ret;

#if !IS_ENABLED(CONFIG_DRM_MSM_MDSS)
	return -ENODEV;
#endif

	rg55_msm_step("MSM-RETRY");

	mdss_np = rg55g1_find_mdss_node();
	if (mdss_np && rg55g1_msm_drm_registered(mdss_np)) {
		of_node_put(mdss_np);
		return 0;
	}
	of_node_put(mdss_np);

	ret = rg55g1_ensure_smem();
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_DRM_MSM)
	rg55_msm_step("MSM-KMS-REG");
	ret = rg55g1_ensure_msm_kms();
	if (ret) {
		pr_emerg("rg55g1: MSM KMS driver register failed: %d\n", ret);
		return ret;
	}
#endif

	mdss_np = rg55g1_find_mdss_node();
	if (!mdss_np)
		return -ENODEV;

	rg55g1_enable_mdss_tree(mdss_np);

	ret = rg55g1_ensure_dispcc();
	if (ret) {
		of_node_put(mdss_np);
		return ret;
	}

	ret = rg55g1_ensure_mdss_phy(mdss_np);
	if (ret) {
		of_node_put(mdss_np);
		return ret;
	}

	ret = rg55g1_msm_display_proceed();
	of_node_put(mdss_np);
	return ret;
}

int __init rg55g1_populate_msm_display(void)
{
	int ret;

#if !IS_ENABLED(CONFIG_DRM_MSM_MDSS)
	return -ENODEV;
#endif

	rg55g1_status("MSM-BEGIN", 0x00ff8000);
	pr_emerg("rg55g1: msm display populate begin\n");

	ret = rg55g1_ensure_gcc();
	if (ret)
		pr_emerg("rg55g1: GCC not ready for display (%d)\n", ret);

	ret = rg55g1_ensure_smem();
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_DRM_MSM)
	rg55_msm_step("MSM-KMS-REG");
	ret = rg55g1_ensure_msm_kms();
	if (ret) {
		pr_emerg("rg55g1: MSM KMS driver register failed: %d\n", ret);
		return ret;
	}
#endif

	{
		struct device_node *mdss_np = rg55g1_find_mdss_node();

		if (!mdss_np)
			return -ENODEV;
		rg55g1_enable_mdss_tree(mdss_np);
		of_node_put(mdss_np);
	}

	ret = rg55g1_ensure_dispcc();
	if (ret)
		return ret;

	{
		struct device_node *mdss_np = rg55g1_find_mdss_node();

		if (!mdss_np)
			return -ENODEV;
		ret = rg55g1_ensure_mdss_phy(mdss_np);
		of_node_put(mdss_np);
		if (ret)
			return ret;
	}

	ret = rg55g1_msm_display_proceed();
	if (ret) {
		rg55g1_status("MSM-REG", 0x00ff0000);
		pr_emerg("rg55g1: msm kms register failed: %d\n", ret);
	}
	return ret;
}

/*
 * Full LV6 path does not run USB bringup's MSM retry. After devices settle,
 * perform controlled display handoff (mdss/dispcc stay disabled in DT until
 * ensure_* force-enables them).
 */
static int __init rg55g1_msm_late_bringup(void)
{
	int ret;

	/* Opt-in only: default cmdline leaves msm= off so simpledrm owns fb. */
	if (!rg55g1_msm_auto && !rg55g1_msm_early)
		return 0;

	rg55g1_status("MSM-LATE", 0x00ff8000);
	ret = rg55g1_msm_display_retry();
	if (!ret)
		pr_emerg("rg55g1: MSM display probe done (late; splash kept until modeset)\n");
	else if (ret != -ENODEV)
		pr_emerg("rg55g1: MSM late bringup: %d (splash kept)\n", ret);
	rg55g1_splash_resync();
	return 0;
}
late_initcall_sync(rg55g1_msm_late_bringup);
