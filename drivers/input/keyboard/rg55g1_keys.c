// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RG55G1 power / volume keys (stock ravelin-pmic-overlay mapping).
 *
 *   KEY_POWER      — PMK8350 PON gen3 KPDPWR (SID0 @ 0x1300)
 *   KEY_VOLUMEDOWN — PMK8350 PON gen3 RESIN
 *   KEY_VOLUMEUP   — TLMM gpio53 (ACTIVE_LOW)
 *
 * Stock uses qcom,pmk8350-pwrkey / resin + gpio-keys. On this bring-up
 * TLMM gpiochip panics and SPMI IRQ path is unreliable, so one polled
 * input device reads TLMM MMIO + SPMI debug arb instead.
 */
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spmi.h>
#include <dt-bindings/gpio/gpio.h>

#define DRV_NAME		"rg55g1-keys"

#define RG55G1_TLMM_PHYS	0x0f100000UL
#define RG55G1_TLMM_SIZE	0x00300000UL
#define TLMM_REG_SIZE		0x1000
#define TLMM_CTL_OE		BIT(9)
#define TLMM_CTL_MUX_MASK	(0x7 << 2)
#define TLMM_CTL_PULL_MASK	0x3
#define TLMM_CTL_PULL_UP	0x3
#define TLMM_IO_IN		BIT(0)

#define PON_RT_STS		0x10
#define PON_GEN3_RESIN_N_SET	BIT(6)
#define PON_GEN3_KPDPWR_N_SET	BIT(7)

#define DEFAULT_POLL_MS		20
#define DEFAULT_VOL_UP_GPIO	53
#define DEFAULT_PON_SID		0
#define DEFAULT_PON_BASE	0x1300

struct rg55g1_keys {
	struct device *dev;
	struct input_dev *input;
	void __iomem *tlmm;
	struct spmi_controller *spmi;
	int vol_up_gpio;
	bool vol_up_active_low;
	u8 pon_sid;
	u16 pon_base;
	int last_power;
	int last_voldown;
	int last_volup;
	bool spmi_ok;
	bool tlmm_ok;
};

struct rg55_spmi_match {
	struct spmi_controller *ctrl;
	bool want_debug;
};

static int rg55g1_keys_match_spmi(struct device *dev, void *data)
{
	struct rg55_spmi_match *m = data;
	struct device_node *np;

	if (strncmp(dev_name(dev), "spmi-", 5) != 0)
		return 0;

	np = of_node_get(dev->parent ? dev->parent->of_node : NULL);
	if (m->want_debug) {
		if (!np ||
		    !of_device_is_compatible(np, "qcom,spmi-pmic-arb-debug")) {
			of_node_put(np);
			return 0;
		}
	} else if (np &&
		   of_device_is_compatible(np, "qcom,spmi-pmic-arb-debug")) {
		of_node_put(np);
		return 0;
	}
	of_node_put(np);
	m->ctrl = to_spmi_controller(dev);
	return 1;
}

static struct spmi_controller *rg55g1_keys_get_spmi(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct rg55_spmi_match m = { .want_debug = true };

	np = of_find_compatible_node(NULL, NULL, "qcom,spmi-pmic-arb-debug");
	if (!np) {
		m.want_debug = false;
		np = of_find_compatible_node(NULL, NULL, "qcom,spmi-pmic-arb");
	}
	if (!np)
		return NULL;

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;

	device_for_each_child(&pdev->dev, &m, rg55g1_keys_match_spmi);
	put_device(&pdev->dev);
	return m.ctrl;
}

static int rg55g1_keys_sid_read(struct spmi_controller *ctrl, u8 sid, u16 addr,
				u8 *val)
{
	struct spmi_device *sdev;
	int ret;

	sdev = spmi_device_alloc(ctrl);
	if (!sdev)
		return -ENOMEM;
	sdev->usid = sid;
	ret = spmi_ext_register_readl(sdev, addr, val, 1);
	put_device(&sdev->dev);
	return ret;
}

