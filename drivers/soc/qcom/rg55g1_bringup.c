// SPDX-License-Identifier: GPL-2.0-only
/*
 * RG55G1 bring-up: reach ash on splash FB, then enable a minimal USB host
 * path so a USB keyboard can feed tty0.
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spmi.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <dt-bindings/clock/qcom,sm4450-gcc.h>
#include <soc/qcom/cmd-db.h>
#include <soc/qcom/rpmh.h>
#include <soc/qcom/tcs.h>

bool rg55g1_skip_of_populate = true;
EXPORT_SYMBOL_GPL(rg55g1_skip_of_populate);

/* When set, PHY may proceed without PMIC regulators (bootloader left USB up). */
bool rg55g1_usb_loose_supplies = true;
EXPORT_SYMBOL_GPL(rg55g1_usb_loose_supplies);

extern bool rg55g1_block_deferred;
extern void rg55g1_status(const char *msg, u32 color);
extern void rg55g1_status_vbus(const char *msg, u32 color);
extern void driver_deferred_probe_trigger(void);

static void rg55g1_force_status(struct device_node *np, const char *status)
{
	struct property *pp;
	char *val;

	if (!np || !status)
		return;

	val = kstrdup(status, GFP_KERNEL);
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

static void rg55g1_force_disabled_node(struct device_node *np)
{
	if (!np || !of_device_is_available(np))
		return;
	rg55g1_force_status(np, "disabled");
}

static bool rg55g1_is_usb_keep(struct device_node *np)
{
	/* HS-only: skip SS PHY and nop-xceiv (legacy usb-phy / dummy vbus). */
	return of_device_is_compatible(np, "qcom,ravelin-gcc") ||
	       of_device_is_compatible(np, "qcom,sm4450-gcc") ||
	       of_device_is_compatible(np, "qcom,dwc-usb3-msm") ||
	       of_device_is_compatible(np, "qcom,dwc3") ||
	       of_device_is_compatible(np, "qcom,snps-dwc3") ||
	       of_device_is_compatible(np, "snps,dwc3") ||
	       of_device_is_compatible(np, "qcom,usb-hsphy-snps-femto") ||
	       of_device_is_compatible(np, "qcom,usb-snps-femto-v2-phy") ||
	       of_device_is_compatible(np, "qcom,spmi-pmic-arb") ||
	       of_device_is_compatible(np, "qcom,spmi-pmic-arb-debug") ||
	       of_device_is_compatible(np, "qcom,qsmmu-v500") ||
	       of_device_is_compatible(np, "qcom,smmu-500") ||
	       of_device_is_compatible(np, "arm,mmu-500");
}

static int rg55g1_disable_children(const char *path)
{
	struct device_node *parent, *child;
	int n = 0;

	parent = of_find_node_by_path(path);
	if (!parent)
		return 0;

	for_each_child_of_node(parent, child) {
		if (!of_device_is_available(child))
			continue;
		if (rg55g1_is_usb_keep(child))
			continue;
		rg55g1_force_disabled_node(child);
		n++;
	}
	of_node_put(parent);
	return n;
}

static void rg55g1_set_compatible(struct device_node *np, const char *compat)
{
	struct property *pp;
	char *val;

	if (!np || !compat)
		return;

	val = kstrdup(compat, GFP_KERNEL);
	if (!val)
		return;

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(val);
		return;
	}

	pp->name = "compatible";
	pp->length = strlen(val) + 1;
	pp->value = val;
	of_update_property(np, pp);
}

/* Multi-string compatible: "a\0b\0" with length including both NULs. */
static void rg55g1_set_compatible2(struct device_node *np,
				   const char *c0, const char *c1)
{
	struct property *pp;
	char *val;
	size_t l0, l1;

	if (!np || !c0 || !c1)
		return;

	l0 = strlen(c0);
	l1 = strlen(c1);
	val = kmalloc(l0 + 1 + l1 + 1, GFP_KERNEL);
	if (!val)
		return;
	memcpy(val, c0, l0 + 1);
	memcpy(val + l0 + 1, c1, l1 + 1);

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(val);
		return;
	}
	pp->name = "compatible";
	pp->length = l0 + 1 + l1 + 1;
	pp->value = val;
	of_update_property(np, pp);
}

static void rg55g1_set_string_prop(struct device_node *np, const char *name,
				   const char *value)
{
	struct property *pp;
	char *val;

	if (!np || !name || !value)
		return;

	val = kstrdup(value, GFP_KERNEL);
	if (!val)
		return;

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(val);
		return;
	}

	pp->name = kstrdup(name, GFP_KERNEL);
	if (!pp->name) {
		kfree(val);
		kfree(pp);
		return;
	}
	pp->length = strlen(val) + 1;
	pp->value = val;
	of_update_property(np, pp);
}

static void rg55g1_set_phandle_prop(struct device_node *np, const char *name,
				    phandle ph)
{
	struct property *pp;
	__be32 *val;

	if (!np || !name || !ph)
		return;

	val = kmalloc(sizeof(*val), GFP_KERNEL);
	if (!val)
		return;

	*val = cpu_to_be32(ph);

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(val);
		return;
	}

	pp->name = kstrdup(name, GFP_KERNEL);
	if (!pp->name) {
		kfree(val);
		kfree(pp);
		return;
	}
	pp->length = sizeof(*val);
	pp->value = val;
	of_update_property(np, pp);
}

static void rg55g1_remove_prop(struct device_node *np, const char *name)
{
	struct property *pp;

	if (!np || !name)
		return;
	pp = of_find_property(np, name, NULL);
	if (pp)
		of_remove_property(np, pp);
}

static void rg55g1_set_u32_prop(struct device_node *np, const char *name, u32 v)
{
	struct property *pp;
	__be32 *val;

	if (!np || !name)
		return;

	val = kmalloc(sizeof(*val), GFP_KERNEL);
	if (!val)
		return;
	*val = cpu_to_be32(v);

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(val);
		return;
	}
	pp->name = kstrdup(name, GFP_KERNEL);
	if (!pp->name) {
		kfree(val);
		kfree(pp);
		return;
	}
	pp->length = sizeof(*val);
	pp->value = val;
	of_update_property(np, pp);
}

static void rg55g1_patch_dwc3_hs_only(struct device_node *dwc3,
				      struct device_node *hsphy)
{
	if (!dwc3 || !hsphy || !hsphy->phandle)
		return;

	/* Stock hsphy has no #phy-cells; required for of_phy_get. */
	rg55g1_set_u32_prop(hsphy, "#phy-cells", 0);

