// SPDX-License-Identifier: GPL-2.0-only
/*
 * Anbernic RG55G1 continuous-splash framebuffer + boot status strip.
 *
 * ABL keeps the panel scanning 0xb8000000. rg55g1_splash registers /dev/fb0
 * with shadow→HW flush (console below the status strip). With rg55g1.msm=1,
 * MSM DRM owns the panel after handoff — do not bind simpledrm (it would
 * take /dev/dri/card0 and a second fbdev). Without msm=, simpledrm is still
 * used as the early DRM fallback.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fb.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfb.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>

#include "rg55g1_bringup.h"
#include "rg55g1_splash.h"

void __iomem *rg55g1_splash_hw;
EXPORT_SYMBOL_GPL(rg55g1_splash_hw);

struct fb_info *rg55g1_splash_info;

#define RG55_FB_W		1080
#define RG55_FB_H		1920
#define RG55_LINE_H		32
#define RG55_STATUS_ROWS	8
#define RG55_STATUS_H		(RG55_STATUS_ROWS * RG55_LINE_H)
#define RG55_CON_H		(RG55_FB_H - RG55_STATUS_H)

struct rg55_fb_par {
	u32 palette[16];
	resource_size_t base;
	resource_size_t size;
	void *hw_base;
};

static const struct fb_fix_screeninfo rg55_fb_fix = {
	.id		= "rg55splash",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.accel		= FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo rg55_fb_var = {
	.height		= -1,
	.width		= -1,
	.activate	= FB_ACTIVATE_NOW,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static int rg55_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			  u_int transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	u32 cr = red >> (16 - info->var.red.length);
	u32 cg = green >> (16 - info->var.green.length);
	u32 cb = blue >> (16 - info->var.blue.length);
	u32 value;

	if (regno >= 16)
		return -EINVAL;

	value = (cr << info->var.red.offset) |
		(cg << info->var.green.offset) |
		(cb << info->var.blue.offset);
	if (info->var.transp.length > 0) {
		u32 mask = (1 << info->var.transp.length) - 1;

		mask <<= info->var.transp.offset;
		value |= mask;
	}
	pal[regno] = value;

	return 0;
}

static void rg55_fb_destroy(struct fb_info *info)
{
	struct rg55_fb_par *par = info->par;

	if (par->hw_base) {
		if (rg55g1_splash_hw == par->hw_base)
			rg55g1_splash_hw = NULL;
		rg55g1_splash_info = NULL;
		vfree(info->screen_base);
		memunmap(par->hw_base);
		info->screen_base = NULL;
		par->hw_base = NULL;
	}

	framebuffer_release(info);
}

static void rg55_repaint_status(void);

static void rg55_sync_hw(void *hw, u32 y, u32 h)
{
	u8 *start;
	size_t len;

	if (!hw || !h || y >= RG55_FB_H)
		return;
	if (y + h > RG55_FB_H)
		h = RG55_FB_H - y;
	start = (u8 *)hw + (size_t)y * RG55_FB_W * 4;
	len = (size_t)h * RG55_FB_W * 4;
	wmb();
	dcache_clean_poc((unsigned long)start, (unsigned long)start + len);
}

static void rg55_flush_tile(struct fb_info *info, u32 dx, u32 dy,
			    u32 width, u32 height)
{
	struct rg55_fb_par *par = info->par;
	u8 *src = info->screen_base;
	u8 *dst = par->hw_base;
	u32 line = info->fix.line_length;
	u32 bpp = info->var.bits_per_pixel / 8;
	u32 y;
	u32 hw_y;

	if (!src || !dst || !width || !height)
		return;
	if (dx >= info->var.xres || dy >= info->var.yres)
		return;
	if (dx + width > info->var.xres)
		width = info->var.xres - dx;
	if (dy + height > info->var.yres)
		height = info->var.yres - dy;

	hw_y = dy + RG55_STATUS_H;
	for (y = 0; y < height; y++) {
		size_t soff = (size_t)(dy + y) * line + (size_t)dx * bpp;
		size_t doff = (size_t)(hw_y + y) * line + (size_t)dx * bpp;
		u32 x;

		if (bpp == 4 && (((uintptr_t)(dst + doff) | (uintptr_t)(src + soff)) & 3) == 0) {
			u32 *d = (u32 *)(dst + doff);
			u32 *s = (u32 *)(src + soff);

			for (x = 0; x < width; x++)
				d[x] = s[x];
		} else {
			memcpy(dst + doff, src + soff, (size_t)width * bpp);
		}
	}
	rg55_sync_hw(dst, hw_y, height);
}

static void rg55_flush_rect_tiled(struct fb_info *info, u32 dx, u32 dy,
				  u32 width, u32 height)
{
	u32 y, x;

	for (y = 0; y < height; y += 8) {
		u32 th = height - y;

		if (th > 8)
			th = 8;
		for (x = 0; x < width; x += 64) {
			u32 tw = width - x;

			if (tw > 64)
				tw = 64;
			rg55_flush_tile(info, dx + x, dy + y, tw, th);
		}
	}
}

static void rg55_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	rg55_flush_rect_tiled(info, rect->dx, rect->dy, rect->width, rect->height);
}

static void rg55_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	rg55_flush_rect_tiled(info, area->dx, area->dy, area->width, area->height);
}

static void rg55_imageblit(struct fb_info *info, const struct fb_image *image)
{
	sys_imageblit(info, image);
	rg55_flush_rect_tiled(info, image->dx, image->dy, image->width, image->height);
}

static int rg55_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres != RG55_FB_W)
		var->xres = RG55_FB_W;
	if (var->xres_virtual != RG55_FB_W)
		var->xres_virtual = RG55_FB_W;
	if (var->yres > RG55_CON_H)
		var->yres = RG55_CON_H;
	if (var->yres_virtual > RG55_CON_H)
		var->yres_virtual = RG55_CON_H;
	if (var->bits_per_pixel != 32)
		var->bits_per_pixel = 32;
	var->red.offset = 16;
	var->red.length = 8;
	var->green.offset = 8;
	var->green.length = 8;
	var->blue.offset = 0;
	var->blue.length = 8;
	var->transp.offset = 0;
	var->transp.length = 0;
	return 0;
}

static void rg55_flush_bytes(struct fb_info *info, unsigned long p, size_t len)
{
	u32 line = info->fix.line_length;
	u32 bpp = info->var.bits_per_pixel / 8;
	unsigned long end;
	u32 y0, y1, x0, x1;

	if (!len || !info->screen_base)
		return;

	end = p + len;
	if (end > info->screen_size)
		end = info->screen_size;
	if (end <= p)
		return;

	y0 = (u32)(p / line);
	x0 = (u32)((p % line) / bpp);
	y1 = (u32)((end - 1) / line);
	x1 = (u32)(((end - 1) % line) / bpp) + 1;

	if (x0 == 0 && x1 >= info->var.xres)
		rg55_flush_rect_tiled(info, 0, y0, info->var.xres, y1 - y0 + 1);
	else
		rg55_flush_rect_tiled(info, x0, y0, x1 - x0, y1 - y0 + 1);
}

static int rg55_fb_sync(struct fb_info *info)
{
	rg55_flush_rect_tiled(info, 0, 0, info->var.xres, info->var.yres);
	return 0;
}

static int rg55_fb_pan_display(struct fb_var_screeninfo *var,
			       struct fb_info *info)
{
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;
	rg55_fb_sync(info);
	return 0;
}

static ssize_t rg55_fb_write(struct fb_info *info, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	unsigned long total_size;
	size_t written;
	int err = 0;

	if (!(info->flags & FBINFO_VIRTFB) || !info->screen_base)
		return -ENODEV;

	total_size = info->screen_size;
	if (!total_size)
		total_size = info->fix.smem_len;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size - p) {
		count = total_size - p;
		if (!count)
			return -ENOSPC;
		err = -ENOSPC;
	}

	if (copy_from_user(info->screen_base + p, buf, count))
		return -EFAULT;

	written = count;
	rg55_flush_bytes(info, p, written);

	*ppos += written;
	return written ? (ssize_t)written : err;
}

static const struct fb_ops rg55_splash_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= rg55_fb_write,
	.fb_check_var	= rg55_check_var,
	.fb_pan_display	= rg55_fb_pan_display,
	.fb_fillrect	= rg55_fillrect,
	.fb_copyarea	= rg55_copyarea,
	.fb_imageblit	= rg55_imageblit,
	.fb_sync	= rg55_fb_sync,
	.fb_destroy	= rg55_fb_destroy,
	.fb_setcolreg	= rg55_setcolreg,
};

static unsigned int rg55_status_slot;
static char rg55_status_msg[RG55_STATUS_ROWS][24];
static u32 rg55_status_color[RG55_STATUS_ROWS];
static char rg55_vbus_sticky[24] = "VBUS:?";
static u32 rg55_vbus_color = 0x00808080;
static char rg55_batt_sticky[24] = "BAT:?";
static u32 rg55_batt_color = 0x00808080;
static bool rg55g1_drm_done;

static void rg55_hw_bar(void *hw, u32 y, u32 h, u32 color)
{
	u32 x, row;
	u32 line = RG55_FB_W * 4;

	if (!hw || !h || y >= RG55_FB_H)
		return;
	if (y + h > RG55_FB_H)
		h = RG55_FB_H - y;
	for (row = 0; row < h; row++) {
		u32 *p = (u32 *)((u8 *)hw + (size_t)(y + row) * line);

		for (x = 0; x < RG55_FB_W; x++)
			p[x] = color;
	}
}

static void rg55_hw_putchar2x(void *hw, u32 x0, u32 y0, char c, u32 fg, u32 bg)
{
	const u8 *glyph;
	u32 line = RG55_FB_W * 4;
	int row, col;

	if (c < 32 || c > 126)
		c = '?';
	glyph = (const u8 *)font_vga_8x16.data + ((unsigned char)c) * 16;

	for (row = 0; row < 16; row++) {
		u8 bits = glyph[row];
		u32 *p0 = (u32 *)((u8 *)hw + (size_t)(y0 + row * 2) * line +
				  (size_t)x0 * 4);
		u32 *p1 = (u32 *)((u8 *)hw + (size_t)(y0 + row * 2 + 1) * line +
				  (size_t)x0 * 4);

		for (col = 0; col < 8; col++) {
			u32 pix = (bits & (0x80 >> col)) ? fg : bg;

			p0[col * 2] = pix;
			p0[col * 2 + 1] = pix;
			p1[col * 2] = pix;
			p1[col * 2 + 1] = pix;
		}
	}
}

static void rg55_hw_puts2x(void *hw, u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
	while (s && *s && x + 16 <= RG55_FB_W) {
		rg55_hw_putchar2x(hw, x, y, *s++, fg, bg);
		x += 16;
	}
}

static u32 rg55_status_fg(u32 bg)
{
	u32 r = (bg >> 16) & 0xff;
	u32 g = (bg >> 8) & 0xff;
	u32 b = bg & 0xff;

	if (r * 299 + g * 587 + b * 114 > 128000)
		return 0x00000000;
	return 0x00ffffff;
}

static void rg55_repaint_status(void)
{
	void *hw = rg55g1_splash_hw;
	unsigned int i;
	char buf[28];
	u32 fg;

	if (!hw)
		return;

	rg55_hw_bar(hw, 0, RG55_LINE_H, rg55_vbus_color);
	snprintf(buf, sizeof(buf), "0 %s", rg55_vbus_sticky);
	fg = rg55_status_fg(rg55_vbus_color);
	rg55_hw_puts2x(hw, 16, 0, buf, fg, rg55_vbus_color);

	rg55_hw_bar(hw, RG55_LINE_H, RG55_LINE_H, rg55_batt_color);
	snprintf(buf, sizeof(buf), "1 %s", rg55_batt_sticky);
	fg = rg55_status_fg(rg55_batt_color);
	rg55_hw_puts2x(hw, 16, RG55_LINE_H, buf, fg, rg55_batt_color);

	for (i = 0; i < rg55_status_slot && i < RG55_STATUS_ROWS - 2; i++) {
		u32 y = (i + 2) * RG55_LINE_H;

		rg55_hw_bar(hw, y, RG55_LINE_H, rg55_status_color[i]);
		snprintf(buf, sizeof(buf), "%u %s", i + 2, rg55_status_msg[i]);
		fg = rg55_status_fg(rg55_status_color[i]);
		rg55_hw_puts2x(hw, 16, y, buf, fg, rg55_status_color[i]);
	}
	rg55_sync_hw(hw, 0, RG55_STATUS_H);
}

void rg55g1_status(const char *msg, u32 color)
{
	if (!rg55g1_splash_hw)
		return;
	if (!msg)
		msg = "?";

	if (rg55_status_slot >= RG55_STATUS_ROWS - 2) {
		memmove(rg55_status_msg[0], rg55_status_msg[1],
			(RG55_STATUS_ROWS - 3) * sizeof(rg55_status_msg[0]));
		memmove(rg55_status_color, rg55_status_color + 1,
			(RG55_STATUS_ROWS - 3) * sizeof(rg55_status_color[0]));
		rg55_status_slot = RG55_STATUS_ROWS - 3;
	}
	strscpy(rg55_status_msg[rg55_status_slot], msg,
		sizeof(rg55_status_msg[0]));
	rg55_status_color[rg55_status_slot] = color;
	rg55_status_slot++;
	rg55_repaint_status();
}
EXPORT_SYMBOL_GPL(rg55g1_status);

void rg55g1_status_vbus(const char *msg, u32 color)
{
	if (!msg)
		msg = "VBUS:?";
	strscpy(rg55_vbus_sticky, msg, sizeof(rg55_vbus_sticky));
	rg55_vbus_color = color;
	if (rg55g1_splash_hw)
		rg55_repaint_status();
}
EXPORT_SYMBOL_GPL(rg55g1_status_vbus);

void rg55g1_status_batt(const char *msg, u32 color)
{
	if (!msg)
		msg = "BAT:?";
	strscpy(rg55_batt_sticky, msg, sizeof(rg55_batt_sticky));
	rg55_batt_color = color;
	if (rg55g1_splash_hw)
		rg55_repaint_status();
}
EXPORT_SYMBOL_GPL(rg55g1_status_batt);

void rg55g1_mark(u32 y, u32 color)
{
	(void)y;
	rg55g1_status("MARK", color);
}
EXPORT_SYMBOL_GPL(rg55g1_mark);

static struct fb_info *rg55g1_pending_fb;

static int __init rg55g1_splash_console_init(void)
{
	struct fb_info *info;
	struct rg55_fb_par *par;
	void *shadow;
	void *hw;
	size_t len = 1080UL * 1920UL * 4;
	phys_addr_t phys = 0xb8000000UL;
	unsigned int stride = 1080;
	int i;

	hw = memremap(phys, len, MEMREMAP_WB);
	if (!hw)
		hw = memremap(phys, len, MEMREMAP_WC);
	if (!hw)
		return 0;

	shadow = vzalloc(len);
	if (!shadow) {
		memunmap(hw);
		return 0;
	}
	memset(shadow, 0, len);

	info = framebuffer_alloc(sizeof(*par), NULL);
	if (!info) {
		vfree(shadow);
		memunmap(hw);
		return 0;
	}

	par = info->par;
	info->fix = rg55_fb_fix;
	info->fix.smem_start = phys;
	info->fix.smem_len = (u32)RG55_CON_H * stride * 4;
	info->screen_size = info->fix.smem_len;
	info->fix.line_length = stride * 4;
	info->var = rg55_fb_var;
	info->var.xres = RG55_FB_W;
	info->var.yres = RG55_CON_H;
	info->var.xres_virtual = RG55_FB_W;
	info->var.yres_virtual = RG55_CON_H;
	info->var.bits_per_pixel = 32;
	info->var.red.offset = 16;
	info->var.red.length = 8;
	info->var.green.offset = 8;
	info->var.green.length = 8;
	info->var.blue.offset = 0;
	info->var.blue.length = 8;
	info->fbops = &rg55_splash_ops;
	info->flags = FBINFO_VIRTFB | FBINFO_HWACCEL_DISABLED;
	info->screen_base = shadow;
	info->pseudo_palette = par->palette;
	for (i = 0; i < 16; i++) {
		u32 intensity = (i & 8) ? 0xff : 0xaa;
		u32 r = (i & 4) ? intensity : 0;
		u32 g = (i & 2) ? intensity : 0;
		u32 b = (i & 1) ? intensity : 0;

		if (i == 0)
			r = g = b = 0;
		else if (i == 8)
			r = g = b = 0x55;
		else if (i == 7 || i == 15)
			r = g = b = 0xff;
		par->palette[i] = (r << 16) | (g << 8) | b;
	}
	par->base = phys;
	par->size = len;
	par->hw_base = hw;
	rg55g1_splash_hw = hw;
	rg55g1_splash_info = info;

	{
		u32 y, x;

		rg55_hw_bar(hw, 0, RG55_STATUS_H, 0x00202020);
		rg55_sync_hw(hw, 0, RG55_STATUS_H);
		for (y = 0; y < RG55_CON_H; y += 8) {
			for (x = 0; x < RG55_FB_W; x += 64)
				rg55_flush_tile(info, x, y, 64, 8);
		}
	}

	rg55_status_slot = 0;
	rg55g1_status("FB-MAP", 0x00404040);
	rg55g1_status("READY", 0x00ffff00);
	rg55g1_status_vbus("VBUS:wait", 0x00808080);
	rg55g1_status_batt("BAT:wait", 0x00808080);
	rg55g1_pending_fb = info;
	return 0;
}
early_initcall(rg55g1_splash_console_init);

#if IS_ENABLED(CONFIG_DRM_SIMPLEDRM)
static void rg55g1_disable_simple_framebuffer_dt(void)
{
	struct device_node *np;

	sysfb_disable(NULL);

	np = of_get_compatible_child(of_chosen, "simple-framebuffer");
	if (!np)
		np = of_find_node_by_path("/chosen/framebuffer@b8000000");
	if (!np)
		return;

	rg55g1_force_node_disabled(np);
	pr_emerg("rg55g1: simple-framebuffer DT disabled (MSM path)\n");
	of_node_put(np);
}

static int rg55g1_populate_simpledrm(void)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_get_compatible_child(of_chosen, "simple-framebuffer");
	if (!np)
		np = of_find_node_by_path("/chosen/framebuffer@b8000000");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		rg55g1_status("DRM-NO-DT", 0x00ff0000);
		pr_emerg("rg55g1: no simple-framebuffer DT node for DRM\n");
		return -ENODEV;
	}

	sysfb_disable(NULL);
	pdev = of_platform_device_create(np, NULL, NULL);
	of_node_put(np);
	if (!pdev) {
		rg55g1_status("DRM-FAIL", 0x00ff0000);
		pr_emerg("rg55g1: of_platform_device_create(simple-framebuffer) failed\n");
		return -ENOMEM;
	}

	if (!pdev->dev.driver) {
		rg55g1_status("DRM-NOBIND", 0x00ff8000);
		pr_emerg("rg55g1: simpledrm not bound on %s\n", dev_name(&pdev->dev));
		return -EPROBE_DEFER;
	}

	rg55g1_status("DRM-OK", 0x0000ff00);
	pr_emerg("rg55g1: simpledrm bound %s driver=%s\n",
		 dev_name(&pdev->dev), pdev->dev.driver->name);
	return 0;
}
#else
static void rg55g1_disable_simple_framebuffer_dt(void)
{
}

static int rg55g1_populate_simpledrm(void)
{
	return 0;
}
#endif

static int __init rg55g1_splash_fbdev_register(void)
{
	struct fb_info *info = rg55g1_pending_fb;
	int ret;

	if (!info)
		return 0;

	rg55g1_pending_fb = NULL;

	if (register_framebuffer(info) < 0) {
		struct rg55_fb_par *par = info->par;

		rg55g1_splash_hw = NULL;
		rg55g1_splash_info = NULL;
		vfree(info->screen_base);
		if (par->hw_base)
			memunmap(par->hw_base);
		framebuffer_release(info);
		return -EIO;
	}

	rg55g1_status("FB-REG", 0x0000ff00);
	pr_emerg("rg55g1: splash fb %dx%d console under %dpx top status\n",
		 RG55_FB_W, RG55_CON_H, RG55_STATUS_H);

#if IS_ENABLED(CONFIG_DRM_MSM_MDSS)
	if (rg55g1_msm_early) {
		ret = rg55g1_populate_msm_display();
		if (!ret) {
			rg55g1_disable_simple_framebuffer_dt();
			rg55g1_sanitize_dt();
			rg55g1_status("DT-OK", 0x00ff00ff);
			pr_emerg("rg55g1: /dev/fb0 + MSM KMS ready\n");
			rg55g1_drm_done = true;
			return 0;
		}
		pr_emerg("rg55g1: MSM display failed (%d), trying simpledrm\n", ret);
	} else if (rg55g1_msm_auto) {
		/*
		 * MSM comes up via OF populate (mainline/lv6) or USB bringup.
		 * Skip simpledrm so it never claims card0/fb — splash fb0 stays
		 * until MSM post_init quiesces INTF and modesets.
		 */
		rg55g1_disable_simple_framebuffer_dt();
		rg55g1_sanitize_dt();
		rg55g1_status("DT-OK", 0x00ff00ff);
		pr_emerg("rg55g1: MSM deferred (rg55g1.msm=1) — simpledrm skipped\n");
		rg55g1_drm_done = true;
		return 0;
	} else {
		pr_emerg("rg55g1: MSM deferred — append rg55g1.msm=1 (~4s, pre-init)\n");
	}
