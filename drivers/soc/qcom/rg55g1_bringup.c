// SPDX-License-Identifier: GPL-2.0-only
/*
 * RG55G1 bring-up: reach ash on splash FB, then enable a minimal USB host
 * path so a USB keyboard can feed tty0, plus SDHCI for /dev/mmcblk*, and
 * PM7250B Type-C dual-role (OTG VBUS for peripherals / USBIN battery charge).
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/blkdev.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
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
#include "rg55g1_bringup.h"
#include "rg55g1_splash.h"

bool rg55g1_skip_of_populate = true;
EXPORT_SYMBOL_GPL(rg55g1_skip_of_populate);

bool rg55g1_msm_early;
bool rg55g1_msm_auto;
bool rg55g1_preserve_abl_display = true;
EXPORT_SYMBOL_GPL(rg55g1_preserve_abl_display);

bool rg55g1_abl_panel_ready = true;
EXPORT_SYMBOL_GPL(rg55g1_abl_panel_ready);
/* Default on: OF must not reset/populate MDSS while ABL splash is scanning. */
bool rg55g1_skip_mdss_reset = true;
bool rg55g1_skip_mdss_populate = true;
EXPORT_SYMBOL_GPL(rg55g1_skip_mdss_reset);
EXPORT_SYMBOL_GPL(rg55g1_skip_mdss_populate);

/* Always sync-unpack initramfs on this board (async + device probe wedges). */
bool rg55g1_force_sync_rootfs = true;
EXPORT_SYMBOL_GPL(rg55g1_force_sync_rootfs);

/* When set, PHY may proceed without PMIC regulators (bootloader left USB up). */
bool rg55g1_usb_loose_supplies = true;
EXPORT_SYMBOL_GPL(rg55g1_usb_loose_supplies);

/* Set by xhci-plat poll from PORT_CONNECT (keyboard may work while CC=A0/C0). */
bool rg55g1_usb_host_port_connected;
EXPORT_SYMBOL_GPL(rg55g1_usb_host_port_connected);

extern bool rg55g1_block_deferred;
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

void rg55g1_force_node_okay(struct device_node *np)
{
	if (!np)
		return;
	rg55g1_force_status(np, "okay");
}
EXPORT_SYMBOL_GPL(rg55g1_force_node_okay);

void rg55g1_force_node_disabled(struct device_node *np)
{
	rg55g1_force_disabled_node(np);
}
EXPORT_SYMBOL_GPL(rg55g1_force_node_disabled);

static bool rg55g1_want_lv6;

static bool rg55g1_is_mainline_board(void)
{
	/* Stock flattened DTB used qcom,ravelinp; mainline uses anbernic + sm4450. */
	return of_machine_is_compatible("anbernic,rg55g1") ||
	       (of_machine_is_compatible("qcom,sm4450") &&
		!of_machine_is_compatible("qcom,ravelinp"));
}

static struct device_node *rg55g1_find_soc_node(void)
{
	struct device_node *soc;

	/* Mainline sm4450.dtsi uses soc@0; stock flattened DT used /soc. */
	soc = of_find_node_by_path("/soc@0");
	if (!soc)
		soc = of_find_node_by_path("/soc");
	return soc;
}

static bool rg55g1_is_hsphy(struct device_node *np)
{
	return of_device_is_compatible(np, "qcom,usb-hsphy-snps-femto") ||
	       of_device_is_compatible(np, "qcom,usb-snps-femto-v2-phy") ||
	       of_device_is_compatible(np, "qcom,usb-snps-hs-7nm-phy") ||
	       of_device_is_compatible(np, "qcom,sm4450-usb-hs-phy");
}

static bool rg55g1_is_dwc_glue(struct device_node *np)
{
	return of_device_is_compatible(np, "qcom,dwc-usb3-msm") ||
	       of_device_is_compatible(np, "qcom,dwc3") ||
	       of_device_is_compatible(np, "qcom,snps-dwc3") ||
	       of_device_is_compatible(np, "qcom,sm4450-dwc3");
}

static bool rg55g1_is_sdhci(struct device_node *np)
{
	return of_device_is_compatible(np, "qcom,sdhci-msm-v5") ||
	       of_device_is_compatible(np, "qcom,sdhci-msm-v4");
}

static bool rg55g1_is_display_node(struct device_node *np)
{
	return of_device_is_compatible(np, "qcom,ravelin-dispcc") ||
	       of_device_is_compatible(np, "qcom,sm4450-dispcc") ||
	       of_device_is_compatible(np, "qcom,ravelin-mdss") ||
	       of_device_is_compatible(np, "qcom,sm4450-mdss") ||
	       of_device_is_compatible(np, "qcom,mdss") ||
	       of_device_is_compatible(np, "qcom,sm4450-dpu") ||
	       of_device_is_compatible(np, "qcom,ravelin-dpu") ||
	       of_device_is_compatible(np, "qcom,sm4450-dsi-ctrl") ||
	       of_device_is_compatible(np, "qcom,mdss-dsi-ctrl") ||
	       of_device_is_compatible(np, "qcom,sm4450-dsi-phy-4nm") ||
	       of_device_is_compatible(np, "qcom,ravelin-dsi-phy-4nm") ||
	       of_device_is_compatible(np, "focaltech,ft7131m");
}

static bool rg55g1_is_smem_keep(struct device_node *np)
{
	return of_device_is_compatible(np, "qcom,smem") ||
	       of_device_is_compatible(np, "qcom,tcsr-mutex");
}

static bool rg55g1_is_smmu_node(struct device_node *np)
{
	return of_device_is_compatible(np, "qcom,qsmmu-v500") ||
	       of_device_is_compatible(np, "qcom,smmu-500") ||
	       of_device_is_compatible(np, "arm,mmu-500") ||
	       of_device_is_compatible(np, "qcom,adreno-smmu");
}