	rg55g1_set_string_prop(dwc3, "dr_mode", "host");
	rg55g1_set_string_prop(dwc3, "maximum-speed", "high-speed");
	rg55g1_set_string_prop(dwc3, "phy_type", "utmi");
	/* Avoid FREECLK_EXISTS hanging xHCI CMD_RESET on HS-only. */
	rg55g1_set_string_prop(dwc3, "snps,dis-u2-freeclk-exists-quirk", "");
	rg55g1_set_string_prop(dwc3, "snps,dis_enblslpm_quirk", "");
	/* Stock Android quirks — keep HS PHY out of suspend during host. */
	rg55g1_set_string_prop(dwc3, "snps,dis_u2_susphy_quirk", "");
	rg55g1_set_string_prop(dwc3, "snps,dis_u3_susphy_quirk", "");
	rg55g1_set_string_prop(dwc3, "snps,dis-u1-entry-quirk", "");
	rg55g1_set_string_prop(dwc3, "snps,dis-u2-entry-quirk", "");
	/*
	 * Stock usb-phy points at generic-PHY-only hsphy + missing ssphy →
	 * permanent -EPROBE_DEFER in dwc3_core_get_phy(). Use phys instead.
	 */
	rg55g1_remove_prop(dwc3, "usb-phy");
	rg55g1_remove_prop(dwc3, "usb-role-switch");
	/*
	 * Direct phys DMA: rg55g1_bringup_apps_smmu() puts USB SID into
	 * unmatched/bypass. Do not attach to the (unprobed) arm-smmu driver.
	 */
	rg55g1_remove_prop(dwc3, "iommus");
	rg55g1_remove_prop(dwc3, "qcom,iommu-dma");
	rg55g1_remove_prop(dwc3, "qcom,iommu-dma-addr-pool");
	/*
	 * With apps-smmu bypassed, USB master may not be CPU-coherent.
	 * Claiming dma-coherent skips cache maintenance and causes event
	 * TRB / cmd_trb pointer desync ("completion does not match").
	 */
	rg55g1_remove_prop(dwc3, "dma-coherent");
	rg55g1_set_phandle_prop(dwc3, "phys", hsphy->phandle);
	rg55g1_set_string_prop(dwc3, "phy-names", "usb2-phy");
}

static void rg55g1_patch_ssusb_glue(struct device_node *ssusb)
{
	if (!ssusb)
		return;
	/* EUD extcon is disabled → permanent defer + depopulate child. */
	rg55g1_remove_prop(ssusb, "extcon");
	rg55g1_remove_prop(ssusb, "usb-role-switch");
	rg55g1_remove_prop(ssusb, "interconnects");
	rg55g1_remove_prop(ssusb, "interconnect-names");
	/* HS-only: dwc3-qcom must mux UTMI onto PIPE. */
	rg55g1_set_string_prop(ssusb, "qcom,select-utmi-as-pipe-clk", "");
}

/**
 * rg55g1_sanitize_dt() - disable hang-prone DT nodes; keep USB/GCC.
 */
int rg55g1_sanitize_dt(void)
{
	int n = 0;
	struct device_node *rm, *child, *ramo;

	rg55g1_block_deferred = true;
	rg55g1_skip_of_populate = true;

	n += rg55g1_disable_children("/soc");
	n += rg55g1_disable_children("/firmware");

	ramo = of_find_node_by_path("/reserved-memory/ramoops_region");
	if (ramo) {
		rg55g1_force_disabled_node(ramo);
		of_node_put(ramo);
		n++;
	}

	rm = of_find_node_by_path("/reserved-memory");
	if (rm) {
		for_each_child_of_node(rm, child) {
			if (!of_property_present(child, "compatible"))
				continue;
			if (!of_device_is_available(child))
				continue;
			/* Keep cmd-db — HS PHY LDOs need RPMh resource lookup. */
			if (of_device_is_compatible(child, "qcom,cmd-db"))
				continue;
			rg55g1_force_disabled_node(child);
			n++;
		}
		of_node_put(rm);
	}

	pr_emerg("rg55g1: DT sanitize disabled %d nodes (kept USB/GCC)\n", n);
	return n;
}
EXPORT_SYMBOL_GPL(rg55g1_sanitize_dt);

/* PM7250B SMB5 — DCDC @ 0x1100 / TYPEC @ 0x1500 (smb5-reg.h). */
#define RG55_DCDC_BASE			0x1100
#define RG55_DCDC_TYPE			(RG55_DCDC_BASE + 0x04)
#define RG55_DCDC_SUBTYPE		(RG55_DCDC_BASE + 0x05)
#define RG55_POWER_PATH_STATUS		(RG55_DCDC_BASE + 0x0B)
#define RG55_CMD_OTG			(RG55_DCDC_BASE + 0x40)
#define RG55_OTG_ILIMIT			(RG55_DCDC_BASE + 0x52)
#define RG55_OTG_CFG			(RG55_DCDC_BASE + 0x53)
#define RG55_OTG_FAULT_CFG		(RG55_DCDC_BASE + 0x56)
#define RG55_OTG_EN			BIT(0)
#define RG55_OTG_EN_SRC_CFG		BIT(1)
#define RG55_OTG_DBG_MODE		BIT(2)
#define RG55_OTG_ENG_CFG		(RG55_DCDC_BASE + 0xC0)
/* Android sets this before enabling OTG (halt 1-in-8 mode). */
#define RG55_OTG_ENG_HALT		BIT(0)
#define RG55_USB_CMD_IL			0x1340	/* USBIN_CMD_IL */
#define RG55_USB_SUSPEND		BIT(0)

/* Aliases kept for older scan logs that looked for "OTG@0x1100". */
#define RG55_OTG_BASE			RG55_DCDC_BASE
#define RG55_OTG_TYPE			RG55_DCDC_TYPE
#define RG55_OTG_SUBTYPE		RG55_DCDC_SUBTYPE

#define RG55_TYPEC_BASE			0x1500
#define RG55_TYPEC_TYPE			(RG55_TYPEC_BASE + 0x04)
#define RG55_TYPEC_SRC_STATUS		(RG55_TYPEC_BASE + 0x08)
#define RG55_TYPEC_SM_STATUS		(RG55_TYPEC_BASE + 0x09)
#define RG55_TYPEC_MISC_STATUS		(RG55_TYPEC_BASE + 0x0B)
#define RG55_TYPEC_MODE_CFG		(RG55_TYPEC_BASE + 0x44)
#define RG55_TYPEC_VCONN_CTL		(RG55_TYPEC_BASE + 0x46)
#define RG55_TYPEC_DEBUG_SRC		(RG55_TYPEC_BASE + 0x4C)
#define RG55_TYPEC_EXIT_STATE		(RG55_TYPEC_BASE + 0x50)
#define RG55_TYPEC_CURRSRC_CFG		(RG55_TYPEC_BASE + 0x52)
#define RG55_TYPEC_CC_ATTACHED		BIT(0)
#define RG55_TYPEC_SNK_SRC_MODE		BIT(6)	/* 1 = operating as source */
#define RG55_TYPEC_ATTACH_STATE		BIT(5)	/* SM_STATUS */
#define RG55_TYPEC_SM_VSAFE5V		BIT(5)	/* legacy name; unused */
#define RG55_TYPEC_SM_VSAFE0V		BIT(6)
#define RG55_TYPEC_DISABLE_CMD		BIT(0)
#define RG55_TYPEC_EN_SNK_ONLY		BIT(1)
#define RG55_TYPEC_EN_SRC_ONLY		BIT(2)
#define RG55_TYPEC_RP_1P5		0x1	/* TYPEC_SRC_RP_1P5A */
#define RG55_TYPEC_SEL_SRC_UPPER_REF	BIT(2)
#define RG55_TYPEC_VCONN_EN_SRC		BIT(0)
#define RG55_TYPEC_DBG_SRC_EN		BIT(0)
#define RG55_TYPEC_VBUS_DETECT		RG55_TYPEC_SNK_SRC_MODE
#define RG55_TYPEC_SRC_RD_OPEN		BIT(3)

struct rg55_spmi_match {
	struct spmi_controller *ctrl;
	bool want_debug;
};