#endif

	ret = rg55g1_populate_simpledrm();
	if (ret)
		return ret;

	/* simpledrm modeset may memset_io() scanout; push fbcon shadow back below status. */
	if (rg55g1_splash_info && rg55g1_splash_info->fbops->fb_sync)
		rg55g1_splash_info->fbops->fb_sync(rg55g1_splash_info);

	rg55g1_sanitize_dt();

	rg55g1_status("DT-OK", 0x00ff00ff);
	pr_emerg("rg55g1: /dev/fb0 + /dev/dri/card0 ready\n");
	rg55g1_drm_done = true;
	return 0;
}

static int __init rg55g1_splash_fbdev_init(void)
{
	return rg55g1_splash_fbdev_register();
}
fs_initcall(rg55g1_splash_fbdev_init);

int __init rg55g1_fbmem_ensure(void);

int __init rg55g1_splash_fbdev_bringup(void)
{
	int ret;

	if (!rg55g1_pending_fb && rg55g1_drm_done)
		return 0;
	if (!rg55g1_pending_fb && !rg55g1_splash_hw)
		return 0;

	ret = rg55g1_fbmem_ensure();
	if (ret)
		return ret;

	return rg55g1_splash_fbdev_register();
}

MODULE_DESCRIPTION("Anbernic RG55G1 splash framebuffer and status strip");
MODULE_LICENSE("GPL");
