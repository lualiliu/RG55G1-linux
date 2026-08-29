/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_RG55G1_BRINGUP_H
#define _QCOM_RG55G1_BRINGUP_H

#include <linux/init.h>
#include <linux/types.h>

struct device_node;

int rg55g1_sanitize_dt(void);
int rg55g1_bringup_usb(void);
int rg55g1_vbus_refresh(void);
void rg55g1_force_node_okay(struct device_node *np);
void rg55g1_force_node_disabled(struct device_node *np);
void rg55g1_dt_remove_prop(struct device_node *np, const char *name);
int __init rg55g1_populate_msm_display(void);
int rg55g1_msm_display_retry(void);
int __init rg55g1_usb_serial_bringup(void);
void rg55g1_log_export_start(void);
void rg55g1_log_export_flush(void);

extern bool rg55g1_log_usb;

/* false = defer MSM to USB bringup (RPMh/GDSC ready); true = try at fs_initcall */
extern bool rg55g1_msm_early;

/* Post-init MSM retry (default off — use bootarg rg55g1.msm=1). */
extern bool rg55g1_msm_auto;

/* Sticky: panel was programmed by ABL — skip FT7131M re-init on modeset. */
extern bool rg55g1_abl_panel_ready;

/* Keep ABL continuous splash / GDSC during first attach (cleared after quiesce). */
extern bool rg55g1_preserve_abl_display;

/* Set once DPU CTL_FLUSH clears under ABL keepalive — GEM scanout is live. */
extern bool rg55g1_kms_scanout_ok;

void rg55g1_dispcc_quiesce_splash(void);

/* Ensure apps-SMMU MDSS SID 0x800 is phys-DMA bypass (ABL splash). */
int rg55g1_ensure_mdss_smmu_bypass(void);

/* Skip MDSS hardware reset while attaching during bring-up. */
extern bool rg55g1_skip_mdss_reset;

/* RG55G1 manually probes DPU/DSI/PHY; skip mdss of_platform_populate. */
extern bool rg55g1_skip_mdss_populate;

#if IS_ENABLED(CONFIG_SM_DISPCC_4450)
int rg55g1_dispcc_driver_register(void);
#endif

#if IS_ENABLED(CONFIG_SM_GPUCC_4450)
int rg55g1_gpucc_driver_register(void);
#endif

#if IS_ENABLED(CONFIG_DRM_MSM)
int rg55g1_msm_kms_register(void);
#endif

#if IS_ENABLED(CONFIG_DRM_PANEL_FOCALTECH_FT7131M)
int rg55g1_ft7131m_driver_register(void);
#endif

#endif