static int rg55g1_match_spmi_ctrl(struct device *dev, void *data)
{
	struct rg55_spmi_match *m = data;
	struct device_node *np;

	if (strncmp(dev_name(dev), "spmi-", 5) != 0)
		return 0;

	np = of_node_get(dev->parent ? dev->parent->of_node : NULL);
	if (m->want_debug) {
		if (!np || !of_device_is_compatible(np, "qcom,spmi-pmic-arb-debug")) {
			of_node_put(np);
			return 0;
		}
	} else {
		if (np && of_device_is_compatible(np, "qcom,spmi-pmic-arb-debug")) {
			of_node_put(np);
			return 0;
		}
	}
	of_node_put(np);
	m->ctrl = to_spmi_controller(dev);
	return 1;
}

static struct platform_device *rg55g1_ensure_platform(struct device_node *np)
{
	struct platform_device *pdev;

	if (!np)
		return NULL;
	rg55g1_force_status(np, "okay");
	pdev = of_find_device_by_node(np);
	if (!pdev) {
		if (!of_platform_device_create(np, NULL, NULL))
			return NULL;
		driver_deferred_probe_trigger();
		msleep(400);
		pdev = of_find_device_by_node(np);
	} else {
		put_device(&pdev->dev);
		driver_deferred_probe_trigger();
		msleep(150);
		pdev = of_find_device_by_node(np);
	}
	return pdev;
}

static struct spmi_controller *rg55g1_get_spmi(bool debug)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct rg55_spmi_match m = { .want_debug = debug };

	np = of_find_compatible_node(NULL, NULL,
				     debug ? "qcom,spmi-pmic-arb-debug" :
					     "qcom,spmi-pmic-arb");
	if (!np)
		return NULL;

	if (!debug) {
		rg55g1_remove_prop(np, "interrupts-extended");
		rg55g1_remove_prop(np, "interrupt-names");
	} else {
		/* Avoid defer on unresolved stock clock / depends-on. */
		rg55g1_remove_prop(np, "clocks");
		rg55g1_remove_prop(np, "clock-names");
		rg55g1_remove_prop(np, "depends-on-supply");
		/*
		 * Stock fuse gate may false-negative; try the bus anyway —
		 * DENIED/timeout is safer than giving up before a write.
		 */
		rg55g1_remove_prop(np, "qcom,fuse-enable-bit");
		rg55g1_remove_prop(np, "qcom,fuse-disable-bit");
	}

	pdev = rg55g1_ensure_platform(np);
	of_node_put(np);
	if (!pdev)
		return NULL;

	device_for_each_child(&pdev->dev, &m, rg55g1_match_spmi_ctrl);
	put_device(&pdev->dev);
	return m.ctrl;
}

static int rg55g1_sid_read(struct spmi_controller *ctrl, u8 sid, u16 addr,
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

static int rg55g1_sid_write(struct spmi_controller *ctrl, u8 sid, u16 addr,
			    u8 val)
{
	struct spmi_device *sdev;
	int ret;

	sdev = spmi_device_alloc(ctrl);
	if (!sdev)
		return -ENOMEM;
	sdev->usid = sid;
	ret = spmi_ext_register_writel(sdev, addr, &val, 1);
	put_device(&sdev->dev);
	return ret;
}

/* Keep TYPEC in SRC + OTG asserted — DISABLE leaves receptacle at 0V. */
static struct spmi_controller *vbus_keep_ctrl;
static u8 vbus_keep_sid;
static void rg55g1_vbus_keep_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(vbus_keep_work, rg55g1_vbus_keep_fn);

static void rg55g1_vbus_force_otg(struct spmi_controller *ctrl, u8 sid)
{
	u8 eng = 0, il = 0, cfg = 0;

	/* SRC-only + Rp (no debug-accessory wrestling). */
	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_DEBUG_SRC, 0);
	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_CURRSRC_CFG,
			       RG55_TYPEC_RP_1P5);
	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_MODE_CFG,
			       RG55_TYPEC_EN_SRC_ONLY);

	/* Suspend USBIN — required before OTG boost on SMB5. */
	if (!rg55g1_sid_read(ctrl, sid, RG55_USB_CMD_IL, &il))
		(void)rg55g1_sid_write(ctrl, sid, RG55_USB_CMD_IL,
				       il | RG55_USB_SUSPEND);

	/* SW OTG control; disable mid/collapse fault kills. */
	(void)rg55g1_sid_read(ctrl, sid, RG55_OTG_CFG, &cfg);
	cfg &= ~(RG55_OTG_EN_SRC_CFG | RG55_OTG_DBG_MODE);
	(void)rg55g1_sid_write(ctrl, sid, RG55_OTG_CFG, cfg);
	(void)rg55g1_sid_write(ctrl, sid, RG55_OTG_ILIMIT, 0x02); /* 1.5A */
	(void)rg55g1_sid_write(ctrl, sid, RG55_OTG_FAULT_CFG, 0);

	/* Android: SET halt-1-in-8 before enabling OTG. */
	if (!rg55g1_sid_read(ctrl, sid, RG55_OTG_ENG_CFG, &eng))
		(void)rg55g1_sid_write(ctrl, sid, RG55_OTG_ENG_CFG,
				       eng | RG55_OTG_ENG_HALT);

	(void)rg55g1_sid_write(ctrl, sid, RG55_CMD_OTG, RG55_OTG_EN);
}

static void rg55g1_vbus_keep_fn(struct work_struct *work)
{
	u8 cmd = 0, misc = 0, sm = 0, pp = 0, src = 0;
	char msg[28];
	u32 color;

	if (!vbus_keep_ctrl)
		return;

	rg55g1_vbus_force_otg(vbus_keep_ctrl, vbus_keep_sid);

	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid, RG55_CMD_OTG, &cmd);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_MISC_STATUS, &misc);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_SM_STATUS, &sm);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_POWER_PATH_STATUS, &pp);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_SRC_STATUS, &src);

	/*
	 * k=CMD_OTG  S=source-mode(misc.6)  A=attach(sm.5)  C=CC
	 * Want: k01/S1/A1/C1  (+ meter ~5V)
	 */
	snprintf(msg, sizeof(msg), "k%02X/S%d/A%d/C%d",
		 cmd,
		 !!(misc & RG55_TYPEC_SNK_SRC_MODE),
		 !!(sm & RG55_TYPEC_ATTACH_STATE),
		 !!(misc & RG55_TYPEC_CC_ATTACHED));
	if ((cmd & RG55_OTG_EN) && (misc & RG55_TYPEC_SNK_SRC_MODE))
		color = 0x0000ff00;
	else if (misc & RG55_TYPEC_CC_ATTACHED)
		color = 0x00ffff00;
	else
		color = 0x00ff8000;
	rg55g1_status_vbus(msg, color);

	/* Quiet: no kernel log. pp/src kept for future if needed. */
	(void)pp;
	(void)src;

	schedule_delayed_work(&vbus_keep_work, msecs_to_jiffies(500));
}

/**
 * rg55g1_enable_pm7250b_vbus() - enable Type-C VBUS via SPMI debug arb.
 *
 * Main arb EE ownership blocks HLOS writes to PM7250B OTG (SEA). Stock DT
 * exposes qcom,spmi-pmic-arb-debug which bypasses EE; use that for OTG.
 * PM4450s are rails only — VBUS boost is on the charger SID (PM7250B).
 */