static bool rg55g1_is_usb_keep(struct device_node *np)
{
	/* HS-only: skip SS PHY and nop-xceiv (legacy usb-phy / dummy vbus). */
	return of_device_is_compatible(np, "qcom,ravelin-gcc") ||
	       of_device_is_compatible(np, "qcom,sm4450-gcc") ||
	       rg55g1_is_dwc_glue(np) ||
	       of_device_is_compatible(np, "snps,dwc3") ||
	       rg55g1_is_hsphy(np) ||
	       of_device_is_compatible(np, "qcom,spmi-pmic-arb") ||
	       of_device_is_compatible(np, "qcom,spmi-pmic-arb-debug") ||
	       /*
		* Keep SMMU nodes out of sanitize-disable, but do NOT create
		* platform devices for them in USB bringup (see populate loop).
		*/
	       rg55g1_is_smmu_node(np) ||
	       rg55g1_is_sdhci(np) ||
	       rg55g1_is_smem_keep(np) ||
	       rg55g1_is_display_node(np);
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
 * rg55g1_sanitize_dt() - disable hang-prone DT nodes; keep USB/GCC/SDHCI.
 *
 * Mainline board + rg55g1.lv6=1: do NOT flip block_deferred / skip_of_populate.
 * Splash runs at fs_initcall *before* rootfs_initcall; the old code re-armed
 * LV6-skip here and forced SYNC-RD even when the user passed lv6=1, then MSM
 * quiesced ABL splash into a black screen when modeset lagged.
 */
int rg55g1_sanitize_dt(void)
{
	int n = 0;
	struct device_node *rm, *child, *ramo;
	bool keep_lv6 = rg55g1_want_lv6 || rg55g1_is_mainline_board();

	if (keep_lv6) {
		pr_emerg("rg55g1: DT sanitize skipped (mainline/lv6 — keep OF populate)\n");
		return 0;
	}

	rg55g1_block_deferred = true;
	rg55g1_skip_of_populate = true;

	/* Stock flattened DT used /soc; mainline uses /soc@0 (kept above). */
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

	pr_emerg("rg55g1: DT sanitize disabled %d nodes (kept USB/GCC/SDHCI/display)\n", n);
	return n;
}
EXPORT_SYMBOL_GPL(rg55g1_sanitize_dt);

/*
 * PM7250B SMB5 (smb5-reg.h):
 *   CHGR @ 0x1000 / DCDC @ 0x1100 / USBIN @ 0x1300 / TYPEC @ 0x1500
 * Current step for FCC/ICL is 50 mA.
 */
#define RG55_CHGR_BASE			0x1000
#define RG55_CHGR_STATUS_1		(RG55_CHGR_BASE + 0x06)
#define RG55_CHGR_STATUS_MASK		GENMASK(2, 0)
#define RG55_CHGR_INHIBIT		0
#define RG55_CHGR_TRICKLE		1
#define RG55_CHGR_PRE			2
#define RG55_CHGR_FULLON		3
#define RG55_CHGR_TAPER			4
#define RG55_CHGR_TERMINATE		5
#define RG55_CHGR_PAUSE			6
#define RG55_CHGR_DISABLE		7
#define RG55_CHGR_ENABLE		(RG55_CHGR_BASE + 0x42)
#define RG55_CHGR_ENABLE_BIT		BIT(0)
#define RG55_CHGR_PAUSE_CMD		(RG55_CHGR_BASE + 0x43)
#define RG55_CHGR_CFG2			(RG55_CHGR_BASE + 0x51)
#define RG55_CHGR_CHG_EN_SRC		BIT(7)	/* 0 = SW charging enable */
#define RG55_CHGR_FCC_CFG		(RG55_CHGR_BASE + 0x61)
#define RG55_CHGR_CUR_STEP_UA		50000
#define RG55_CHGR_FCC_1A		(1000000 / RG55_CHGR_CUR_STEP_UA)

#define RG55_DCDC_BASE			0x1100
#define RG55_DCDC_TYPE			(RG55_DCDC_BASE + 0x04)
#define RG55_DCDC_SUBTYPE		(RG55_DCDC_BASE + 0x05)
#define RG55_POWER_PATH_STATUS		(RG55_DCDC_BASE + 0x0B)
#define RG55_PP_USBIN_SUSPEND_STS	BIT(6)
#define RG55_PP_USE_USBIN		BIT(4)
#define RG55_PP_VALID_INPUT		BIT(0)
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

#define RG55_USBIN_BASE			0x1300
#define RG55_USBIN_INT_RT_STS		(RG55_USBIN_BASE + 0x10)
#define RG55_USBIN_PLUGIN_RT		BIT(4)
#define RG55_APSD_STATUS		(RG55_USBIN_BASE + 0x07)
#define RG55_APSD_DONE			BIT(0)
#define RG55_APSD_RESULT		(RG55_USBIN_BASE + 0x08)
#define RG55_APSD_DCP			BIT(3)
#define RG55_APSD_CDP			BIT(2)
#define RG55_APSD_SDP			BIT(0)
#define RG55_USB_CMD_IL			(RG55_USBIN_BASE + 0x40) /* USBIN_CMD_IL */
#define RG55_USB_SUSPEND		BIT(0)
#define RG55_USBIN_ICL_OVERRIDE		(RG55_USBIN_BASE + 0x42)
#define RG55_ICL_OVERRIDE_BIT		BIT(0)
#define RG55_CMD_APSD			(RG55_USBIN_BASE + 0x41)
#define RG55_APSD_RERUN			BIT(0)
#define RG55_USBIN_ICL_OPTIONS		(RG55_USBIN_BASE + 0x66)
#define RG55_USB51_MODE			BIT(1)
#define RG55_USBIN_MODE_CHG		BIT(0)
#define RG55_USBIN_ICL_CFG		(RG55_USBIN_BASE + 0x70)
#define RG55_ICL_500MA			(500000 / RG55_CHGR_CUR_STEP_UA)
#define RG55_ICL_1500MA			(1500000 / RG55_CHGR_CUR_STEP_UA)
#define RG55_ICL_3000MA			(3000000 / RG55_CHGR_CUR_STEP_UA)

/* Aliases kept for older scan logs that looked for "OTG@0x1100". */
#define RG55_OTG_BASE			RG55_DCDC_BASE
#define RG55_OTG_TYPE			RG55_DCDC_TYPE
#define RG55_OTG_SUBTYPE		RG55_DCDC_SUBTYPE

#define RG55_TYPEC_BASE			0x1500
#define RG55_TYPEC_TYPE			(RG55_TYPEC_BASE + 0x04)
#define RG55_TYPEC_SNK_STATUS		(RG55_TYPEC_BASE + 0x06)
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
#define RG55_TYPEC_EN_TRY_SNK		BIT(4)
#define RG55_TYPEC_RP_1P5		0x1	/* TYPEC_SRC_RP_1P5A */
#define RG55_TYPEC_SEL_SRC_UPPER_REF	BIT(2)
#define RG55_TYPEC_VCONN_EN_SRC		BIT(0)
#define RG55_TYPEC_DBG_SRC_EN		BIT(0)
#define RG55_TYPEC_VBUS_DETECT		RG55_TYPEC_SNK_SRC_MODE
#define RG55_TYPEC_SRC_RD_OPEN		BIT(3)
#define RG55_SNK_RP_STD			BIT(3)
#define RG55_SNK_RP_1P5			BIT(2)
#define RG55_SNK_RP_3P0			BIT(1)

/* PM7250B Qualcomm Gauge @ 0x4800 (qg-reg.h / qg-defs.h). */
#define RG55_QG_BASE			0x4800
#define RG55_QG_TYPE			(RG55_QG_BASE + 0x04)
#define RG55_QG_SUBTYPE			(RG55_QG_BASE + 0x05)
#define RG55_QG_STATUS1			(RG55_QG_BASE + 0x08)
#define RG55_QG_OK			BIT(7)
#define RG55_QG_BATT_PRESENT		BIT(0)
#define RG55_QG_TYPE_VAL		0x0D
#define RG55_QG_SOC_MONOTONIC		(RG55_QG_BASE + 0xBF)
#define RG55_QG_LAST_ADC_V		(RG55_QG_BASE + 0xC0)
#define RG55_QG_LAST_ADC_I		(RG55_QG_BASE + 0xC2)
#define RG55_QG_S2_AVG_V		(RG55_QG_BASE + 0x80)
#define RG55_QG_V_RAW_TO_UV(raw)	div_u64(194637ULL * (u64)(raw), 1000)
#define RG55_QG_FIFO_RESET_RAW		0x8000
/* Voltage→SOC when ADSP gauge is idle (monotonic SOC is often stale 100%). */
#define RG55_VBATT_EMPTY_UV		3400000
#define RG55_VBATT_FULL_UV		4200000
#define RG55_VBATT_MIN_VALID_UV		2500000
#define RG55_VBATT_MAX_VALID_UV		4600000

enum rg55_vbus_role {
	RG55_VBUS_ROLE_NONE = 0,
	RG55_VBUS_ROLE_OTG,
	RG55_VBUS_ROLE_CHG,
	RG55_VBUS_ROLE_READY,
};

struct rg55_batt_state {
	int capacity;		/* 0..100 */
	int voltage_uv;
	int current_ua;
	int status;		/* POWER_SUPPLY_STATUS_* */
	int health;
	bool present;
	bool usb_online;
	bool valid;
};

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

static int rg55g1_sid_readn(struct spmi_controller *ctrl, u8 sid, u16 addr,
			    u8 *buf, size_t len)
{
	struct spmi_device *sdev;
	int ret;

	if (!buf || !len || len > 16)
		return -EINVAL;
	sdev = spmi_device_alloc(ctrl);
	if (!sdev)
		return -ENOMEM;
	sdev->usid = sid;
	ret = spmi_ext_register_readl(sdev, addr, buf, len);
	put_device(&sdev->dev);
	return ret;
}

/*
 * Type-C keepalive — same port: sticky OTG (keyboard) XOR sink charge.
 *
 * Do NOT idle in try.SNK: flipping TRY_SNK↔SRC drops VBUS and yields the
 * k01/S1/A0/C0 ↔ RDY/trySNK oscillation (keyboard never reaches A1/C1).
 * Default is SRC+OTG (original working host path). Runtime charger probe runs
 * only when xHCI reports no device (keyboard stays up even if PMIC CC=A0/C0).
 */
static struct spmi_controller *vbus_keep_ctrl;
static u8 vbus_keep_sid;
static enum rg55_vbus_role vbus_keep_role = RG55_VBUS_ROLE_NONE;
static unsigned long vbus_chg_probe_at;
static unsigned int vbus_chg_idle_ticks;
static void rg55g1_vbus_keep_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(vbus_keep_work, rg55g1_vbus_keep_fn);
static void rg55g1_batt_update_and_show(struct spmi_controller *ctrl, u8 sid);
static int rg55g1_psy_register(void);
static bool rg55g1_vbus_partner_is_charger(u8 snk, u8 pp, u8 cmd, u8 plugin,
					  u8 apsd, bool is_src);
static bool rg55g1_vbus_probe_charger(struct spmi_controller *ctrl, u8 sid);

#define RG55_CHG_IDLE_TICKS		6	/* ~3s without xHCI device */
#define RG55_CHG_PROBE_PERIOD_MS	8000
#define RG55_CHG_FIRST_PROBE_MS		5000	/* after boot, before 1st probe */
#define RG55_BOOT_CHG_PROBE_MS		400
#define RG55_CHG_PROBE_WINDOW_MS	400

static void rg55g1_vbus_unsuspend_usbin(struct spmi_controller *ctrl, u8 sid)
{
	u8 il = 0;

	if (!rg55g1_sid_read(ctrl, sid, RG55_USB_CMD_IL, &il))
		(void)rg55g1_sid_write(ctrl, sid, RG55_USB_CMD_IL,
				       il & ~RG55_USB_SUSPEND);
}

/* Android PMIC Type-C init: VCONN + CC debounce before SRC/OTG. */
static void rg55g1_vbus_typec_host_prep(struct spmi_controller *ctrl, u8 sid)
{
	u8 exit = 0;

	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_VCONN_CTL,
			       RG55_TYPEC_VCONN_EN_SRC);
	if (!rg55g1_sid_read(ctrl, sid, RG55_TYPEC_EXIT_STATE, &exit))
		(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_EXIT_STATE,
				       exit | RG55_TYPEC_SEL_SRC_UPPER_REF | BIT(1));
}

static void rg55g1_vbus_force_otg(struct spmi_controller *ctrl, u8 sid)
{
	u8 eng = 0, il = 0, cfg = 0;

	rg55g1_vbus_typec_host_prep(ctrl, sid);

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
	vbus_keep_role = RG55_VBUS_ROLE_OTG;
}