static void rg55g1_keys_tlmm_cfg_input(struct rg55g1_keys *keys, int gpio)
{
	u32 ctl;

	if (!keys->tlmm || gpio < 0)
		return;

	ctl = readl_relaxed(keys->tlmm + (unsigned long)gpio * TLMM_REG_SIZE);
	ctl &= ~(TLMM_CTL_MUX_MASK | TLMM_CTL_PULL_MASK | TLMM_CTL_OE);
	ctl |= TLMM_CTL_PULL_UP;
	writel_relaxed(ctl, keys->tlmm + (unsigned long)gpio * TLMM_REG_SIZE);
}

static int rg55g1_keys_tlmm_get(struct rg55g1_keys *keys, int gpio)
{
	u32 io;

	if (!keys->tlmm || gpio < 0)
		return -ENODEV;

	io = readl_relaxed(keys->tlmm + (unsigned long)gpio * TLMM_REG_SIZE + 0x4);
	return !!(io & TLMM_IO_IN);
}

static void rg55g1_keys_poll(struct input_dev *input)
{
	struct rg55g1_keys *keys = input_get_drvdata(input);
	bool changed = false;
	u8 sts;
	int raw, pressed;

	if (keys->spmi_ok && keys->spmi) {
		if (!rg55g1_keys_sid_read(keys->spmi, keys->pon_sid,
					  keys->pon_base + PON_RT_STS, &sts)) {
			pressed = !!(sts & PON_GEN3_KPDPWR_N_SET);
			if (pressed != keys->last_power) {
				input_report_key(input, KEY_POWER, pressed);
				keys->last_power = pressed;
				changed = true;
			}

			pressed = !!(sts & PON_GEN3_RESIN_N_SET);
			if (pressed != keys->last_voldown) {
				input_report_key(input, KEY_VOLUMEDOWN, pressed);
				keys->last_voldown = pressed;
				changed = true;
			}
		}
	}

	if (keys->tlmm_ok) {
		raw = rg55g1_keys_tlmm_get(keys, keys->vol_up_gpio);
		if (raw >= 0) {
			pressed = keys->vol_up_active_low ? !raw : raw;
			if (pressed != keys->last_volup) {
				input_report_key(input, KEY_VOLUMEUP, pressed);
				keys->last_volup = pressed;
				changed = true;
			}
		}
	}

	if (changed)
		input_sync(input);
}

static int rg55g1_keys_parse_vol_up(struct rg55g1_keys *keys)
{
	struct device_node *np = keys->dev->of_node;
	struct of_phandle_args args;
	u32 gpio = DEFAULT_VOL_UP_GPIO;
	int ret;

	keys->vol_up_active_low = true;

	ret = of_parse_phandle_with_args(np, "vol-up-gpios", "#gpio-cells", 0,
					 &args);
	if (!ret) {
		keys->vol_up_gpio = args.args[0];
		keys->vol_up_active_low = !!(args.args[1] & GPIO_ACTIVE_LOW);
		of_node_put(args.np);
	} else if (!of_property_read_u32(np, "vol-up-gpio", &gpio)) {
		keys->vol_up_gpio = gpio;
	} else {
		keys->vol_up_gpio = DEFAULT_VOL_UP_GPIO;
	}

	return 0;
}