static int rg55g1_enable_pm7250b_vbus(void)
{
	static bool done;
	static char last[24] = "VBUS:?";
	static u32 last_color = 0x00808080;
	static int last_ret = -ENODEV;
	struct spmi_controller *main_ctrl, *dbg_ctrl;
	struct spmi_controller *ctrl;
	u8 sid, pmic_type, pmic_sub, type, subtype, cfg, cmd;
	int found = 0, otg_sid = -1;
	int ret;

	/* Always refresh sticky line (USB spam used to scroll it away). */
	if (done) {
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}
	done = true;

	rg55g1_status_vbus("VBUS-SCAN", 0x00ffff00);
	rg55g1_status("VBUS-SCAN", 0x00ffff00);

	main_ctrl = rg55g1_get_spmi(false);
	rg55g1_status_vbus(main_ctrl ? "VBUS-MAIN" : "NO-MAIN",
			   main_ctrl ? 0x00ffff00 : 0x00ff0000);

	dbg_ctrl = rg55g1_get_spmi(true);
	rg55g1_status_vbus(dbg_ctrl ? "VBUS-DBG" : "NO-DBG",
			   dbg_ctrl ? 0x00ffff00 : 0x00ff8000);

	ctrl = dbg_ctrl ? dbg_ctrl : main_ctrl;
	if (!ctrl) {
		strscpy(last, "NO-SPMI", sizeof(last));
		last_color = 0x00ff0000;
		last_ret = -ENODEV;
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}

	pr_emerg("rg55g1: SPMI main=%s debug=%s — scan SID 0..15 for OTG\n",
		 main_ctrl ? "ok" : "no", dbg_ctrl ? "ok" : "no");

	for (sid = 0; sid < 16; sid++) {
		pmic_type = 0;
		pmic_sub = 0;
		ret = rg55g1_sid_read(ctrl, sid, 0x0104, &pmic_type);
		if (ret)
			continue;
		(void)rg55g1_sid_read(ctrl, sid, 0x0105, &pmic_sub);
		found++;
		pr_emerg("rg55g1: sid=%u REVID TYPE=0x%02x SUB=0x%02x\n",
			 sid, pmic_type, pmic_sub);

		type = 0;
		subtype = 0;
		ret = rg55g1_sid_read(ctrl, sid, RG55_OTG_TYPE, &type);
		if (ret || !type || type == 0xff)
			continue;
		(void)rg55g1_sid_read(ctrl, sid, RG55_OTG_SUBTYPE, &subtype);
		pr_emerg("rg55g1: sid=%u OTG@0x1100 TYPE=0x%02x SUB=0x%02x\n",
			 sid, type, subtype);
		otg_sid = sid;
	}

	if (otg_sid < 0) {
		snprintf(last, sizeof(last), found ? "NO-OTG/%d" : "VBUS-NONE",
			 found);
		last_color = 0x00ff8000;
		last_ret = -ENODEV;
		rg55g1_status_vbus(last, last_color);
		pr_emerg("rg55g1: %d PMIC(s) seen, no OTG@0x1100 "
			 "(PM4450=rails; need PM7250B)\n", found);
		return last_ret;
	}

	snprintf(last, sizeof(last), "OTG-s%d", otg_sid);
	last_color = 0x00ffff00;
	rg55g1_status_vbus(last, last_color);

	/*
	 * Prefer debug arb (bypasses EE). If missing, try main arb write —
	 * software returns -EPERM without SEA when another EE owns the
	 * channel (only *stealing* write_ee caused SEA before).
	 */
	ctrl = dbg_ctrl ? dbg_ctrl : main_ctrl;
	if (!ctrl) {
		snprintf(last, sizeof(last), "NO-WR/s%d", otg_sid);
		last_color = 0x00ff0000;
		last_ret = -ENODEV;
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}

	/* Probe write path (debug arb preferred), then apply full OTG seq. */
	ret = rg55g1_sid_write(ctrl, otg_sid, RG55_CMD_OTG, RG55_OTG_EN);
	pr_emerg("rg55g1: sid=%d CMD_OTG probe via %s -> %d\n",
		 otg_sid, dbg_ctrl ? "debug" : "main", ret);

	if (ret == -EPERM && !dbg_ctrl) {
		snprintf(last, sizeof(last), "EPERM/s%d", otg_sid);
		last_color = 0x00ff8000;
		last_ret = ret;
		rg55g1_status_vbus(last, last_color);
		pr_emerg("rg55g1: main arb EE denies OTG write on sid=%d "
			 "(need debug arb or ADSP)\n", otg_sid);
		return last_ret;
	}
	if (ret) {
		snprintf(last, sizeof(last), "OTG-WF/%d", ret);
		last_color = 0x00ff0000;
		last_ret = ret;
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}

	msleep(20);
	cmd = 0;
	(void)rg55g1_sid_read(ctrl, otg_sid, RG55_CMD_OTG, &cmd);
	if (!(cmd & RG55_OTG_EN)) {
		snprintf(last, sizeof(last), "NOACK/s%d", otg_sid);
		last_color = 0x00ff8000;
		last_ret = -EIO;
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}

	/*
	 * TYPEC must stay EN_SRC_ONLY (not DISABLE). OTG enable follows
	 * Android smblib: USBIN suspend + ENG halt set + DCDC_CMD_OTG.
	 */
	{
		u8 ttype = 0, misc = 0, sm = 0, mode = 0, src = 0;
		u8 otg_type = 0, otg_sub = 0;
		int wr;

		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_OTG_TYPE, &otg_type);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_OTG_SUBTYPE, &otg_sub);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_TYPE, &ttype);
		pr_emerg("rg55g1: DCDC TYPE=0x%02x SUB=0x%02x TYPEC_TYPE=0x%02x\n",
			 otg_type, otg_sub, ttype);

		rg55g1_status_vbus("TYPEC-SRC", 0x00ffff00);

		/* Pulse DISABLE once, then lock SRC-only. */
		wr = rg55g1_sid_write(ctrl, otg_sid, RG55_TYPEC_MODE_CFG,
				      RG55_TYPEC_DISABLE_CMD);
		msleep(5);
		(void)rg55g1_sid_write(ctrl, otg_sid, RG55_TYPEC_CURRSRC_CFG,
				       RG55_TYPEC_RP_1P5);
		wr |= rg55g1_sid_write(ctrl, otg_sid, RG55_TYPEC_MODE_CFG,
				       RG55_TYPEC_EN_SRC_ONLY);
		(void)rg55g1_sid_write(ctrl, otg_sid, RG55_TYPEC_VCONN_CTL,
				       RG55_TYPEC_VCONN_EN_SRC);
		(void)rg55g1_sid_write(ctrl, otg_sid, RG55_TYPEC_EXIT_STATE,
				       RG55_TYPEC_SEL_SRC_UPPER_REF | BIT(3));

		rg55g1_vbus_force_otg(ctrl, otg_sid);
		msleep(150);

		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_MISC_STATUS,
				      &misc);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_SM_STATUS, &sm);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_MODE_CFG,
				      &mode);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_SRC_STATUS,
				      &src);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_CMD_OTG, &cmd);

		if (!(mode & RG55_TYPEC_EN_SRC_ONLY)) {
			(void)rg55g1_sid_write(ctrl, otg_sid, RG55_TYPEC_MODE_CFG,
					       RG55_TYPEC_EN_SRC_ONLY);
			rg55g1_vbus_force_otg(ctrl, otg_sid);
			msleep(50);
			(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_MODE_CFG,
					      &mode);
			(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_MISC_STATUS,
					      &misc);
			(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_SM_STATUS,
					      &sm);
			(void)rg55g1_sid_read(ctrl, otg_sid, RG55_CMD_OTG, &cmd);
		}

		pr_emerg("rg55g1: final misc=0x%02x sm=0x%02x mode=0x%02x "
			 "CMD=0x%02x src=0x%02x (wr=%d)\n",
			 misc, sm, mode, cmd, src, wr);

		vbus_keep_ctrl = ctrl;
		vbus_keep_sid = otg_sid;
		schedule_delayed_work(&vbus_keep_work, msecs_to_jiffies(500));

		snprintf(last, sizeof(last), "k%02X/S%d/A%d/C%d",
			 cmd,
			 !!(misc & RG55_TYPEC_SNK_SRC_MODE),
			 !!(sm & RG55_TYPEC_ATTACH_STATE),
			 !!(misc & RG55_TYPEC_CC_ATTACHED));
		if ((cmd & RG55_OTG_EN) && (misc & RG55_TYPEC_SNK_SRC_MODE)) {
			last_color = 0x0000ff00;
			last_ret = 0;
		} else if (misc & RG55_TYPEC_CC_ATTACHED) {
			last_color = 0x00ffff00;
			last_ret = -EAGAIN;
		} else {
			last_color = 0x00ff8000;
			last_ret = -EAGAIN;
		}
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}
}