/* Sink path: stop OTG, accept USBIN, enable CHGR (FCC 1A, ICL from Rp/APSD). */
static void rg55g1_vbus_force_charge(struct spmi_controller *ctrl, u8 sid)
{
	u8 snk = 0, apsd = 0, opts = 0, cfg2 = 0, icl = RG55_ICL_500MA;

	(void)rg55g1_sid_write(ctrl, sid, RG55_CMD_OTG, 0);
	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_DEBUG_SRC, 0);
	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_MODE_CFG,
			       RG55_TYPEC_EN_SNK_ONLY);

	rg55g1_vbus_unsuspend_usbin(ctrl, sid);

	/* SW controls charging enable (clear CHG_EN_SRC). */
	if (!rg55g1_sid_read(ctrl, sid, RG55_CHGR_CFG2, &cfg2)) {
		cfg2 &= ~RG55_CHGR_CHG_EN_SRC;
		(void)rg55g1_sid_write(ctrl, sid, RG55_CHGR_CFG2, cfg2);
	}

	(void)rg55g1_sid_write(ctrl, sid, RG55_CMD_APSD, RG55_APSD_RERUN);

	(void)rg55g1_sid_read(ctrl, sid, RG55_TYPEC_SNK_STATUS, &snk);
	(void)rg55g1_sid_read(ctrl, sid, RG55_APSD_RESULT, &apsd);

	if (snk & RG55_SNK_RP_3P0)
		icl = RG55_ICL_3000MA;
	else if (snk & RG55_SNK_RP_1P5)
		icl = RG55_ICL_1500MA;
	else if (snk & RG55_SNK_RP_STD)
		icl = RG55_ICL_500MA;
	else if (apsd & (RG55_APSD_DCP | RG55_APSD_CDP))
		icl = RG55_ICL_1500MA;
	else
		icl = RG55_ICL_1500MA; /* wall wart / unknown: try 1.5A */

	(void)rg55g1_sid_write(ctrl, sid, RG55_USBIN_ICL_CFG, icl);
	(void)rg55g1_sid_write(ctrl, sid, RG55_USBIN_ICL_OVERRIDE,
			       RG55_ICL_OVERRIDE_BIT);

	if (!rg55g1_sid_read(ctrl, sid, RG55_USBIN_ICL_OPTIONS, &opts)) {
		opts = (opts & ~RG55_USB51_MODE) | RG55_USBIN_MODE_CHG;
		(void)rg55g1_sid_write(ctrl, sid, RG55_USBIN_ICL_OPTIONS, opts);
	}

	(void)rg55g1_sid_write(ctrl, sid, RG55_CHGR_FCC_CFG, RG55_CHGR_FCC_1A);
	(void)rg55g1_sid_write(ctrl, sid, RG55_CHGR_PAUSE_CMD, 0);
	(void)rg55g1_sid_write(ctrl, sid, RG55_CHGR_ENABLE,
			       RG55_CHGR_ENABLE_BIT);
	vbus_keep_role = RG55_VBUS_ROLE_CHG;
}

static bool rg55g1_vbus_partner_is_charger(u8 snk, u8 pp, u8 cmd, u8 plugin,
					  u8 apsd, bool is_src)
{
	/* Partner advertises Rp → wall/PC source (we sink). */
	if (snk & (RG55_SNK_RP_STD | RG55_SNK_RP_1P5 | RG55_SNK_RP_3P0))
		return true;

	/* DCP/CDP only — bare SDP false-triggers against OTG. */
	if (apsd & (RG55_APSD_DCP | RG55_APSD_CDP))
		return true;

	if (cmd & RG55_OTG_EN)
		return false;
	if (plugin & RG55_USBIN_PLUGIN_RT)
		return true;
	if ((pp & RG55_PP_VALID_INPUT) && (pp & RG55_PP_USE_USBIN) &&
	    !(pp & RG55_PP_USBIN_SUSPEND_STS))
		return true;
	(void)is_src;
	return false;
}

/*
 * Brief sink window to detect a wall charger. Only call when xHCI has no
 * peripheral — flipping to SNK drops VBUS and disconnects a keyboard.
 */
static bool rg55g1_vbus_probe_charger_ms(struct spmi_controller *ctrl, u8 sid,
					  unsigned int ms)
{
	u8 snk = 0, pp = 0, cmd = 0, plugin = 0, apsd = 0;

	if (rg55g1_usb_host_port_connected)
		return false;

	(void)rg55g1_sid_write(ctrl, sid, RG55_CMD_OTG, 0);
	(void)rg55g1_sid_write(ctrl, sid, RG55_TYPEC_MODE_CFG,
			       RG55_TYPEC_EN_SNK_ONLY);
	rg55g1_vbus_unsuspend_usbin(ctrl, sid);
	(void)rg55g1_sid_write(ctrl, sid, RG55_CMD_APSD, RG55_APSD_RERUN);
	msleep(ms);

	(void)rg55g1_sid_read(ctrl, sid, RG55_TYPEC_SNK_STATUS, &snk);
	(void)rg55g1_sid_read(ctrl, sid, RG55_POWER_PATH_STATUS, &pp);
	(void)rg55g1_sid_read(ctrl, sid, RG55_CMD_OTG, &cmd);
	(void)rg55g1_sid_read(ctrl, sid, RG55_USBIN_INT_RT_STS, &plugin);
	(void)rg55g1_sid_read(ctrl, sid, RG55_APSD_RESULT, &apsd);

	return rg55g1_vbus_partner_is_charger(snk, pp, cmd, plugin, apsd, false);
}

static bool rg55g1_vbus_probe_charger(struct spmi_controller *ctrl, u8 sid)
{
	return rg55g1_vbus_probe_charger_ms(ctrl, sid, RG55_CHG_PROBE_WINDOW_MS);
}

static void rg55g1_vbus_keep_fn(struct work_struct *work)
{
	u8 cmd = 0, misc = 0, sm = 0, pp = 0, src = 0, snk = 0;
	u8 chg = 0, icl = 0, en = 0, plugin = 0, apsd = 0;
	char msg[28];
	u32 color;
	bool want_chg;

	if (!vbus_keep_ctrl)
		return;

	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_MISC_STATUS, &misc);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_SM_STATUS, &sm);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_POWER_PATH_STATUS, &pp);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_SRC_STATUS, &src);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_SNK_STATUS, &snk);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid, RG55_CMD_OTG, &cmd);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_USBIN_INT_RT_STS, &plugin);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_APSD_RESULT, &apsd);

	if (vbus_keep_role == RG55_VBUS_ROLE_CHG) {
		want_chg = rg55g1_vbus_partner_is_charger(snk, pp, cmd, plugin,
							  apsd, false);
	} else if (rg55g1_usb_host_port_connected) {
		/* Keyboard/peripheral on xHCI — stay host, never PROBE-CHG. */
		want_chg = false;
		vbus_chg_idle_ticks = 0;
		vbus_chg_probe_at = jiffies +
			msecs_to_jiffies(RG55_CHG_PROBE_PERIOD_MS);
	} else {
		want_chg = !!(snk & (RG55_SNK_RP_STD | RG55_SNK_RP_1P5 |
				     RG55_SNK_RP_3P0));

		if (!want_chg) {
			vbus_chg_idle_ticks++;
			if (vbus_chg_idle_ticks >= RG55_CHG_IDLE_TICKS &&
			    time_after(jiffies, vbus_chg_probe_at)) {
				rg55g1_status_vbus("PROBE-CHG", 0x00ffff00);
				if (rg55g1_vbus_probe_charger(vbus_keep_ctrl,
							      vbus_keep_sid))
					want_chg = true;
				vbus_chg_probe_at = jiffies +
					msecs_to_jiffies(RG55_CHG_PROBE_PERIOD_MS);
				vbus_chg_idle_ticks = 0;
			}
		} else {
			vbus_chg_idle_ticks = 0;
		}
	}

	if (want_chg) {
		if (vbus_keep_role != RG55_VBUS_ROLE_CHG)
			rg55g1_vbus_force_charge(vbus_keep_ctrl, vbus_keep_sid);
		else {
			rg55g1_vbus_unsuspend_usbin(vbus_keep_ctrl,
						   vbus_keep_sid);
			(void)rg55g1_sid_write(vbus_keep_ctrl, vbus_keep_sid,
					       RG55_CHGR_ENABLE,
					       RG55_CHGR_ENABLE_BIT);
		}
	} else {
		/* Re-apply full SRC+OTG every tick (original stable keyboard path). */
		rg55g1_vbus_force_otg(vbus_keep_ctrl, vbus_keep_sid);
	}

	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid, RG55_CMD_OTG, &cmd);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_MISC_STATUS, &misc);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_TYPEC_SM_STATUS, &sm);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_POWER_PATH_STATUS, &pp);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_CHGR_STATUS_1, &chg);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_USBIN_ICL_CFG, &icl);
	(void)rg55g1_sid_read(vbus_keep_ctrl, vbus_keep_sid,
			      RG55_CHGR_ENABLE, &en);

	if (vbus_keep_role == RG55_VBUS_ROLE_CHG) {
		/* CHG/c<stat>/i<icl>/p — charging when en + valid path */
		snprintf(msg, sizeof(msg), "CHG/c%X/i%02X/p%d",
			 (unsigned int)(chg & RG55_CHGR_STATUS_MASK), icl,
			 !!(pp & RG55_PP_VALID_INPUT));
		if ((en & RG55_CHGR_ENABLE_BIT) && (pp & RG55_PP_VALID_INPUT) &&
		    !(pp & RG55_PP_USBIN_SUSPEND_STS))
			color = 0x0000ff00;
		else if (misc & RG55_TYPEC_CC_ATTACHED)
			color = 0x00ffff00;
		else
			color = 0x00ff8000;
	} else if (vbus_keep_role == RG55_VBUS_ROLE_OTG) {
		/* k=CMD_OTG  S=source  A=attach  C=CC — want k01/S1/A1/C1 */
		snprintf(msg, sizeof(msg), "k%02X/S%d/A%d/C%d",
			 cmd,
			 !!(misc & RG55_TYPEC_SNK_SRC_MODE),
			 !!(sm & RG55_TYPEC_ATTACH_STATE),
			 !!(misc & RG55_TYPEC_CC_ATTACHED));
		if ((cmd & RG55_OTG_EN) && (misc & RG55_TYPEC_SNK_SRC_MODE) &&
		    (misc & RG55_TYPEC_CC_ATTACHED))
			color = 0x0000ff00;
		else if ((cmd & RG55_OTG_EN) && (misc & RG55_TYPEC_SNK_SRC_MODE))
			color = 0x00ffff00; /* VBUS up, waiting for CC */
		else
			color = 0x00ff8000;
	} else {
		snprintf(msg, sizeof(msg), "OTG-idle");
		color = 0x00808080;
	}
	rg55g1_status_vbus(msg, color);

	rg55g1_batt_update_and_show(vbus_keep_ctrl, vbus_keep_sid);

	schedule_delayed_work(&vbus_keep_work, msecs_to_jiffies(500));
}

