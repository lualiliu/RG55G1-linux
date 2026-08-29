/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_RG55G1_SPLASH_H
#define _QCOM_RG55G1_SPLASH_H

#include <linux/fb.h>
#include <linux/types.h>

extern void __iomem *rg55g1_splash_hw;
extern struct fb_info *rg55g1_splash_info;

void rg55g1_status(const char *msg, u32 color);
void rg55g1_status_vbus(const char *msg, u32 color);
void rg55g1_status_batt(const char *msg, u32 color);
void rg55g1_mark(u32 y, u32 color);

/*
 * Copy a 32bpp RGB rect into ABL scanout (0xb8000000). Used when MSM KMS
 * cannot reprogram SSPP (CTL_FLUSH stuck) but INTF still scans splash RAM.
 */
void rg55g1_splash_blit_rgb32(const void *src, unsigned int src_pitch,
			      unsigned int x1, unsigned int y1,
			      unsigned int x2, unsigned int y2);

/* Bright border + status so we can tell if ABL scanout is still alive. */
void rg55g1_splash_paint_alive(const char *tag);

int __init rg55g1_splash_fbdev_bringup(void);

#endif