/*
 * Minimal ACTIVE-TCS poke — stock DT has qcom,tcs-config with 10 cells;
 * upstream rpmh-rsc rejects that (-EINVAL) so apps_rsc never probes (RSC-!).
 * Vote PHY LDOs by programming DRV2 TCS0 directly and polling CMD_STATUS.
 */
#define RG55_RSC_DRV2		0x17a20000
#define RG55_RSC_TCS_OFF	0xd00
#define RG55_RSC_TCS_STRIDE	672
#define RG55_RSC_CMD_STRIDE_V27	20
#define RG55_RSC_CMD_STRIDE_V30	24
#define RG55_TCS_AMC_ENABLE	BIT(16)
#define RG55_TCS_AMC_TRIGGER	BIT(24)
#define RG55_CMD_MSGID		(8 | BIT(8) | BIT(16)) /* LEN|RESP_REQ|WRITE */
#define RG55_CMD_COMPL		BIT(16)

static void rg55g1_set_u32_array_prop(struct device_node *np, const char *name,
				      const u32 *vals, int n)
{
	struct property *pp;
	__be32 *be;
	int i;

	if (!np || !name || !vals || n <= 0)
		return;

	be = kmalloc_array(n, sizeof(*be), GFP_KERNEL);
	if (!be)
		return;
	for (i = 0; i < n; i++)
		be[i] = cpu_to_be32(vals[i]);

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		kfree(be);
		return;
	}
	pp->name = kstrdup(name, GFP_KERNEL);
	if (!pp->name) {
		kfree(be);
		kfree(pp);
		return;
	}
	pp->length = n * sizeof(*be);
	pp->value = be;
	of_update_property(np, pp);
}

static int rg55g1_rpmh_raw_cmds(struct tcs_cmd *cmds, int ncmds)
{
	void __iomem *drv, *tcs, *cmd;
	u32 rsc_id, ver_maj, cmd_stride, ctrl, st;
	u32 off_ctrl, off_en, off_msgid, off_addr, off_data, off_status, off_irq_en;
	int i, t, tcs_id = 0;

	drv = ioremap(RG55_RSC_DRV2, 0x10000);
	if (!drv)
		return -ENOMEM;

	rsc_id = readl_relaxed(drv);
	ver_maj = (rsc_id >> 16) & 0xff;
	pr_emerg("rg55g1: RSC drv2 id=0x%x maj=%u\n", rsc_id, ver_maj);

	if (ver_maj >= 3) {
		cmd_stride = RG55_RSC_CMD_STRIDE_V30;
		off_ctrl = 0x24;
		off_en = 0x2c;
		off_msgid = 0x34;
		off_addr = 0x38;
		off_data = 0x3c;
		off_status = 0x40;
		off_irq_en = 0x00;
	} else {
		cmd_stride = RG55_RSC_CMD_STRIDE_V27;
		off_ctrl = 0x14;
		off_en = 0x1c;
		off_msgid = 0x30;
		off_addr = 0x34;
		off_data = 0x38;
		off_status = 0x3c;
		off_irq_en = 0x00;
	}

	tcs = drv + RG55_RSC_TCS_OFF + RG55_RSC_TCS_STRIDE * tcs_id;

	/*
	 * Do NOT enable TCS IRQs — a later rpmh-rsc probe would see a
	 * pending IRQ with no stashed request and WARN at rpmh-rsc.c:451.
	 * We poll CMD_STATUS instead.
	 */
	writel_relaxed(0, drv + RG55_RSC_TCS_OFF + off_irq_en);

	/* Clear previous AMC trigger/enable (same sequence as rpmh-rsc). */
	ctrl = readl_relaxed(tcs + off_ctrl);
	ctrl &= ~RG55_TCS_AMC_TRIGGER;
	writel(ctrl, tcs + off_ctrl);
	ctrl &= ~RG55_TCS_AMC_ENABLE;
	writel(ctrl, tcs + off_ctrl);
	writel(0, tcs + off_en);

	for (i = 0; i < ncmds; i++) {
		cmd = tcs + cmd_stride * i;
		writel_relaxed(RG55_CMD_MSGID, cmd + off_msgid);
		writel_relaxed(cmds[i].addr, cmd + off_addr);
		writel_relaxed(cmds[i].data, cmd + off_data);
	}
	writel_relaxed(GENMASK(ncmds - 1, 0), tcs + off_en);

	writel(RG55_TCS_AMC_ENABLE, tcs + off_ctrl);
	writel(RG55_TCS_AMC_ENABLE | RG55_TCS_AMC_TRIGGER, tcs + off_ctrl);

	for (i = 0; i < ncmds; i++) {
		cmd = tcs + cmd_stride * i;
		for (t = 0; t < 10000; t++) {
			st = readl_relaxed(cmd + off_status);
			if (st & RG55_CMD_COMPL)
				break;
			udelay(10);
		}
		if (!(st & RG55_CMD_COMPL)) {
			pr_emerg("rg55g1: RPMh raw cmd%d addr=0x%x timeout st=0x%x\n",
				 i, cmds[i].addr, st);
			iounmap(drv);
			return -ETIMEDOUT;
		}
	}

	/* Untrigger + clear any latched IRQ status bit. */
	writel(RG55_TCS_AMC_ENABLE, tcs + off_ctrl);
	writel(0, tcs + off_ctrl);
	writel_relaxed(BIT(tcs_id), drv + RG55_RSC_TCS_OFF + 0x08); /* IRQ_CLEAR */
	writel_relaxed(0, drv + RG55_RSC_TCS_OFF + off_irq_en);

	iounmap(drv);
	return 0;
}

/**
 * rg55g1_enable_hsphy_rails() - vote HS PHY LDOs via raw RPMh TCS (fixes P=0).
 *
 * Stock hsphy supplies: ldob5 (~0.88V), ldob23 (1.8V), ldob25 (~3.1V).
 * Without these the 0x88e3000 CSR space reads as all zeros.
 *
 * Bypasses rpmh-rsc platform driver: OEM tcs-config length makes it -EINVAL.
 */