/*
 * Minimal /sys/class/power_supply/{battery,usb} via PM7250B QG + CHGR.
 * Full qcom_battmgr needs ADSP charger_pd; this is SPMI bring-up only.
 */
static struct rg55_batt_state rg55_batt;
static struct power_supply *rg55_batt_psy;
static struct power_supply *rg55_usb_psy;
static bool rg55_qg_ok;
static u8 rg55_qg_sid;

static int rg55_uv_to_capacity(int uv)
{
	if (uv <= RG55_VBATT_EMPTY_UV)
		return 0;
	if (uv >= RG55_VBATT_FULL_UV)
		return 100;
	return (uv - RG55_VBATT_EMPTY_UV) * 100 /
	       (RG55_VBATT_FULL_UV - RG55_VBATT_EMPTY_UV);
}

static int rg55_qg_raw_to_uv(u16 raw)
{
	int uv;

	if (raw == 0 || raw == 0xffff || raw == RG55_QG_FIFO_RESET_RAW)
		return 0;
	uv = RG55_QG_V_RAW_TO_UV(raw);
	if (uv < RG55_VBATT_MIN_VALID_UV || uv > RG55_VBATT_MAX_VALID_UV)
		return 0;
	return uv;
}

static int rg55_read_vbatt_uv(struct spmi_controller *ctrl, u8 qsid)
{
	u8 vraw[2] = { 0, 0 };
	int uv = 0;

	if (!rg55g1_sid_readn(ctrl, qsid, RG55_QG_S2_AVG_V, vraw, 2)) {
		uv = rg55_qg_raw_to_uv(vraw[0] | ((u16)vraw[1] << 8));
		if (uv)
			return uv;
	}
	if (!rg55g1_sid_readn(ctrl, qsid, RG55_QG_LAST_ADC_V, vraw, 2))
		uv = rg55_qg_raw_to_uv(vraw[0] | ((u16)vraw[1] << 8));
	return uv;
}

static int rg55_chg_status_to_psy(u8 chg_stat, bool usb_online, bool otg)
{
	u8 st = chg_stat & RG55_CHGR_STATUS_MASK;

	if (otg)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (!usb_online)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	switch (st) {
	case RG55_CHGR_TRICKLE:
	case RG55_CHGR_PRE:
	case RG55_CHGR_FULLON:
	case RG55_CHGR_TAPER:
		return POWER_SUPPLY_STATUS_CHARGING;
	case RG55_CHGR_TERMINATE:
		return POWER_SUPPLY_STATUS_FULL;
	case RG55_CHGR_INHIBIT:
	case RG55_CHGR_PAUSE:
	case RG55_CHGR_DISABLE:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	default:
		return POWER_SUPPLY_STATUS_CHARGING;
	}
}

static void rg55g1_qg_probe(struct spmi_controller *ctrl, u8 chg_sid)
{
	u8 type = 0, st = 0;
	u8 sid;

	rg55_qg_ok = false;

	if (!rg55g1_sid_read(ctrl, chg_sid, RG55_QG_TYPE, &type) &&
	    type == RG55_QG_TYPE_VAL) {
		(void)rg55g1_sid_read(ctrl, chg_sid, RG55_QG_STATUS1, &st);
		rg55_qg_sid = chg_sid;
		rg55_qg_ok = true;
		pr_emerg("rg55g1: QG@0x4800 sid=%u status1=0x%02x\n",
			 chg_sid, st);
		return;
	}

	for (sid = 0; sid < 16; sid++) {
		if (sid == chg_sid)
			continue;
		if (!rg55g1_sid_read(ctrl, sid, RG55_QG_TYPE, &type) &&
		    type == RG55_QG_TYPE_VAL) {
			rg55_qg_sid = sid;
			rg55_qg_ok = true;
			pr_emerg("rg55g1: QG@0x4800 sid=%u (alt)\n", sid);
			return;
		}
	}
	pr_emerg("rg55g1: QG not found — voltage/SOC estimate only\n");
}

static void rg55g1_batt_sample(struct spmi_controller *ctrl, u8 chg_sid)
{
	u8 soc_raw = 0, chg = 0, pp = 0, en = 0, cmd = 0, st = 0;
	u8 plugin = 0;
	u8 qsid = rg55_qg_ok ? rg55_qg_sid : chg_sid;
	int uv = 0, pct = -1;
	bool usb_online, otg;
	u8 chg_st;

	(void)rg55g1_sid_read(ctrl, chg_sid, RG55_CHGR_STATUS_1, &chg);
	(void)rg55g1_sid_read(ctrl, chg_sid, RG55_POWER_PATH_STATUS, &pp);
	(void)rg55g1_sid_read(ctrl, chg_sid, RG55_CHGR_ENABLE, &en);
	(void)rg55g1_sid_read(ctrl, chg_sid, RG55_CMD_OTG, &cmd);
	(void)rg55g1_sid_read(ctrl, chg_sid, RG55_USBIN_INT_RT_STS, &plugin);

	otg = !!(cmd & RG55_OTG_EN) && vbus_keep_role == RG55_VBUS_ROLE_OTG;
	/*
	 * online = external USBIN in use, or we actively entered charge role.
	 * Do not force offline merely because role was OTG earlier.
	 */
	usb_online = (vbus_keep_role == RG55_VBUS_ROLE_CHG) ||
		     ((plugin & RG55_USBIN_PLUGIN_RT) && !otg) ||
		     ((pp & RG55_PP_VALID_INPUT) && (pp & RG55_PP_USE_USBIN) &&
		      !(pp & RG55_PP_USBIN_SUSPEND_STS) && !otg);

	uv = rg55_read_vbatt_uv(ctrl, qsid);

	if (rg55_qg_ok) {
		(void)rg55g1_sid_read(ctrl, qsid, RG55_QG_STATUS1, &st);
		(void)rg55g1_sid_read(ctrl, qsid, RG55_QG_SOC_MONOTONIC,
				      &soc_raw);
		rg55_batt.present = !!(st & RG55_QG_BATT_PRESENT) ||
				    !!(st & RG55_QG_OK) || uv > 2500000;

		/*
		 * Monotonic SOC needs ADSP qpnp-qg; without it the register
		 * often sits at 0xff/0xfe → fake 100%. Only trust mid-range
		 * values when QG_OK is set.
		 */
		if ((st & RG55_QG_OK) && soc_raw > 2 && soc_raw < 0xfd)
			pct = DIV_ROUND_CLOSEST((int)soc_raw * 100, 255);
	} else {
		rg55_batt.present = uv > 2500000;
	}

	if (pct < 0 && uv > 0)
		pct = rg55_uv_to_capacity(uv);
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;

	chg_st = chg & RG55_CHGR_STATUS_MASK;
	rg55_batt.capacity = pct;
	rg55_batt.voltage_uv = uv;
	rg55_batt.current_ua = 0;
	rg55_batt.usb_online = usb_online;
	rg55_batt.status = rg55_chg_status_to_psy(chg, usb_online, otg);
	/* Only clamp to 100 on real charge-terminate, not inhibit=0. */
	if (usb_online && chg_st == RG55_CHGR_TERMINATE)
		rg55_batt.capacity = 100;
	rg55_batt.health = rg55_batt.present ? POWER_SUPPLY_HEALTH_GOOD :
					       POWER_SUPPLY_HEALTH_DEAD;
	rg55_batt.valid = true;
}

static void rg55g1_batt_update_and_show(struct spmi_controller *ctrl, u8 sid)
{
	char msg[24];
	u32 color;
	int mv;

	if (!ctrl)
		return;

	rg55g1_batt_sample(ctrl, sid);

	mv = rg55_batt.voltage_uv / 1000;
	if (rg55_batt.status == POWER_SUPPLY_STATUS_CHARGING)
		snprintf(msg, sizeof(msg), "BAT:%d%% %d.%02dV+",
			 rg55_batt.capacity, mv / 1000, (mv % 1000) / 10);
	else if (rg55_batt.status == POWER_SUPPLY_STATUS_FULL)
		snprintf(msg, sizeof(msg), "BAT:FULL %d.%02dV",
			 mv / 1000, (mv % 1000) / 10);
	else
		snprintf(msg, sizeof(msg), "BAT:%d%% %d.%02dV",
			 rg55_batt.capacity, mv / 1000, (mv % 1000) / 10);

	if (rg55_batt.capacity <= 15 &&
	    rg55_batt.status != POWER_SUPPLY_STATUS_CHARGING)
		color = 0x00ff0000;
	else if (rg55_batt.status == POWER_SUPPLY_STATUS_CHARGING)
		color = 0x00ffff00;
	else if (rg55_batt.status == POWER_SUPPLY_STATUS_FULL)
		color = 0x0000ff00;
	else
		color = 0x0000c0ff;

	rg55g1_status_batt(msg, color);

	/* Only notify after class+psy exist (LV4 skip otherwise WARNs). */
	if (rg55_batt_psy)
		power_supply_changed(rg55_batt_psy);
	if (rg55_usb_psy)
		power_supply_changed(rg55_usb_psy);
}