static int rg55g1_keys_probe(struct platform_device *pdev)
{
	struct rg55g1_keys *keys;
	struct input_dev *input;
	u32 val;
	int error;

	keys = devm_kzalloc(&pdev->dev, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;

	keys->dev = &pdev->dev;
	keys->last_power = -1;
	keys->last_voldown = -1;
	keys->last_volup = -1;

	if (of_property_read_u32(pdev->dev.of_node, "qcom,spmi-sid", &val))
		keys->pon_sid = DEFAULT_PON_SID;
	else
		keys->pon_sid = val;

	if (of_property_read_u32(pdev->dev.of_node, "qcom,pon-base", &val))
		keys->pon_base = DEFAULT_PON_BASE;
	else
		keys->pon_base = val;

	rg55g1_keys_parse_vol_up(keys);

	keys->tlmm = ioremap(RG55G1_TLMM_PHYS, RG55G1_TLMM_SIZE);
	if (keys->tlmm) {
		rg55g1_keys_tlmm_cfg_input(keys, keys->vol_up_gpio);
		keys->tlmm_ok = true;
	} else {
		dev_warn(&pdev->dev, "TLMM ioremap failed; no VOLUMEUP\n");
	}

	keys->spmi = rg55g1_keys_get_spmi();
	if (keys->spmi) {
		u8 sts;

		if (!rg55g1_keys_sid_read(keys->spmi, keys->pon_sid,
					  keys->pon_base + PON_RT_STS, &sts)) {
			keys->spmi_ok = true;
			dev_info(&pdev->dev,
				 "PON@sid%u/0x%x RT_STS=0x%02x\n",
				 keys->pon_sid, keys->pon_base, sts);
		} else {
			dev_warn(&pdev->dev,
				 "PON RT_STS read failed; no POWER/VOLUMEDOWN\n");
		}
	} else {
		dev_warn(&pdev->dev, "no SPMI controller; no POWER/VOLUMEDOWN\n");
	}

	if (!keys->tlmm_ok && !keys->spmi_ok)
		return -ENODEV;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	keys->input = input;
	input_set_drvdata(input, keys);

	input->name = "rg55g1-keys";
	input->phys = "rg55g1-keys/input0";
	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x1;
	input->id.product = 0x1;
	input->id.version = 0x100;

	if (keys->spmi_ok) {
		input_set_capability(input, EV_KEY, KEY_POWER);
		input_set_capability(input, EV_KEY, KEY_VOLUMEDOWN);
	}
	if (keys->tlmm_ok)
		input_set_capability(input, EV_KEY, KEY_VOLUMEUP);

	error = input_setup_polling(input, rg55g1_keys_poll);
	if (error)
		return error;

	if (of_property_read_u32(pdev->dev.of_node, "poll-interval", &val))
		val = DEFAULT_POLL_MS;
	input_set_poll_interval(input, val);

	error = input_register_device(input);
	if (error)
		return error;

	platform_set_drvdata(pdev, keys);
	pr_emerg("rg55g1-keys: registered (vol-up gpio%d%s, pon sid%u@0x%x%s)\n",
		 keys->vol_up_gpio, keys->tlmm_ok ? "" : " off",
		 keys->pon_sid, keys->pon_base, keys->spmi_ok ? "" : " off");
	return 0;
}

static void rg55g1_keys_remove(struct platform_device *pdev)
{
	struct rg55g1_keys *keys = platform_get_drvdata(pdev);

	if (keys && keys->tlmm)
		iounmap(keys->tlmm);
}

static const struct of_device_id rg55g1_keys_of_match[] = {
	{ .compatible = "anbernic,rg55g1-keys" },
	{ }
};
MODULE_DEVICE_TABLE(of, rg55g1_keys_of_match);

static struct platform_driver rg55g1_keys_driver = {
	.probe = rg55g1_keys_probe,
	.remove = rg55g1_keys_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = rg55g1_keys_of_match,
	},
};

static int __init rg55g1_keys_driver_init(void)
{
	return platform_driver_register(&rg55g1_keys_driver);
}

static void __exit rg55g1_keys_driver_exit(void)
{
	platform_driver_unregister(&rg55g1_keys_driver);
}

/*
 * RG55G1 skips subsys/device initcalls (LV4+). Register at arch so the
 * driver exists before rg55g1_bringup_usb() creates the platform device.
 */
arch_initcall(rg55g1_keys_driver_init);
module_exit(rg55g1_keys_driver_exit);

MODULE_AUTHOR("RG55G1 bringup");
MODULE_DESCRIPTION("RG55G1 power and volume keys");
MODULE_LICENSE("GPL");