static int rg55g1_enable_hsphy_rails(void)
{
	struct device_node *cmd_np, *rsc_np;
	static const struct {
		const char *name;
		u32 uv;
	} rails[] = {
		{ "ldob5",  880000 },
		{ "ldob23", 1800000 },
		{ "ldob25", 3072000 },
	};
	/* Upstream wants exactly 8 cells; OEM blob has a trailing junk pair. */
	static const u32 tcs_fix[] = {
		2, 2,	/* ACTIVE_TCS, 2 */
		0, 3,	/* SLEEP_TCS, 3 */
		1, 3,	/* WAKE_TCS, 3 */
		3, 0,	/* CONTROL_TCS, 0 */
	};
	struct tcs_cmd cmds[6];
	int i, n = 0, ret, tries, ok = 0;

	cmd_np = of_find_compatible_node(NULL, NULL, "qcom,cmd-db");
	if (cmd_np) {
		struct platform_device *p;

		rg55g1_force_status(cmd_np, "okay");
		p = of_find_device_by_node(cmd_np);
		if (p)
			put_device(&p->dev);
		else
			of_platform_device_create(cmd_np, NULL, NULL);
		of_node_put(cmd_np);
	}
	for (tries = 0; tries < 50; tries++) {
		if (!cmd_db_ready())
			break;
		msleep(20);
	}
	ret = cmd_db_ready();
	if (ret) {
		pr_emerg("rg55g1: cmd-db not ready (%d)\n", ret);
		rg55g1_status("CMDDB-!", 0x00ff0000);
		return ret;
	}

	/*
	 * Keep apps_rsc disabled: OEM tcs-config has 10 cells (upstream wants
	 * 8) and a probe after our raw vote races IRQs → WARN at :451.
	 * Raw MMIO vote is enough for PHY LDOs.
	 */
	rsc_np = of_find_node_by_path("/soc/rsc@17a00000");
	if (rsc_np) {
		int ne = of_property_count_u32_elems(rsc_np, "qcom,tcs-config");

		pr_emerg("rg55g1: stock tcs-config elems=%d (need 8)\n", ne);
		if (ne != 8)
			rg55g1_set_u32_array_prop(rsc_np, "qcom,tcs-config",
						  tcs_fix, ARRAY_SIZE(tcs_fix));
		rg55g1_remove_prop(rsc_np, "power-domains");
		rg55g1_force_disabled_node(rsc_np);
		of_node_put(rsc_np);
	}

	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		u32 addr = cmd_db_read_addr(rails[i].name);

		if (!addr) {
			pr_emerg("rg55g1: cmd-db missing %s\n", rails[i].name);
			continue;
		}
		cmds[n].addr = addr + 0x0;
		cmds[n].data = DIV_ROUND_UP(rails[i].uv, 1000);
		cmds[n].wait = 1;
		n++;
		cmds[n].addr = addr + 0x4;
		cmds[n].data = 1;
		cmds[n].wait = 1;
		n++;
		ok++;
		pr_emerg("rg55g1: queue %s addr=0x%x %umV\n",
			 rails[i].name, addr, DIV_ROUND_UP(rails[i].uv, 1000));
	}

	if (!n) {
		rg55g1_status("LDO-NA", 0x00ff0000);
		return -ENOENT;
	}

	/* One TCS holds up to 16 cmds; we have 6. */
	ret = rg55g1_rpmh_raw_cmds(cmds, n);
	pr_emerg("rg55g1: RPMh raw vote (%d cmds, %d rails) -> %d\n", n, ok, ret);
	usleep_range(2000, 3000);

	if (!ret) {
		rg55g1_status("LDO-OK", 0x0000ff00);
		return 0;
	}
	rg55g1_status("LDO-!", 0x00ff8000);
	return ret;
}

/**
 * rg55g1_usb_phy_clk_reset() - ungated HS PHY CSR space.
 *
 * Diagnostic P=0 (UTMI_CTRL0) means 0x88e3000 is dead: QUSB2PHY still in BCR
 * and/or USB clkref off. Controller can still show PP with CCS stuck at 0.
 */
static void rg55g1_usb_phy_clk_reset(void)
{
	struct device_node *gcc_np;
	void __iomem *gcc;
	u32 bcr;
	int i;
	static const u32 clk_ids[] = {
		GCC_USB30_PRIM_MASTER_CLK,
		GCC_USB30_PRIM_MOCK_UTMI_CLK,
		GCC_USB30_PRIM_SLEEP_CLK,
		GCC_CFG_NOC_USB3_PRIM_AXI_CLK,
		GCC_AGGRE_USB3_PRIM_AXI_CLK,
		GCC_USB3_0_CLKREF_EN,
		GCC_EUSB3_0_CLKREF_EN,
		GCC_USB3_PRIM_PHY_AUX_CLK,
		GCC_USB3_PRIM_PHY_COM_AUX_CLK,
	};

	gcc_np = of_find_compatible_node(NULL, NULL, "qcom,sm4450-gcc");
	if (!gcc_np)
		gcc_np = of_find_compatible_node(NULL, NULL, "qcom,ravelin-gcc");

	if (gcc_np) {
		for (i = 0; i < ARRAY_SIZE(clk_ids); i++) {
			struct of_phandle_args args = { };
			struct clk *clk;

			args.np = gcc_np;
			args.args[0] = clk_ids[i];
			args.args_count = 1;
			clk = of_clk_get_from_provider(&args);
			if (IS_ERR(clk))
				continue;
			if (clk_prepare_enable(clk))
				clk_put(clk);
			/* leak enable vote intentionally for bring-up */
		}
		of_node_put(gcc_np);
	}

	/* Direct GCC poke — clkref CBCR + QUSB2PHY BCR */
	gcc = ioremap(0x00100000, 0xa0000);
	if (!gcc) {
		pr_emerg("rg55g1: GCC ioremap failed\n");
		return;
	}

	writel(1, gcc + 0x9c00c); /* gcc_eusb3_0_clkref_en */
	writel(1, gcc + 0x9c010); /* gcc_usb3_0_clkref_en */

	bcr = readl(gcc + 0x22000); /* GCC_QUSB2PHY_PRIM_BCR */
	writel(1, gcc + 0x22000);
	udelay(100);
	writel(0, gcc + 0x22000);
	udelay(100);

	pr_emerg("rg55g1: QUSB2PHY BCR was 0x%x -> 0, clkref forced\n", bcr);
	rg55g1_status("PHY-CLK", 0x00ffff00);
	iounmap(gcc);
}

/**
 * rg55g1_hsphy_force_wake() - write SNPS femto regs once CSR space is live.
 */
static void rg55g1_hsphy_force_wake(void)
{
	void __iomem *phy = ioremap(0x088e3000, 0x200);
	u32 utmi, common0, common1, ctrl1, ctrl2;

	if (!phy)
		return;

	common0 = readl(phy + 0x54);
	utmi = readl(phy + 0x3c);

	/* Clear SIDDQ, set SLEEPM / VBUSVLDEXT / SUSPEND_N */
	writel(common0 & ~BIT(2), phy + 0x54);
	common1 = readl(phy + 0x58);
	writel(common1 | BIT(4) | BIT(5), phy + 0x58); /* VBUSVLDEXTSEL | PLLBTUNE */
	ctrl1 = readl(phy + 0x60);
	writel(ctrl1 | BIT(0), phy + 0x60); /* VBUSVLDEXT0 */
	ctrl2 = readl(phy + 0x64);
	writel(ctrl2 | BIT(2) | BIT(3), phy + 0x64); /* SUSPEND_N | SEL */
	writel((utmi & ~GENMASK(4, 3)) | BIT(0), phy + 0x3c); /* SLEEPM, OPMODE=0 */

	utmi = readl(phy + 0x3c);
	common0 = readl(phy + 0x54);
	pr_emerg("rg55g1: HS PHY wake UTMI=0x%x COMMON0=0x%x\n", utmi, common0);
	iounmap(phy);
}
static void rg55g1_bringup_scm(void)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_find_compatible_node(NULL, NULL, "qcom,scm");
	if (!np)
		return;

	rg55g1_force_status(np, "okay");
	pdev = of_find_device_by_node(np);
	if (pdev) {
		put_device(&pdev->dev);
	} else if (!of_platform_device_create(np, NULL, NULL)) {
		pr_emerg("rg55g1: scm create failed\n");
	} else {
		pr_emerg("rg55g1: scm created\n");
		msleep(200);
	}
	of_node_put(np);
}