static int rg55_batt_get_prop(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	if (!rg55_batt.valid)
		return -ENODATA;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = rg55_batt.status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = rg55_batt.health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = rg55_batt.present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = rg55_batt.capacity;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = rg55_batt.voltage_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = rg55_batt.current_ua;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int rg55_usb_get_prop(struct power_supply *psy,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	if (!rg55_batt.valid)
		return -ENODATA;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = rg55_batt.usb_online;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = rg55_batt.usb_online;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = rg55_batt.usb_online ?
			      POWER_SUPPLY_USB_TYPE_SDP :
			      POWER_SUPPLY_USB_TYPE_UNKNOWN;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property rg55_batt_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_SCOPE,
};

static enum power_supply_property rg55_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_SCOPE,
};

static const struct power_supply_desc rg55_batt_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = rg55_batt_props,
	.num_properties = ARRAY_SIZE(rg55_batt_props),
	.get_property = rg55_batt_get_prop,
};

static const struct power_supply_desc rg55_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = rg55_usb_props,
	.num_properties = ARRAY_SIZE(rg55_usb_props),
	.get_property = rg55_usb_get_prop,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP),
};

static int rg55g1_psy_register(void)
{
	if (rg55_batt_psy)
		return 0;

	rg55_batt_psy = power_supply_register(NULL, &rg55_batt_desc, NULL);
	if (IS_ERR(rg55_batt_psy)) {
		pr_err("rg55g1: battery psy register failed %ld\n",
		       PTR_ERR(rg55_batt_psy));
		rg55_batt_psy = NULL;
		return -ENODEV;
	}

	rg55_usb_psy = power_supply_register(NULL, &rg55_usb_desc, NULL);
	if (IS_ERR(rg55_usb_psy)) {
		pr_err("rg55g1: usb psy register failed %ld\n",
		       PTR_ERR(rg55_usb_psy));
		rg55_usb_psy = NULL;
		/* battery alone is still useful */
	}

	pr_emerg("rg55g1: power_supply battery%s registered\n",
		 rg55_usb_psy ? "+usb" : "");
	return 0;
}

/**
 * rg55g1_enable_pm7250b_vbus() - bring up PM7250B Type-C dual-role via SPMI.
 *
 * Main arb EE ownership blocks HLOS writes to PM7250B (SEA). Stock DT
 * exposes qcom,spmi-pmic-arb-debug which bypasses EE.
 * PM4450s are rails only — OTG boost + USBIN charger are on PM7250B.
 * Keepalive: sticky SRC+OTG (keyboard) or sink charge; no runtime PROBE-CHG.
 */
