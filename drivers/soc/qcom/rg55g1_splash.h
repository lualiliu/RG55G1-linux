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

int __init rg55g1_splash_fbdev_bringup(void);

#endif