/**
 * rg55g1_disable_eud() - release Type-C USB from Embedded USB Debugger.
 *
 * Stock debug builds leave EUD enabled; it muxes the HS PHY away from DWC3,
 * so xHCI shows PP but CCS never asserts (no 1-1).
 *
 * Do NOT call qcom_scm_io_writel() unless SCM is probed — that helper
 * dereferences __scm unconditionally and panics when of_platform is skipped.
 */
static void rg55g1_disable_eud(void)
{
	void __iomem *base, *mgr;
	u32 csr = 0, en2 = 0;
	int ret = -ENODEV;

	base = ioremap(0x088e0000, 0x2000);
	mgr = ioremap(0x088e2000, 0x1000);

	if (base) {
		csr = readl(base + 0x1014); /* EUD_REG_CSR_EUD_EN */
		writel(0, base + 0x1014);
		writel(0, base + 0x1018); /* SW_ATTACH_DET */
	}

	if (mgr) {
		en2 = readl(mgr);
		writel(0, mgr); /* EUD_EN2 — try non-secure path first */
	}

	/* Only if firmware/scm was actually created (usually not in bring-up). */
	if (qcom_scm_is_available())
		ret = qcom_scm_io_writel(0x088e2000, 0);

	pr_emerg("rg55g1: EUD disable CSR was 0x%x EN2 was 0x%x scm=%d\n",
		 csr, en2, ret);
	rg55g1_status(csr || en2 ? "EUD-OFF" : "EUD-0", 0x00ffff00);

	if (base)
		iounmap(base);
	if (mgr)
		iounmap(mgr);
}

/**
 * rg55g1_bringup_apps_smmu() - USB SID 0x540 phys DMA, keep display alive.
 *
 * Black-screen causes (avoided here):
 *  - USFCFG=0 + any display stream left unmatched (IOVA treated as PA)
 *  - Overwriting a live broad SMR before a verified relocate copy
 *  - Global TLBI while display is mid-frame
 *
 * Method: ensure an exact SID=0x540 SMR wins first-match, keep it VALID,
 * point S2CR at a disabled CB (QCOM BYPASS quirk). Never touch sCR0.
 */
static bool rg55g1_smmu_bypass_cb(void __iomem *smmu, u32 id1, u32 *cbndx_out)
{
	u32 pgshift = (id1 & BIT(31)) ? 16 : 12;
	u32 numpage = 1u << (FIELD_GET(GENMASK(30, 28), id1) + 1);
	u32 numcb = FIELD_GET(GENMASK(7, 0), id1);
	void __iomem *gr1 = smmu + (1u << pgshift);
	u32 try;

	if (!numcb || numcb > 128)
		numcb = 32;

	for (try = 0; try < 4 && try < numcb; try++) {
		u32 cbndx = numcb - 1 - try;
		void __iomem *cb = smmu + ((numpage + cbndx) << pgshift);
		u32 cbar = FIELD_PREP(GENMASK(17, 16), 1); /* S1+S2_BYPASS */
		u32 got;

		writel_relaxed(cbar, gr1 + (cbndx << 2));
		writel_relaxed(0, cb + 0x0); /* SCTLR.M=0 */
		got = readl_relaxed(gr1 + (cbndx << 2));
		if (FIELD_GET(GENMASK(17, 16), got) != 1)
			continue;
		if (readl_relaxed(cb + 0x0) & BIT(0))
			continue;
		*cbndx_out = cbndx;
		return true;
	}
	return false;
}

static int rg55g1_bringup_apps_smmu(void)
{
	void __iomem *gcc, *smmu;
	u32 id0, id1, scr0, smr, s2cr, want_s2cr, cbndx = 0;
	u32 numsmr, i, sid = 0x540;
	int first = -1, free_smr = -1, usb_idx = -1;

	gcc = ioremap(0x00100000, 0xa0000);
	if (gcc) {
		writel(1, gcc + 0x8d02c);
		writel(1, gcc + 0x8d008);
		writel(1, gcc + 0x8d00c);
		iounmap(gcc);
	}

	smmu = ioremap(0x15000000, 0x100000);
	if (!smmu) {
		rg55g1_status("SMMU-MAP", 0x00ff0000);
		return -ENOMEM;
	}

	id0 = readl_relaxed(smmu + 0x20);
	id1 = readl_relaxed(smmu + 0x24);
	scr0 = readl_relaxed(smmu + 0x0);
	pr_emerg("rg55g1: apps-smmu ID0=0x%x ID1=0x%x sCR0=0x%x\n",
		 id0, id1, scr0);

	if (id0 == 0 || id0 == ~0u) {
		rg55g1_status("SMMU-DE", 0x00ff0000);
		iounmap(smmu);
		return -EIO;
	}

	if (!rg55g1_smmu_bypass_cb(smmu, id1, &cbndx)) {
		pr_emerg("rg55g1: no writable bypass CB (TZ?)\n");
		rg55g1_status("SMMU-CB!", 0x00ff8000);
		iounmap(smmu);
		return -EPERM;
	}

	numsmr = id0 & 0xff;
	if (!numsmr || numsmr > 256)
		numsmr = 128;

	for (i = 0; i < numsmr; i++) {
		u32 id, mask;

		smr = readl_relaxed(smmu + 0x800 + (i << 2));
		if (!(smr & BIT(31))) {
			if (free_smr < 0)
				free_smr = i;
			continue;
		}
		id = smr & 0xffff;
		mask = (smr >> 16) & 0x7fff;
		if ((sid & ~mask) != (id & ~mask))
			continue;
		s2cr = readl_relaxed(smmu + 0xc00 + (i << 2));
		pr_emerg("rg55g1: SMR[%u] match SID 0x%x smr=0x%x s2cr=0x%x\n",
			 i, sid, smr, s2cr);
		if (first < 0)
			first = i;
	}

	want_s2cr = FIELD_PREP(GENMASK(17, 16), 0) | /* TRANS → disabled CB */
		    FIELD_PREP(GENMASK(7, 0), cbndx);

	if (first < 0) {
		if (free_smr < 0) {
			rg55g1_status("SMMU-NSID", 0x00ff8000);
			iounmap(smmu);
			return -ENOENT;
		}
		usb_idx = free_smr;
	} else {
		smr = readl_relaxed(smmu + 0x800 + (first << 2));
		if (((smr >> 16) & 0x7fff) == 0 && (smr & 0xffff) == sid) {
			usb_idx = first;
		} else if (free_smr >= 0 && free_smr < first) {
			usb_idx = free_smr;
		} else if (free_smr > first) {
			u32 old_s2cr = readl_relaxed(smmu + 0xc00 + (first << 2));

			/* Copy broad entry first; abort if TZ ignores the copy. */
			writel_relaxed(smr, smmu + 0x800 + (free_smr << 2));
			writel_relaxed(old_s2cr, smmu + 0xc00 + (free_smr << 2));
			if (readl_relaxed(smmu + 0x800 + (free_smr << 2)) != smr ||
			    readl_relaxed(smmu + 0xc00 + (free_smr << 2)) != old_s2cr) {
				pr_emerg("rg55g1: relocate copy to SMR[%d] rejected\n",
					 free_smr);
				rg55g1_status("SMMU-TZ", 0x00ff8000);
				iounmap(smmu);
				return -EPERM;
			}
			usb_idx = first;
			pr_emerg("rg55g1: relocated SMR[%d]→[%d] (verified)\n",
				 first, free_smr);
		} else {
			rg55g1_status("SMMU-FULL", 0x00ff8000);
			iounmap(smmu);
			return -ENOSPC;
		}
	}

	/* Exact VALID SMR + bypass CB. Never clear VALID, never USFCFG. */
	writel_relaxed(BIT(31) | sid, smmu + 0x800 + (usb_idx << 2));
	writel_relaxed(want_s2cr, smmu + 0xc00 + (usb_idx << 2));

	smr = readl_relaxed(smmu + 0x800 + (usb_idx << 2));
	s2cr = readl_relaxed(smmu + 0xc00 + (usb_idx << 2));
	if (!(smr & BIT(31)) || (smr & 0xffff) != sid) {
		pr_emerg("rg55g1: USB SMR[%d] write rejected smr=0x%x\n",
			 usb_idx, smr);
		rg55g1_status("SMMU-SMR!", 0x00ff8000);
		iounmap(smmu);
		return -EPERM;
	}

	pr_emerg("rg55g1: SMMU USB SMR[%d]=0x%x s2cr=0x%x cb=%u sCR0=0x%x\n",
		 usb_idx, smr, s2cr, cbndx, scr0);
	rg55g1_status("SMMU-BYP", 0x0000ff00);
	iounmap(smmu);
	return 0;
}