static int rg55g1_enable_pm7250b_vbus(void)
{
	static bool done;
	static char last[24] = "VBUS:?";
	static u32 last_color = 0x00808080;
	static int last_ret = -ENODEV;
	struct spmi_controller *main_ctrl, *dbg_ctrl;
	struct spmi_controller *ctrl;
	u8 sid, pmic_type, pmic_sub, type, subtype, cmd;
	int found = 0, otg_sid = -1;
	int ret;

	/* Always refresh sticky line (USB spam used to scroll it away). */
	if (vbus_keep_ctrl) {
		rg55g1_status_vbus(last, last_color);
		return 0;
	}
	/*
	 * Do not latch done on NO-SPMI — SPMI may probe after the first
	 * EXEC-INIT refresh (mainline DT used to lack arb nodes entirely).
	 */
	if (done) {
		rg55g1_status_vbus(last, last_color);
		return last_ret;
	}

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
		/* Allow retry once controllers appear. */
		return last_ret;
	}

	done = true;

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

	/* Probe write path (debug arb preferred), then enter dual-role. */
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
	 * Write path OK. Sticky OTG by default (keyboard). If a charger is
	 * already present, enter sink charge instead.
	 */
	{
		u8 ttype = 0, misc = 0, sm = 0, pp = 0;
		u8 otg_type = 0, otg_sub = 0;
		u8 snk = 0, plugin = 0, apsd = 0;

		(void)rg55g1_sid_write(ctrl, otg_sid, RG55_CMD_OTG, 0);

		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_OTG_TYPE, &otg_type);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_OTG_SUBTYPE, &otg_sub);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_TYPE, &ttype);
		pr_emerg("rg55g1: DCDC TYPE=0x%02x SUB=0x%02x TYPEC_TYPE=0x%02x\n",
			 otg_type, otg_sub, ttype);

		/* One short sink peek so boot-with-charger works. */
		rg55g1_status_vbus("PROBE-CHG", 0x00ffff00);
		if (rg55g1_vbus_probe_charger_ms(ctrl, otg_sid,
						 RG55_BOOT_CHG_PROBE_MS)) {
			rg55g1_vbus_force_charge(ctrl, otg_sid);
		} else {
			rg55g1_status_vbus("TYPEC-OTG", 0x00ffff00);
			rg55g1_vbus_force_otg(ctrl, otg_sid);
		}
		msleep(50);

		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_MISC_STATUS,
				      &misc);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_SM_STATUS, &sm);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_POWER_PATH_STATUS,
				      &pp);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_CMD_OTG, &cmd);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_TYPEC_SNK_STATUS, &snk);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_USBIN_INT_RT_STS,
				      &plugin);
		(void)rg55g1_sid_read(ctrl, otg_sid, RG55_APSD_RESULT, &apsd);
		(void)snk;
		(void)plugin;
		(void)apsd;

		pr_emerg("rg55g1: dual-role misc=0x%02x sm=0x%02x pp=0x%02x "
			 "CMD=0x%02x role=%d\n",
			 misc, sm, pp, cmd, vbus_keep_role);

		vbus_keep_ctrl = ctrl;
		vbus_keep_sid = otg_sid;
		vbus_chg_probe_at = jiffies +
			msecs_to_jiffies(RG55_CHG_FIRST_PROBE_MS);
		vbus_chg_idle_ticks = 0;
		rg55g1_qg_probe(ctrl, otg_sid);
		(void)rg55g1_psy_register();
		rg55g1_batt_update_and_show(ctrl, otg_sid);
		schedule_delayed_work(&vbus_keep_work, msecs_to_jiffies(500));

		if (vbus_keep_role == RG55_VBUS_ROLE_CHG) {
			snprintf(last, sizeof(last), "CHG-s%d", otg_sid);
			last_color = 0x0000ff00;
			last_ret = 0;
		} else if (vbus_keep_role == RG55_VBUS_ROLE_OTG &&
			   (cmd & RG55_OTG_EN) &&
			   (misc & RG55_TYPEC_SNK_SRC_MODE)) {
			snprintf(last, sizeof(last), "k%02X/S%d/A%d/C%d",
				 cmd,
				 !!(misc & RG55_TYPEC_SNK_SRC_MODE),
				 !!(sm & RG55_TYPEC_ATTACH_STATE),
				 !!(misc & RG55_TYPEC_CC_ATTACHED));
			last_color = 0x0000ff00;
			last_ret = 0;
		} else {
			snprintf(last, sizeof(last), "RDY-s%d", otg_sid);
			last_color = 0x00808080;
			last_ret = 0;
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
	/*
	 * Mainline sm4450.dtsi apps_rsc already has valid 8-cell tcs-config —
	 * keep it for rpmh regulators (SD/USB supplies). Only sanitize stock.
	 */
	rsc_np = of_find_node_by_path("/soc@0/rsc@17a00000");
	if (!rsc_np)
		rsc_np = of_find_node_by_path("/soc/rsc@17a00000");
	if (rsc_np) {
		int ne = of_property_count_u32_elems(rsc_np, "qcom,tcs-config");

		pr_emerg("rg55g1: tcs-config elems=%d (need 8) mainline=%d\n",
			 ne, rg55g1_is_mainline_board());
		if (ne != 8)
			rg55g1_set_u32_array_prop(rsc_np, "qcom,tcs-config",
						  tcs_fix, ARRAY_SIZE(tcs_fix));
		if (!rg55g1_is_mainline_board()) {
			rg55g1_remove_prop(rsc_np, "power-domains");
			rg55g1_force_disabled_node(rsc_np);
		}
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
 * rg55g1_enable_sd_rails() - SD slot LDOs (stock dtbo fragment@26).
 *
 * L24B (ldob24) ~2.96V vmmc, L28B (ldob28) 1.8V vqmmc.
 */
static int rg55g1_enable_sd_rails(void)
{
	static const struct {
		const char *name;
		u32 uv;
	} rails[] = {
		{ "ldob24", 2960000 },
		{ "ldob28", 1800000 },
	};
	struct tcs_cmd cmds[4];
	int i, n = 0, ret, ok = 0, tries;

	for (tries = 0; tries < 20; tries++) {
		if (!cmd_db_ready())
			break;
		rg55g1_status("SD-LDO?", 0x00ff8000);
		msleep(50);
	}
	if (cmd_db_ready()) {
		rg55g1_status("SD-LDO!", 0x00ff0000);
		pr_emerg("rg55g1: cmd-db not ready, skip SD LDO vote\n");
		return -EAGAIN;
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
	}

	if (!n)
		return -ENOENT;

	ret = rg55g1_rpmh_raw_cmds(cmds, n);
	pr_emerg("rg55g1: SD RPMh vote (%d cmds, %d rails) -> %d\n", n, ok, ret);
	usleep_range(2000, 3000);
	return ret;
}

/**
 * rg55g1_sdcc_clk_force() - ungated SDCC2 like USB phy bring-up.
 */
static void rg55g1_sdcc_clk_force(void)
{
	struct device_node *gcc_np;
	void __iomem *gcc;
	u32 bcr;
	int i;
	static const u32 clk_ids[] = {
		GCC_SDCC2_AHB_CLK,
		GCC_SDCC2_APPS_CLK,
		GCC_SDCC2_APPS_CLK_SRC,
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

	gcc = ioremap(0x00100000, 0xa0000);
	if (!gcc) {
		pr_emerg("rg55g1: GCC ioremap failed (SDCC)\n");
		return;
	}

	writel(1, gcc + 0x2400c); /* GCC_SDCC2_AHB_CBCR */
	writel(1, gcc + 0x24004); /* GCC_SDCC2_APPS_CBCR */

	bcr = readl(gcc + 0x24000); /* GCC_SDCC2_BCR */
	writel(1, gcc + 0x24000);
	udelay(200);
	writel(0, gcc + 0x24000);
	udelay(200);

	pr_emerg("rg55g1: SDCC2 BCR was 0x%x -> 0, clocks forced\n", bcr);
	rg55g1_status("SD-CLK", 0x00ffff00);
	iounmap(gcc);
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
	int tries;

	np = of_find_compatible_node(NULL, NULL, "qcom,scm");
	if (!np) {
		pr_emerg("rg55g1: qcom,scm DT node missing\n");
		return;
	}

	rg55g1_force_status(np, "okay");
	/* Flattened stock phandle may break of_address_to_resource. */
	rg55g1_remove_prop(np, "qcom,dload-mode");
	rg55g1_remove_prop(np, "interconnects");
	rg55g1_remove_prop(np, "interconnect-names");

	pdev = of_find_device_by_node(np);
	if (pdev) {
		put_device(&pdev->dev);
		pr_emerg("rg55g1: scm already present\n");
	} else if (!of_platform_device_create(np, NULL, NULL)) {
		pr_emerg("rg55g1: scm create failed\n");
	} else {
		pr_emerg("rg55g1: scm created\n");
	}

	for (tries = 0; tries < 20 && !qcom_scm_is_available(); tries++) {
		driver_deferred_probe_trigger();
		msleep(50);
	}
	pr_emerg("rg55g1: scm available=%d\n", qcom_scm_is_available());
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

/**
 * rg55g1_smmu_install_exact() - exact SID SMR that wins first-match.
 *
 * Same rules as USB: never clear VALID on broad entries; relocate if needed.
 */
static int rg55g1_smmu_install_exact(void __iomem *smmu, u32 numsmr, u32 sid,
				     u32 want_s2cr, const char *tag)
{
	u32 smr, s2cr, old_s2cr;
	int first = -1, free_smr = -1, target = -1;
	unsigned int i;

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
		if (first < 0)
			first = i;
	}

	if (first < 0) {
		if (free_smr < 0)
			return -ENOENT;
		target = free_smr;
	} else {
		smr = readl_relaxed(smmu + 0x800 + (first << 2));
		if (((smr >> 16) & 0x7fff) == 0 && (smr & 0xffff) == sid) {
			target = first;
		} else if (free_smr >= 0 && free_smr < first) {
			target = free_smr;
		} else if (free_smr > first) {
			old_s2cr = readl_relaxed(smmu + 0xc00 + (first << 2));
			writel_relaxed(smr, smmu + 0x800 + (free_smr << 2));
			writel_relaxed(old_s2cr, smmu + 0xc00 + (free_smr << 2));
			if (readl_relaxed(smmu + 0x800 + (free_smr << 2)) != smr ||
			    readl_relaxed(smmu + 0xc00 + (free_smr << 2)) != old_s2cr)
				return -EPERM;
			target = first;
			pr_emerg("rg55g1: %s relocated SMR[%d]→[%d]\n",
				 tag, first, free_smr);
		} else {
			return -ENOSPC;
		}
	}

	writel_relaxed(BIT(31) | sid, smmu + 0x800 + (target << 2));
	writel_relaxed(want_s2cr, smmu + 0xc00 + (target << 2));

	smr = readl_relaxed(smmu + 0x800 + (target << 2));
	s2cr = readl_relaxed(smmu + 0xc00 + (target << 2));
	if (!(smr & BIT(31)) || (smr & 0xffff) != sid)
		return -EPERM;

	pr_emerg("rg55g1: SMMU %s SMR[%d]=0x%x s2cr=0x%x\n",
		 tag, target, smr, s2cr);
	return target;
}

static int rg55g1_bringup_apps_smmu(void)
{
	void __iomem *gcc, *smmu;
	u32 id0, id1, scr0, want_s2cr, cbndx = 0;
	u32 numsmr;
	int ret;

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

	want_s2cr = FIELD_PREP(GENMASK(17, 16), 0) |
		    FIELD_PREP(GENMASK(7, 0), cbndx);

	/* MMC first (lower SMR slots), then USB, then display SID. */
	ret = rg55g1_smmu_install_exact(smmu, numsmr, 0x140, want_s2cr, "SD");
	if (ret < 0)
		pr_emerg("rg55g1: SD SID install failed %d\n", ret);
	ret = rg55g1_smmu_install_exact(smmu, numsmr, 0x560, want_s2cr, "eMMC");
	if (ret < 0)
		pr_emerg("rg55g1: eMMC SID install failed %d\n", ret);
	ret = rg55g1_smmu_install_exact(smmu, numsmr, 0x540, want_s2cr, "USB");
	if (ret < 0) {
		pr_emerg("rg55g1: USB SID install failed %d\n", ret);
		rg55g1_status("SMMU-SMR!", 0x00ff8000);
		iounmap(smmu);
		return ret;
	}
	/*
	 * Do not rewrite MDSS SID 0x800 if ABL already programmed it — touching
	 * a live display SMR can hang the bus. Only install when unmatched.
	 */
	{
		bool mdss_present = false;
		unsigned int i;

		for (i = 0; i < numsmr; i++) {
			u32 smr = readl_relaxed(smmu + 0x800 + (i << 2));
			u32 id, mask;

			if (!(smr & BIT(31)))
				continue;
			id = smr & 0xffff;
			mask = (smr >> 16) & 0x7fff;
			if ((0x800 & ~mask) == (id & ~mask)) {
				mdss_present = true;
				break;
			}
		}
		if (mdss_present)
			pr_emerg("rg55g1: MDSS SID already programmed — leave alone\n");
		else {
			ret = rg55g1_smmu_install_exact(smmu, numsmr, 0x800,
							want_s2cr, "MDSS");
			if (ret < 0)
				pr_emerg("rg55g1: MDSS SID install failed %d\n",
					 ret);
		}
	}

	pr_emerg("rg55g1: SMMU bypass cb=%u sCR0=0x%x\n", cbndx, scr0);
	rg55g1_status("SMMU-BYP", 0x0000ff00);
	iounmap(smmu);
	return 0;
}

/**
 * rg55g1_bringup_mmc() - create SDHCI platform devices after GCC is up.
 *
 * Stock Android clock indices differ from mainline sm4450-gcc.h; rewrite
 * clocks/resets, strip iommu/icc/regulator deps that cause probe defer.
 */
static int rg55g1_bringup_mmc(void)
{
	struct device_node *soc, *child, *gcc;
	u32 gcc_ph = 0;
	int n = 0, i;

	rg55g1_status("MMC-PREP", 0x00ff8000);
	(void)rg55g1_enable_sd_rails();
	rg55g1_sdcc_clk_force();

	gcc = of_find_compatible_node(NULL, NULL, "qcom,sm4450-gcc");
	if (!gcc)
		gcc = of_find_compatible_node(NULL, NULL, "qcom,ravelin-gcc");
	if (gcc) {
		gcc_ph = gcc->phandle;
		of_node_put(gcc);
	}

	soc = rg55g1_find_soc_node();
	if (!soc) {
		rg55g1_status("MMC-NOSOC", 0x00ff0000);
		return -ENODEV;
	}

	for_each_child_of_node(soc, child) {
		bool emmc;
		bool mainline;
		u32 clocks[4], resets[2];

		if (!rg55g1_is_sdhci(child))
			continue;

		emmc = child->full_name &&
		       (strstr(child->full_name, "7c4000") ||
			strstr(child->full_name, "7C4000"));

		/* RG55G1 has SD slot only — no soldered eMMC. */
		if (emmc) {
			rg55g1_force_status(child, "disabled");
			continue;
		}

		rg55g1_force_status(child, "okay");
		mainline = of_device_is_compatible(child, "qcom,sm4450-sdhci") &&
			   of_property_present(child, "vmmc-supply");

		/*
		 * Apps-SMMU is MMIO-bypassed; do not bind arm-smmu. Drop ICC
		 * (provider may not be up under LV6 skip).
		 */
		rg55g1_remove_prop(child, "iommus");
		rg55g1_remove_prop(child, "qcom,iommu-dma");
		rg55g1_remove_prop(child, "qcom,iommu-dma-addr-pool");
		rg55g1_remove_prop(child, "qcom,iommu-geometry");
		rg55g1_remove_prop(child, "interconnects");
		rg55g1_remove_prop(child, "interconnect-names");
		rg55g1_remove_prop(child, "operating-points-v2");
		rg55g1_remove_prop(child, "supports-cqe");
		rg55g1_remove_prop(child, "vdd-supply");
		rg55g1_remove_prop(child, "vdd-io-supply");
		rg55g1_remove_prop(child, "vdd-en-dis-supply");
		rg55g1_remove_prop(child, "vdd-io-en-dis-supply");
		rg55g1_remove_prop(child, "qcom,dll-hsr-list");
		rg55g1_remove_prop(child, "qcom,ice-clk-rates");
		rg55g1_remove_prop(child, "qcom,devfreq,freq-table");
		rg55g1_remove_prop(child, "broken-cd");
		rg55g1_set_string_prop(child, "qcom,force-pio", "");

		if (mainline) {
			/* Keep vmmc/vqmmc/pinctrl/cd-gpios from mainline board DT. */
			pr_emerg("rg55g1: MMC mainline path for %pOF\n", child);
		} else {
			/*
			 * Stock: SD stays in slot; avoid broken-cd polling.
			 * Strip Android regulator/pinctrl phandles that defer.
			 */
			rg55g1_set_string_prop(child, "non-removable", "");
			rg55g1_remove_prop(child, "vmmc-supply");
			rg55g1_remove_prop(child, "vqmmc-supply");
			rg55g1_remove_prop(child, "pinctrl-0");
			rg55g1_remove_prop(child, "pinctrl-1");
			rg55g1_remove_prop(child, "pinctrl-names");
			rg55g1_remove_prop(child, "cd-gpios");
			rg55g1_set_u32_prop(child, "max-frequency", 25000000);

			if (gcc_ph) {
				clocks[0] = gcc_ph;
				clocks[1] = GCC_SDCC2_AHB_CLK;
				clocks[2] = gcc_ph;
				clocks[3] = GCC_SDCC2_APPS_CLK;
				resets[0] = gcc_ph;
				resets[1] = GCC_SDCC2_BCR;
				rg55g1_set_u32_array_prop(child, "clocks", clocks, 4);
				rg55g1_set_u32_array_prop(child, "resets", resets, 2);
				{
					static const char names[] = "iface\0core";
					struct property *pp;
					char *val;

					val = kmemdup(names, sizeof(names), GFP_KERNEL);
					if (val) {
						pp = kzalloc(sizeof(*pp), GFP_KERNEL);
						if (pp) {
							pp->name = "clock-names";
							pp->length = sizeof(names);
							pp->value = val;
							of_update_property(child, pp);
						} else {
							kfree(val);
						}
					}
				}
				rg55g1_set_string_prop(child, "reset-names",
							"core_reset");
			}
		}

		if (!of_platform_device_create(child, NULL, NULL)) {
			struct platform_device *exist =
				of_find_device_by_node(child);

			if (exist) {
				put_device(&exist->dev);
				n++;
				pr_emerg("rg55g1: already-up %pOF\n", child);
			} else {
				pr_emerg("rg55g1: mmc create failed for %pOF\n",
					 child);
			}
		} else {
			n++;
			pr_emerg("rg55g1: populated %pOF (%s)\n", child,
				 emmc ? "eMMC" : "SD");
		}
	}
	of_node_put(soc);

	/* Sync SDHCI probe; poll until mmcblk shows (or timeout).
	 * Do not wait_for_device_probe() — USB async probes can block forever.
	 */
	driver_deferred_probe_trigger();
	for (i = 0; i < 40; i++) {
		struct device *d;

		d = class_find_device_by_name(&block_class, "mmcblk0");
		if (!d)
			d = class_find_device_by_name(&block_class, "mmcblk1");
		if (d) {
			put_device(d);
			pr_emerg("rg55g1: mmcblk ready after %d00ms\n", i);
			rg55g1_status("MMC-BLK", 0x0000ff00);
			break;
		}
		if (i == 0 || i == 39)
			pr_emerg("rg55g1: waiting for mmcblk* (%d/40)\n", i + 1);
		msleep(100);
		if ((i & 3) == 3)
			driver_deferred_probe_trigger();
	}
	if (i >= 40) {
		pr_emerg("rg55g1: no mmcblk* after MMC bringup\n");
		rg55g1_status("MMC-NOBLK", 0x00ff0000);
	}

	rg55g1_status(n ? "MMC-DEV" : "MMC-NONE",
		      n ? 0x0000ff00 : 0x00ff0000);
	return n;
}

static int rg55g1_create_of_dev(struct device_node *np, const char *tag)
{
	if (!np)
		return 0;

	rg55g1_force_status(np, "okay");
	if (!of_platform_device_create(np, NULL, NULL)) {
		struct platform_device *exist = of_find_device_by_node(np);

		if (exist) {
			put_device(&exist->dev);
			pr_emerg("rg55g1: already-up %s %pOF\n", tag, np);
			return 1;
		}
		pr_emerg("rg55g1: create failed %s %pOF\n", tag, np);
		return 0;
	}
	pr_emerg("rg55g1: populated %s %pOF\n", tag, np);
	return 1;
}

/**
 * rg55g1_bringup_keys() - power/volume keys -> /dev/input/event*
 *
 * Stock overlay: pmk8350 pwrkey/resin + gpio-keys vol_up@gpio53.
 * Do not use gpio-keys / pinctrl-sm4450 (TLMM gpiochip panics).
 */
static int rg55g1_bringup_keys(void)
{
	struct device_node *keys;
	int n = 0;

	rg55g1_status("KEY-PREP", 0x00ff8000);

	keys = of_find_compatible_node(NULL, NULL, "anbernic,rg55g1-keys");
	if (!keys) {
		rg55g1_status("KEY-NODT", 0x00ff0000);
		pr_emerg("rg55g1: rg55g1-keys DT node missing\n");
		return 0;
	}

	rg55g1_force_status(keys, "okay");
	n += rg55g1_create_of_dev(keys, "keys");
	of_node_put(keys);

	driver_deferred_probe_trigger();
	msleep(50);

	rg55g1_status(n ? "KEY-OK" : "KEY-FAIL",
		      n ? 0x0000ff00 : 0x00ff0000);
	pr_emerg("rg55g1: keys bringup done (%d)\n", n);
	return n;
}

/**
 * rg55g1_bringup_joypad() - clocks/pinmux + singleadc-joypad for /dev/input
 *
 * Do not probe qcom,sm4450-tlmm (gpiochip panics). Buttons + MCU SPI use
 * direct TLMM/GENI MMIO inside the joypad driver — skip full spi-geni
 * platform probe (firmware/ICC hang on this bring-up).
 */
static void rg55g1_enable_qup0_clks(void)
{
	void __iomem *gcc;
	u32 val;

	/* Voted branch enables in GCC_APCS_CLOCK_BRANCH_ENA_VOTE (0x62008) */
	gcc = ioremap(0x00100000, 0xa0000);
	if (!gcc)
		return;

	val = readl_relaxed(gcc + 0x62008);
	val |= BIT(6) | BIT(7) |	/* wrap_0 m/s ahb */
	       BIT(8) | BIT(9) |	/* wrap0 core / core_2x */
	       BIT(13);			/* wrap0_s3 */
	writel_relaxed(val, gcc + 0x62008);
	/* Ensure branch CBCR leave halt (set CLK_ENABLE bit0 if present) */
	writel_relaxed(readl_relaxed(gcc + 0x27004) | BIT(0), gcc + 0x27004);
	writel_relaxed(readl_relaxed(gcc + 0x27008) | BIT(0), gcc + 0x27008);
	writel_relaxed(readl_relaxed(gcc + 0x33000) | BIT(0), gcc + 0x33000);
	writel_relaxed(readl_relaxed(gcc + 0x3300c) | BIT(0), gcc + 0x3300c);
	writel_relaxed(readl_relaxed(gcc + 0x273a8) | BIT(0), gcc + 0x273a8);

	/*
	 * Force S3 RCG to 19.2 MHz (TCXO): CMD_RCGR@0x273b0 CFG=div1 src0.
	 * Leave M/N/D at 0 for integer mode.
	 */
	writel_relaxed(0x1, gcc + 0x273b4);		/* CFG_RCGR */
	writel_relaxed(0x1, gcc + 0x273b0);		/* CMD update */
	iounmap(gcc);
	pr_emerg("rg55g1: QUP0 clocks voted (s3 @19.2M for joy SPI)\n");
}

static void rg55g1_tlmm_scm_mux_spi_se3(void)
{
	/*
	 * gpio18-21 → qup0_se3 (mux=1). Match stock SPI active:
	 * drive-strength=6 (code 2), bias-pull-down, OE clear.
	 */
	static const int pins[] = { 18, 19, 20, 21 };
	void __iomem *tlmm;
	int i, ok = 0;

	tlmm = ioremap(0x0f100000UL, 0x300000UL);
	if (!tlmm) {
		pr_emerg("rg55g1: TLMM ioremap failed for SPI mux\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		u32 ctl = readl_relaxed(tlmm + pins[i] * 0x1000);

		ctl &= ~((0x7 << 2) | 0x3 | (0x7 << 6) | BIT(9));
		ctl |= (1 << 2);	/* mux qup0_se3 */
		ctl |= 0x1;		/* pull-down */
		ctl |= (2 << 6);	/* drive 6 mA */
		writel_relaxed(ctl, tlmm + pins[i] * 0x1000);
		ok++;
	}
	iounmap(tlmm);
	pr_emerg("rg55g1: SPI SE3 pins muxed (%d/4) drv=6 PD base=0xf100000\n",
		 ok);
}

/* Hall(42) + MCU 3V3(95) + RGB(41) — MCU must be powered for SPI ADC. */
static void rg55g1_joypad_mcu_power(void)
{
	static const int pins[] = { 42, 95, 41 };
	void __iomem *tlmm;
	int i;

	tlmm = ioremap(0x0f100000UL, 0x300000UL);
	if (!tlmm)
		return;

	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		u32 ctl = readl_relaxed(tlmm + pins[i] * 0x1000);

		ctl &= ~((0x7 << 2) | 0x3);
		ctl |= BIT(9); /* OE output */
		writel_relaxed(ctl, tlmm + pins[i] * 0x1000);
		writel_relaxed(BIT(1), tlmm + pins[i] * 0x1000 + 0x4); /* OUT */
	}
	iounmap(tlmm);
	msleep(80);
	pr_emerg("rg55g1: joypad MCU/hall/rgb power GPIOs driven high\n");
}

static int rg55g1_bringup_joypad(void)
{
	struct device_node *joy, *stock_spi, *stock_i2c;
	int n = 0;

	rg55g1_status("JOY-PREP", 0x00ff8000);

	rg55g1_enable_qup0_clks();
	rg55g1_tlmm_scm_mux_spi_se3();
	rg55g1_joypad_mcu_power();

	/* Keep stock SE3 siblings from fighting pinmux. */
	stock_spi = of_find_node_by_path("/soc/spi@98c000");
	if (stock_spi) {
		rg55g1_force_status(stock_spi, "disabled");
		of_node_put(stock_spi);
	}
	stock_i2c = of_find_node_by_path("/soc/i2c@98c000");
	if (stock_i2c) {
		rg55g1_force_status(stock_i2c, "disabled");
		of_node_put(stock_i2c);
	}

	/*
	 * Skip geni-se-qup / qcom,geni-spi platform devices: probe needs
	 * SE ELF firmware + ICC and previously hung bring-up. Joypad talks
	 * to SE3 via polled GENI FIFO MMIO when BL left SPI protocol loaded.
	 */
	rg55g1_status("JOY-SPI", 0x00ffff00);
	pr_emerg("rg55g1: SPI via joypad GENI MMIO (no spi-geni probe)\n");

	joy = of_find_compatible_node(NULL, NULL, "singleadc-joypad");
	if (!joy) {
		rg55g1_status("JOY-NODT", 0x00ff0000);
		pr_emerg("rg55g1: singleadc-joypad DT node missing\n");
		return n;
	}

	/* No TLMM pinctrl driver — strip so probe does not defer. */
	rg55g1_remove_prop(joy, "pinctrl-0");
	rg55g1_remove_prop(joy, "pinctrl-names");

	rg55g1_status("JOY-DEV", 0x00ffff00);
	n += rg55g1_create_of_dev(joy, "joypad");
	of_node_put(joy);

	driver_deferred_probe_trigger();
	msleep(100);

	rg55g1_status(n ? "JOY-OK" : "JOY-FAIL",
		      n ? 0x0000ff00 : 0x00ff0000);
	pr_emerg("rg55g1: joypad bringup done (%d)\n", n);
	return n;
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

	/* Type-C dual-role (OTG keyboard / USB sink charge) on PM7250B. */
	(void)rg55g1_enable_pm7250b_vbus();

	soc = rg55g1_find_soc_node();
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

	hsphy = of_find_compatible_node(NULL, NULL, "qcom,usb-snps-hs-7nm-phy");
	if (!hsphy)
		hsphy = of_find_compatible_node(NULL, NULL,
						"qcom,sm4450-usb-hs-phy");
	if (!hsphy)
		hsphy = of_find_compatible_node(NULL, NULL,
						"qcom,usb-hsphy-snps-femto");
	if (!hsphy)
		hsphy = of_find_compatible_node(NULL, NULL,
						"qcom,usb-snps-femto-v2-phy");
	if (hsphy)
		rg55g1_set_u32_prop(hsphy, "#phy-cells", 0);

	/*
	 * LV6 device_initcall is skipped on RG55G1; register usb-serial/ch341
	 * before xhci enumerates the CH340 on OTG.
	 */
	(void)rg55g1_usb_serial_bringup();

	/*
	 * Pass 0: HS PHY first so dwc3 can resolve phys.
	 * Pass 1: DWC3 glue.
	 */
	for (pass = 0; pass < 2; pass++) {
		for_each_child_of_node(soc, child) {
			bool is_gcc = of_device_is_compatible(child, "qcom,ravelin-gcc") ||
				      of_device_is_compatible(child, "qcom,sm4450-gcc");
			bool is_phy = rg55g1_is_hsphy(child);
			bool is_dwc = rg55g1_is_dwc_glue(child);

			if (!rg55g1_is_usb_keep(child))
				continue;
			if (is_gcc)
				continue; /* already created */
			if (rg55g1_is_smmu_node(child))
				continue; /* MMIO bypass only; never bind arm-smmu */
			if (rg55g1_is_display_node(child))
				continue; /* MSM display path owns these */
			if (rg55g1_is_sdhci(child))
				continue; /* created in rg55g1_bringup_mmc() */
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

	/* CH340 should bind right after deferred unblock; start log relay early. */
	rg55g1_log_export_start();

	/* Refresh dual-role after USB clocks/host are up. */
	rg55g1_status("USB-VBUS", 0x00ffff00);
	(void)rg55g1_enable_pm7250b_vbus();
	/* EUD may have been re-enabled by firmware; clear again. */
	rg55g1_disable_eud();
	rg55g1_usb_phy_clk_reset();
	rg55g1_hsphy_force_wake();
	/* Kick keepalive immediately for role re-decide */
	if (vbus_keep_ctrl)
		mod_delayed_work(system_wq, &vbus_keep_work, 0);

	/* SDHCI after GCC/SMMU so iface/core clocks and phys DMA work. */
	n += rg55g1_bringup_mmc();
	driver_deferred_probe_trigger();
	msleep(500);
	driver_deferred_probe_trigger();
	msleep(500);

	/* Power/volume keys need SPMI (same path as VBUS) + TLMM MMIO. */
	n += rg55g1_bringup_keys();

	/* Gamepad after GCC clocks are available for GENI SPI. */
	n += rg55g1_bringup_joypad();

#if IS_ENABLED(CONFIG_DRM_MSM_MDSS)
	/*
	 * Run MSM here (~4–5s, before exec /init) so early dmesg captures
	 * include DISPCC/MDSS lines. Requires rg55g1.msm=1 on cmdline.
	 */
	if (rg55g1_msm_auto) {
		int dret = rg55g1_msm_display_retry();

		if (!dret)
			pr_emerg("rg55g1: MSM display ready (USB bringup)\n");
		else if (dret != -ENODEV)
			pr_emerg("rg55g1: MSM display retry: %d (boot continues)\n",
				 dret);
	}
#endif

	/* Full ringbuffer dump after MSM/MMC/keys (~20s), not at ~2.5s. */
	rg55g1_log_export_flush();

	rg55g1_status(n ? "USB-DEV" : "USB-NONE", n ? 0x0000ff00 : 0x00ff0000);
	pr_emerg("rg55g1: USB bringup done: %d platform devs (see top VBUS line)\n",
		 n);
	{
		char sum[24];

		snprintf(sum, sizeof(sum), n ? "USB+%d" : "USB-0", n);
		rg55g1_status(sum, n ? 0x0000ff00 : 0x00ff0000);
	}
	return n;
}
EXPORT_SYMBOL_GPL(rg55g1_bringup_usb);

int rg55g1_vbus_refresh(void)
{
	return rg55g1_enable_pm7250b_vbus();
}
EXPORT_SYMBOL_GPL(rg55g1_vbus_refresh);

static int __init rg55g1_msm_early_setup(char *str)
{
	rg55g1_msm_early = true;
	return 0;
}
early_param("rg55g1.msm_early", rg55g1_msm_early_setup);

static int __init rg55g1_msm_auto_setup(char *str)
{
	rg55g1_msm_auto = true;
	return 0;
}
early_param("rg55g1.msm", rg55g1_msm_auto_setup);

static int __init rg55g1_lv6_setup(char *str)
{
	rg55g1_want_lv6 = true;
	rg55g1_block_deferred = false;
	rg55g1_skip_of_populate = false;
	pr_emerg("rg55g1: LV6+ initcalls enabled (rg55g1.lv6=1)\n");
	return 0;
}
early_param("rg55g1.lv6", rg55g1_lv6_setup);

static int __init rg55g1_vbus_after_devices(void)
{
	int i, ret = -ENODEV;

	/*
	 * Full LV6 path never calls rg55g1_bringup_usb(); enable OTG VBUS
	 * once SPMI has had a chance to probe so USB-HOST can enumerate.
	 * Also start CH340→ttyUSB dmesg mirror (that used to live in USB bringup).
	 */
	rg55g1_status("VBUS-IC", 0x00ff8000);
	rg55g1_usb_phy_clk_reset();
	rg55g1_hsphy_force_wake();
	rg55g1_disable_eud();
	(void)rg55g1_enable_hsphy_rails();
	/* LV6 never calls rg55g1_bringup_mmc() — still need SD clocks/rails. */
	rg55g1_sdcc_clk_force();
	(void)rg55g1_enable_sd_rails();
	for (i = 0; i < 8; i++) {
		ret = rg55g1_vbus_refresh();
		if (!ret)
			break;
		msleep(250);
	}
	pr_emerg("rg55g1: VBUS after devices: %d\n", ret);

	/* Start only — never flush here (flush slept 3s+ and stalled init). */
	rg55g1_log_export_start();
	return 0;
}
device_initcall_sync(rg55g1_vbus_after_devices);

static int __init rg55g1_bringup_early_flags(void)
{
	/*
	 * Default: skip LV6+ (stock of_platform hang). Mainline board DT is
	 * safe enough for full device_initcall — auto-enable when detected,
	 * or when the user passed rg55g1.lv6=1.
	 */
	if (rg55g1_is_mainline_board()) {
		rg55g1_want_lv6 = true;
		rg55g1_block_deferred = false;
		rg55g1_skip_of_populate = false;
		pr_emerg("rg55g1: mainline DT — LV6+ initcalls enabled\n");
		return 0;
	}
	if (!rg55g1_want_lv6) {
		rg55g1_block_deferred = true;
		rg55g1_skip_of_populate = true;
	}
	return 0;
}
early_initcall(rg55g1_bringup_early_flags);