/**
 * rg55g1_bringup_usb() - create GCC + USB platform devices for HID keyboard.
 */
int rg55g1_bringup_usb(void)
{
	struct device_node *soc, *child, *dwc3, *hsphy = NULL;
	int n = 0;
	int pass;

	rg55g1_status("USB-PREP", 0x00ff8000);

	/* SCM first so secure EUD_EN2 writes can succeed. */
	rg55g1_bringup_scm();

	/* PHY LDOs must be on before 0x88e3000 is readable (else P=0). */
	(void)rg55g1_enable_hsphy_rails();

	/* Ungate HS PHY CSR before EUD/host. */
	rg55g1_usb_phy_clk_reset();
	rg55g1_hsphy_force_wake();

	/* Must free HS PHY from EUD before host can see D+/D-. */
	rg55g1_disable_eud();
	rg55g1_hsphy_force_wake();

	/* Type-C 5V from PM7250B OTG boost — do this before/with host. */
	(void)rg55g1_enable_pm7250b_vbus();

	soc = of_find_node_by_path("/soc");
	if (!soc) {
		rg55g1_status("USB-NOSOC", 0x00ff0000);
		return -ENODEV;
	}

	/* Pass -1: GCC only (SMMU TCU clock + USB clocks). */
	for_each_child_of_node(soc, child) {
		bool is_gcc = of_device_is_compatible(child, "qcom,ravelin-gcc") ||
			      of_device_is_compatible(child, "qcom,sm4450-gcc");

		if (!is_gcc)
			continue;
		rg55g1_force_status(child, "okay");
		if (of_device_is_compatible(child, "qcom,ravelin-gcc"))
			rg55g1_set_compatible(child, "qcom,sm4450-gcc");
		if (!of_platform_device_create(child, NULL, NULL)) {
			struct platform_device *exist =
				of_find_device_by_node(child);

			if (exist) {
				put_device(&exist->dev);
				n++;
			}
		} else {
			n++;
		}
		pr_emerg("rg55g1: GCC ready for SMMU/USB\n");
	}
	msleep(100);

	(void)rg55g1_bringup_apps_smmu();

	hsphy = of_find_compatible_node(NULL, NULL, "qcom,usb-hsphy-snps-femto");
	if (!hsphy)
		hsphy = of_find_compatible_node(NULL, NULL,
						"qcom,usb-snps-femto-v2-phy");
	if (hsphy)
		rg55g1_set_u32_prop(hsphy, "#phy-cells", 0);

	/*
	 * Pass 0: HS PHY first so dwc3 can resolve phys.
	 * Pass 1: DWC3 glue.
	 */
	for (pass = 0; pass < 2; pass++) {
		for_each_child_of_node(soc, child) {
			bool is_gcc = of_device_is_compatible(child, "qcom,ravelin-gcc") ||
				      of_device_is_compatible(child, "qcom,sm4450-gcc");
			bool is_phy =
				of_device_is_compatible(child, "qcom,usb-hsphy-snps-femto") ||
				of_device_is_compatible(child, "qcom,usb-snps-femto-v2-phy");
			bool is_dwc =
				of_device_is_compatible(child, "qcom,dwc-usb3-msm") ||
				of_device_is_compatible(child, "qcom,dwc3") ||
				of_device_is_compatible(child, "qcom,snps-dwc3");

			if (!rg55g1_is_usb_keep(child))
				continue;
			if (is_gcc)
				continue; /* already created */
			if (pass == 0 && is_dwc)
				continue;
			if (pass == 1 && !is_dwc)
				continue;

			rg55g1_force_status(child, "okay");

			if (is_dwc)
				rg55g1_patch_ssusb_glue(child);

			dwc3 = of_get_compatible_child(child, "snps,dwc3");
			if (dwc3) {
				rg55g1_patch_dwc3_hs_only(dwc3, hsphy);
				of_node_put(dwc3);
			}

			if (!of_platform_device_create(child, NULL, NULL)) {
				struct platform_device *exist =
					of_find_device_by_node(child);

				if (exist) {
					put_device(&exist->dev);
					of_platform_populate(child, NULL, NULL, NULL);
					n++;
					pr_emerg("rg55g1: already-up %pOF\n", child);
				} else {
					pr_emerg("rg55g1: usb create failed for %pOF\n",
						 child);
				}
			} else {
				of_platform_populate(child, NULL, NULL, NULL);
				n++;
				pr_emerg("rg55g1: populated %pOF%s\n", child,
					 is_phy ? " (phy)" :
					 is_dwc ? " (dwc)" : "");
			}
		}
	}
	of_node_put(hsphy);
	of_node_put(soc);

	rg55g1_block_deferred = false;
	rg55g1_status("USB-DEF", 0x00ff8000);
	driver_deferred_probe_trigger();
	msleep(300);
	driver_deferred_probe_trigger();
	msleep(200);

	/* Re-assert OTG after USB clocks/host are up (host probe can drop it). */
	rg55g1_status("USB-VBUS", 0x00ffff00);
	(void)rg55g1_enable_pm7250b_vbus();
	/* EUD may have been re-enabled by firmware; clear again. */
	rg55g1_disable_eud();
	rg55g1_usb_phy_clk_reset();
	rg55g1_hsphy_force_wake();
	/* Kick keepalive immediately with fresh writes */
	if (vbus_keep_ctrl)
		mod_delayed_work(system_wq, &vbus_keep_work, 0);

	rg55g1_status(n ? "USB-DEV" : "USB-NONE", n ? 0x0000ff00 : 0x00ff0000);
	return n;
}
EXPORT_SYMBOL_GPL(rg55g1_bringup_usb);

int rg55g1_vbus_refresh(void)
{
	return rg55g1_enable_pm7250b_vbus();
}
EXPORT_SYMBOL_GPL(rg55g1_vbus_refresh);

static int __init rg55g1_bringup_early_flags(void)
{
	rg55g1_block_deferred = true;
	rg55g1_skip_of_populate = true;
	return 0;
}
pure_initcall(rg55g1_bringup_early_flags);
