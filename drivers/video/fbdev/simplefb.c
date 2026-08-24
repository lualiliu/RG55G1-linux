// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simplest possible simple frame-buffer driver, as a platform device
 *
 * Copyright (c) 2013, Stephen Warren
 *
 * Based on q40fb.c, which was:
 * Copyright (C) 2001 Richard Zidlicky <rz@linux-m68k.org>
 *
 * Also based on offb.c, which was:
 * Copyright (C) 1997 Geert Uytterhoeven
 * Copyright (C) 1996 Paul Mackerras
 */

#include <linux/aperture.h>
#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/font.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/parser.h>
#include <linux/platform_data/simplefb.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>

/* Splash HW mapping used by flush path. */
void __iomem *rg55g1_splash_hw;
EXPORT_SYMBOL_GPL(rg55g1_splash_hw);

struct fb_info *rg55g1_splash_info;

static const struct fb_fix_screeninfo simplefb_fix = {
	.id		= "simple",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.accel		= FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo simplefb_var = {
	.height		= -1,
	.width		= -1,
	.activate	= FB_ACTIVATE_NOW,
	.vmode		= FB_VMODE_NONINTERLACED,
};

#define PSEUDO_PALETTE_SIZE 16

static int simplefb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			      u_int transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	u32 cr = red >> (16 - info->var.red.length);
	u32 cg = green >> (16 - info->var.green.length);
	u32 cb = blue >> (16 - info->var.blue.length);
	u32 value;

	if (regno >= PSEUDO_PALETTE_SIZE)
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

struct simplefb_par {
	u32 palette[PSEUDO_PALETTE_SIZE];
	resource_size_t base;
	resource_size_t size;
	struct resource *mem;
	/* RG55G1 splash: draw in shadow (DRAM), flush dirty rects to HW */
	void *hw_base;
#if defined CONFIG_OF && defined CONFIG_COMMON_CLK
	bool clks_enabled;
	unsigned int clk_count;
	struct clk **clks;
#endif
#if defined CONFIG_OF && defined CONFIG_PM_GENERIC_DOMAINS
	unsigned int num_genpds;
	struct device **genpds;
	struct device_link **genpd_links;
#endif
#if defined CONFIG_OF && defined CONFIG_REGULATOR
	bool regulators_enabled;
	u32 regulator_count;
	struct regulator **regulators;
#endif
};

static void simplefb_clocks_destroy(struct simplefb_par *par);
static void simplefb_regulators_destroy(struct simplefb_par *par);
static void simplefb_detach_genpds(void *res);

/*
 * fb_ops.fb_destroy is called by the last put_fb_info() call at the end
 * of unregister_framebuffer() or fb_release(). Do any cleanup here.
 */
static void simplefb_destroy(struct fb_info *info)
{
	struct simplefb_par *par = info->par;
	struct resource *mem = par->mem;

	simplefb_regulators_destroy(info->par);
	simplefb_clocks_destroy(info->par);
	simplefb_detach_genpds(info->par);
	if (par->hw_base) {
		if (rg55g1_splash_hw == par->hw_base)
			rg55g1_splash_hw = NULL;
		vfree(info->screen_base);
		memunmap(par->hw_base);
		info->screen_base = NULL;
		par->hw_base = NULL;
	} else if (info->screen_base) {
		memunmap(info->screen_base);
	}

	framebuffer_release(info);

	if (mem)
		release_mem_region(mem->start, resource_size(mem));
}

static const struct fb_ops simplefb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_destroy	= simplefb_destroy,
	.fb_setcolreg	= simplefb_setcolreg,
};

/*
 * Draw in DRAM shadow, then flush dirty rects to the 1080x1920 splash FB.
 * Console shadow is RG55_CON_H tall; on the panel it is placed BELOW the
 * top status strip (HW y = dy + RG55_STATUS_H) so status stays readable.
 */
#define RG55_FB_W		1080
#define RG55_FB_H		1920
#define RG55_LINE_H		32	/* status row height (2x VGA8x16) */
#define RG55_STATUS_ROWS	8
#define RG55_STATUS_H		(RG55_STATUS_ROWS * RG55_LINE_H) /* 256 */
#define RG55_CON_H		(RG55_FB_H - RG55_STATUS_H)

static void rg55_repaint_status(void);

/* Display scans DRAM; WB stores need a clean so pixels are not speckled. */
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
	struct simplefb_par *par = info->par;
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
		/* Shadow at dy → panel below top status strip. */
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

/* Push shadow bytes [p, p+len) to panel (console sits below status strip). */
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
	.fb_destroy	= simplefb_destroy,
	.fb_setcolreg	= simplefb_setcolreg,
};

static unsigned int rg55_status_slot;
static char rg55_status_msg[RG55_STATUS_ROWS][24];
static u32 rg55_status_color[RG55_STATUS_ROWS];
/* Always painted on row 0 so USB/XHCI spam cannot scroll VBUS away. */
static char rg55_vbus_sticky[24] = "VBUS:?";
static u32 rg55_vbus_color = 0x00808080;

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
	/* defer clean to caller when painting many rows */
}

/* 2× VGA8x16 → 16×32 glyphs for status strip readability. */
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

	/* White/light backgrounds need dark glyphs (EXEC-INIT was unreadable). */
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

	/* Sticky VBUS on row 0 */
	rg55_hw_bar(hw, 0, RG55_LINE_H, rg55_vbus_color);
	snprintf(buf, sizeof(buf), "0 %s", rg55_vbus_sticky);
	fg = rg55_status_fg(rg55_vbus_color);
	rg55_hw_puts2x(hw, 16, 0, buf, fg, rg55_vbus_color);

	for (i = 0; i < rg55_status_slot && i < RG55_STATUS_ROWS - 1; i++) {
		u32 y = (i + 1) * RG55_LINE_H;

		rg55_hw_bar(hw, y, RG55_LINE_H, rg55_status_color[i]);
		snprintf(buf, sizeof(buf), "%u %s", i + 1, rg55_status_msg[i]);
		fg = rg55_status_fg(rg55_status_color[i]);
		rg55_hw_puts2x(hw, 16, y, buf, fg, rg55_status_color[i]);
	}
	rg55_sync_hw(hw, 0, RG55_STATUS_H);
}

/*
 * Top-of-screen status (NOT kernel log). Large 2x font. Re-painted fully
 * each time so console flush cannot leave permanent black speckles on it.
 */
void rg55g1_status(const char *msg, u32 color)
{
	if (!rg55g1_splash_hw)
		return;
	if (!msg)
		msg = "?";

	/* Scrollable region is rows 1..N-1 (row 0 = VBUS sticky). */
	if (rg55_status_slot >= RG55_STATUS_ROWS - 1) {
		memmove(rg55_status_msg[0], rg55_status_msg[1],
			(RG55_STATUS_ROWS - 2) * sizeof(rg55_status_msg[0]));
		memmove(rg55_status_color, rg55_status_color + 1,
			(RG55_STATUS_ROWS - 2) * sizeof(rg55_status_color[0]));
		rg55_status_slot = RG55_STATUS_ROWS - 2;
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

void rg55g1_mark(u32 y, u32 color)
{
	(void)y;
	rg55g1_status("MARK", color);
}
EXPORT_SYMBOL_GPL(rg55g1_mark);

static struct simplefb_format simplefb_formats[] = SIMPLEFB_FORMATS;

struct simplefb_params {
	u32 width;
	u32 height;
	u32 stride;
	struct simplefb_format *format;
	struct resource memory;
};

static int simplefb_parse_dt(struct platform_device *pdev,
			   struct simplefb_params *params)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;
	const char *format;
	int i;

	ret = of_property_read_u32(np, "width", &params->width);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse width property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "height", &params->height);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse height property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "stride", &params->stride);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse stride property\n");
		return ret;
	}

	ret = of_property_read_string(np, "format", &format);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse format property\n");
		return ret;
	}
	params->format = NULL;
	for (i = 0; i < ARRAY_SIZE(simplefb_formats); i++) {
		if (strcmp(format, simplefb_formats[i].name))
			continue;
		params->format = &simplefb_formats[i];
		break;
	}
	if (!params->format) {
		dev_err(&pdev->dev, "Invalid format value\n");
		return -EINVAL;
	}

	ret = of_reserved_mem_region_to_resource(np, 0, &params->memory);
	if (!ret) {
		if (of_property_present(np, "reg"))
			dev_warn(&pdev->dev, "preferring \"memory-region\" over \"reg\" property\n");
	} else {
		memset(&params->memory, 0, sizeof(params->memory));
	}

	return 0;
}

static int simplefb_parse_pd(struct platform_device *pdev,
			     struct simplefb_params *params)
{
	struct simplefb_platform_data *pd = dev_get_platdata(&pdev->dev);
	int i;

	params->width = pd->width;
	params->height = pd->height;
	params->stride = pd->stride;

	params->format = NULL;
	for (i = 0; i < ARRAY_SIZE(simplefb_formats); i++) {
		if (strcmp(pd->format, simplefb_formats[i].name))
			continue;

		params->format = &simplefb_formats[i];
		break;
	}

	if (!params->format) {
		dev_err(&pdev->dev, "Invalid format value\n");
		return -EINVAL;
	}

	memset(&params->memory, 0, sizeof(params->memory));

	return 0;
}

#if defined CONFIG_OF && defined CONFIG_COMMON_CLK
/*
 * Clock handling code.
 *
 * Here we handle the clocks property of our "simple-framebuffer" dt node.
 * This is necessary so that we can make sure that any clocks needed by
 * the display engine that the bootloader set up for us (and for which it
 * provided a simplefb dt node), stay up, for the life of the simplefb
 * driver.
 *
 * When the driver unloads, we cleanly disable, and then release the clocks.
 *
 * We only complain about errors here, no action is taken as the most likely
 * error can only happen due to a mismatch between the bootloader which set
 * up simplefb, and the clock definitions in the device tree. Chances are
 * that there are no adverse effects, and if there are, a clean teardown of
 * the fb probe will not help us much either. So just complain and carry on,
 * and hope that the user actually gets a working fb at the end of things.
 */
static int simplefb_clocks_get(struct simplefb_par *par,
			       struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct clk *clock;
	int i;

	if (dev_get_platdata(&pdev->dev) || !np)
		return 0;

	par->clk_count = of_clk_get_parent_count(np);
	if (!par->clk_count)
		return 0;

	par->clks = kzalloc_objs(struct clk *, par->clk_count);
	if (!par->clks)
		return -ENOMEM;

	for (i = 0; i < par->clk_count; i++) {
		clock = of_clk_get(np, i);
		if (IS_ERR(clock)) {
			if (PTR_ERR(clock) == -EPROBE_DEFER) {
				while (--i >= 0) {
					clk_put(par->clks[i]);
				}
				kfree(par->clks);
				return -EPROBE_DEFER;
			}
			dev_err(&pdev->dev, "%s: clock %d not found: %ld\n",
				__func__, i, PTR_ERR(clock));
			continue;
		}
		par->clks[i] = clock;
	}

	return 0;
}

static void simplefb_clocks_enable(struct simplefb_par *par,
				   struct platform_device *pdev)
{
	int i, ret;

	for (i = 0; i < par->clk_count; i++) {
		if (par->clks[i]) {
			ret = clk_prepare_enable(par->clks[i]);
			if (ret) {
				dev_err(&pdev->dev,
					"%s: failed to enable clock %d: %d\n",
					__func__, i, ret);
				clk_put(par->clks[i]);
				par->clks[i] = NULL;
			}
		}
	}
	par->clks_enabled = true;
}

static void simplefb_clocks_destroy(struct simplefb_par *par)
{
	int i;

	if (!par->clks)
		return;

	for (i = 0; i < par->clk_count; i++) {
		if (par->clks[i]) {
			if (par->clks_enabled)
				clk_disable_unprepare(par->clks[i]);
			clk_put(par->clks[i]);
		}
	}

	kfree(par->clks);
}
#else
static int simplefb_clocks_get(struct simplefb_par *par,
	struct platform_device *pdev) { return 0; }
static void simplefb_clocks_enable(struct simplefb_par *par,
	struct platform_device *pdev) { }
static void simplefb_clocks_destroy(struct simplefb_par *par) { }
#endif

#if defined CONFIG_OF && defined CONFIG_REGULATOR

#define SUPPLY_SUFFIX "-supply"

/*
 * Regulator handling code.
 *
 * Here we handle the num-supplies and vin*-supply properties of our
 * "simple-framebuffer" dt node. This is necessary so that we can make sure
 * that any regulators needed by the display hardware that the bootloader
 * set up for us (and for which it provided a simplefb dt node), stay up,
 * for the life of the simplefb driver.
 *
 * When the driver unloads, we cleanly disable, and then release the
 * regulators.
 *
 * We only complain about errors here, no action is taken as the most likely
 * error can only happen due to a mismatch between the bootloader which set
 * up simplefb, and the regulator definitions in the device tree. Chances are
 * that there are no adverse effects, and if there are, a clean teardown of
 * the fb probe will not help us much either. So just complain and carry on,
 * and hope that the user actually gets a working fb at the end of things.
 */
static int simplefb_regulators_get(struct simplefb_par *par,
				   struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct property *prop;
	struct regulator *regulator;
	const char *p;
	int count = 0, i = 0;

	if (dev_get_platdata(&pdev->dev) || !np)
		return 0;

	/* Count the number of regulator supplies */
	for_each_property_of_node(np, prop) {
		p = strstr(prop->name, SUPPLY_SUFFIX);
		if (p && p != prop->name)
			count++;
	}

	if (!count)
		return 0;

	par->regulators = devm_kcalloc(&pdev->dev, count,
				       sizeof(struct regulator *), GFP_KERNEL);
	if (!par->regulators)
		return -ENOMEM;

	/* Get all the regulators */
	for_each_property_of_node(np, prop) {
		char name[32]; /* 32 is max size of property name */

		p = strstr(prop->name, SUPPLY_SUFFIX);
		if (!p || p == prop->name)
			continue;

		strscpy(name, prop->name,
			strlen(prop->name) - strlen(SUPPLY_SUFFIX) + 1);
		regulator = devm_regulator_get_optional(&pdev->dev, name);
		if (IS_ERR(regulator)) {
			if (PTR_ERR(regulator) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			dev_err(&pdev->dev, "regulator %s not found: %ld\n",
				name, PTR_ERR(regulator));
			continue;
		}
		par->regulators[i++] = regulator;
	}
	par->regulator_count = i;

	return 0;
}

static void simplefb_regulators_enable(struct simplefb_par *par,
				       struct platform_device *pdev)
{
	int i, ret;

	/* Enable all the regulators */
	for (i = 0; i < par->regulator_count; i++) {
		ret = regulator_enable(par->regulators[i]);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to enable regulator %d: %d\n",
				i, ret);
			devm_regulator_put(par->regulators[i]);
			par->regulators[i] = NULL;
		}
	}
	par->regulators_enabled = true;
}

static void simplefb_regulators_destroy(struct simplefb_par *par)
{
	int i;

	if (!par->regulators || !par->regulators_enabled)
		return;

	for (i = 0; i < par->regulator_count; i++)
		if (par->regulators[i])
			regulator_disable(par->regulators[i]);
}
#else
static int simplefb_regulators_get(struct simplefb_par *par,
	struct platform_device *pdev) { return 0; }
static void simplefb_regulators_enable(struct simplefb_par *par,
	struct platform_device *pdev) { }
static void simplefb_regulators_destroy(struct simplefb_par *par) { }
#endif

#if defined CONFIG_OF && defined CONFIG_PM_GENERIC_DOMAINS
static void simplefb_detach_genpds(void *res)
{
	struct simplefb_par *par = res;
	unsigned int i = par->num_genpds;

	if (par->num_genpds <= 1)
		return;

	while (i--) {
		if (par->genpd_links[i])
			device_link_del(par->genpd_links[i]);

		if (!IS_ERR_OR_NULL(par->genpds[i]))
			dev_pm_domain_detach(par->genpds[i], true);
	}
	par->num_genpds = 0;
}

static int simplefb_attach_genpds(struct simplefb_par *par,
				  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int i, num_genpds;
	int err;

	err = of_count_phandle_with_args(dev->of_node, "power-domains",
					 "#power-domain-cells");
	if (err < 0) {
		/* Nothing wrong if optional PDs are missing */
		if (err == -ENOENT)
			return 0;

		dev_err(dev, "failed to parse power-domains: %d\n", err);
		return err;
	}

	num_genpds = err;

	/*
	 * Single power-domain devices are handled by the driver core, so
	 * nothing to do here.
	 */
	if (num_genpds <= 1) {
		par->num_genpds = num_genpds;
		return 0;
	}

	par->genpds = devm_kcalloc(dev, num_genpds, sizeof(*par->genpds),
				   GFP_KERNEL);
	if (!par->genpds)
		return -ENOMEM;

	par->genpd_links = devm_kcalloc(dev, num_genpds,
					sizeof(*par->genpd_links),
					GFP_KERNEL);
	if (!par->genpd_links)
		return -ENOMEM;

	/*
	 * Set par->num_genpds only after genpds and genpd_links are allocated
	 * to exit early from simplefb_detach_genpds() without full
	 * initialisation.
	 */
	par->num_genpds = num_genpds;

	for (i = 0; i < par->num_genpds; i++) {
		par->genpds[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR(par->genpds[i])) {
			err = PTR_ERR(par->genpds[i]);
			if (err == -EPROBE_DEFER) {
				simplefb_detach_genpds(par);
				return err;
			}

			dev_warn(dev, "failed to attach domain %u: %d\n", i, err);
			continue;
		}

		par->genpd_links[i] = device_link_add(dev, par->genpds[i],
						      DL_FLAG_STATELESS |
						      DL_FLAG_PM_RUNTIME |
						      DL_FLAG_RPM_ACTIVE);
		if (!par->genpd_links[i])
			dev_warn(dev, "failed to link power-domain %u\n", i);
	}

	return 0;
}
#else
static void simplefb_detach_genpds(void *res) { }
static int simplefb_attach_genpds(struct simplefb_par *par,
				  struct platform_device *pdev)
{
	return 0;
}
#endif

static int simplefb_probe(struct platform_device *pdev)
{
	int ret;
	struct simplefb_params params;
	struct fb_info *info;
	struct simplefb_par *par;
	struct resource *res, *mem;

	if (fb_get_options("simplefb", NULL))
		return -ENODEV;

	ret = -ENODEV;
	if (dev_get_platdata(&pdev->dev))
		ret = simplefb_parse_pd(pdev, &params);
	else if (pdev->dev.of_node)
		ret = simplefb_parse_dt(pdev, &params);

	if (ret)
		return ret;

	if (params.memory.start == 0 && params.memory.end == 0) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res) {
			dev_err(&pdev->dev, "No memory resource\n");
			return -EINVAL;
		}
	} else {
		res = &params.memory;
	}

	/*
	 * RG55G1: early_initcall already owns continuous-splash scanout.
	 * A second map/register of the same buffer causes a noisy/corrupt
	 * display (two writers + leftover bars).
	 */
	if (res->start == 0xb8000000ULL) {
		dev_info(&pdev->dev,
			 "skip DT simplefb @0xb8000000 (splash console owns it)\n");
		return -ENODEV;
	}

	mem = request_mem_region(res->start, resource_size(res), "simplefb");
	if (!mem) {
		/*
		 * We cannot make this fatal. Sometimes this comes from magic
		 * spaces our resource handlers simply don't know about. Use
		 * the I/O-memory resource as-is and try to map that instead.
		 */
		dev_warn(&pdev->dev, "simplefb: cannot reserve video memory at %pR\n", res);
		mem = res;
	}

	info = framebuffer_alloc(sizeof(struct simplefb_par), &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto error_release_mem_region;
	}
	platform_set_drvdata(pdev, info);

	par = info->par;

	info->fix = simplefb_fix;
	info->fix.smem_start = mem->start;
	info->fix.smem_len = resource_size(mem);
	info->fix.line_length = params.stride;

	info->var = simplefb_var;
	info->var.xres = params.width;
	info->var.yres = params.height;
	info->var.xres_virtual = params.width;
	info->var.yres_virtual = params.height;
	info->var.bits_per_pixel = params.format->bits_per_pixel;
	info->var.red = params.format->red;
	info->var.green = params.format->green;
	info->var.blue = params.format->blue;
	info->var.transp = params.format->transp;

	par->base = info->fix.smem_start;
	par->size = info->fix.smem_len;

	info->fbops = &simplefb_ops;
	/*
	 * Continuous-splash is in a reserved no-map region; ioremap_wc()
	 * can hang on this platform — use memremap(WC) instead.
	 */
	info->screen_base = memremap(info->fix.smem_start,
				      info->fix.smem_len, MEMREMAP_WC);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto error_fb_release;
	}
	info->pseudo_palette = par->palette;

	ret = simplefb_clocks_get(par, pdev);
	if (ret < 0)
		goto error_unmap;

	ret = simplefb_regulators_get(par, pdev);
	if (ret < 0)
		goto error_clocks;

	ret = simplefb_attach_genpds(par, pdev);
	if (ret < 0)
		goto error_regulators;

	simplefb_clocks_enable(par, pdev);
	simplefb_regulators_enable(par, pdev);

	dev_info(&pdev->dev, "framebuffer at 0x%lx, 0x%x bytes\n",
			     info->fix.smem_start, info->fix.smem_len);
	dev_info(&pdev->dev, "format=%s, mode=%dx%dx%d, linelength=%d\n",
			     params.format->name,
			     info->var.xres, info->var.yres,
			     info->var.bits_per_pixel, info->fix.line_length);

	if (mem != res)
		par->mem = mem; /* release in clean-up handler */

	ret = devm_aperture_acquire_for_platform_device(pdev, par->base, par->size);
	if (ret) {
		/*
		 * Bring-up: EFI/sysfb may already own the region; still
		 * register so fbcon can draw on continuous splash.
		 */
		dev_warn(&pdev->dev,
			 "aperture acquire failed (%d), registering anyway\n",
			 ret);
	}
	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register simplefb: %d\n", ret);
		goto error_genpds;
	}

	dev_info(&pdev->dev, "fb%d: simplefb registered!\n", info->node);

	return 0;

error_genpds:
	simplefb_detach_genpds(par);
error_regulators:
	simplefb_regulators_destroy(par);
error_clocks:
	simplefb_clocks_destroy(par);
error_unmap:
	memunmap(info->screen_base);
error_fb_release:
	framebuffer_release(info);
error_release_mem_region:
	if (mem != res)
		release_mem_region(mem->start, resource_size(mem));
	return ret;
}

static void simplefb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	/* simplefb_destroy takes care of info cleanup */
	unregister_framebuffer(info);
}

static const struct of_device_id simplefb_of_match[] = {
	{ .compatible = "simple-framebuffer", },
	{ },
};
MODULE_DEVICE_TABLE(of, simplefb_of_match);

static struct platform_driver simplefb_driver = {
	.driver = {
		.name = "simple-framebuffer",
		.of_match_table = simplefb_of_match,
	},
	.probe = simplefb_probe,
	.remove = simplefb_remove,
};

module_platform_driver(simplefb_driver);

/*
 * RG55G1: map splash FB early for status strip; register /dev/fb0 later.
 * register_framebuffer() must run after fbmem_init (subsys_initcall), otherwise
 * fb_class is NULL and no fb0 device node appears under devtmpfs.
 * Must NOT register from console_initcall — console_lock deadlock with fbcon.
 */
static struct fb_info *rg55g1_pending_fb;

static int __init rg55g1_splash_console_init(void)
{
	struct fb_info *info;
	struct simplefb_par *par;
	void *shadow;
	void *hw;
	size_t len = 1080UL * 1920UL * 4;
	phys_addr_t phys = 0xb8000000UL;
	unsigned int stride = 1080;
	int i;

	/* Prefer WB: WC full-frame ops hang and leave black-on-black text. */
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
	/* Black canvas on HW (tiled) so glyphs are white-on-black, not rainbow on logo. */
	memset(shadow, 0, len);

	info = framebuffer_alloc(sizeof(*par), NULL);
	if (!info) {
		vfree(shadow);
		memunmap(hw);
		return 0;
	}

	par = info->par;
	info->fix = simplefb_fix;
	strscpy(info->fix.id, "rg55splash", sizeof(info->fix.id));
	info->fix.smem_start = phys;
	/*
	 * Critically: smem_len/screen_size cover ONLY the console region.
	 * If they include the full 1920px, fbcon recalculates yres to 1920
	 * and paints black glyphs over the status strip (black speckles).
	 */
	info->fix.smem_len = (u32)RG55_CON_H * stride * 4;
	info->screen_size = info->fix.smem_len;
	info->fix.line_length = stride * 4; /* 1080 * 4 = 4320 */
	info->var = simplefb_var;
	info->var.xres = RG55_FB_W;
	info->var.yres = RG55_CON_H; /* console below top status strip */
	info->var.xres_virtual = RG55_FB_W;
	info->var.yres_virtual = RG55_CON_H;
	info->var.bits_per_pixel = 32;
	/* x8r8g8b8 / XRGB8888 — matches stock continuous-splash */
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
	/*
	 * Standard VGA 16-color palette (not neon full-intensity). Index 7/15
	 * are white so default console text stays readable.
	 */
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
	par->size = len; /* full HW mapping including status strip */
	par->mem = NULL;
	par->hw_base = hw;
	rg55g1_splash_hw = hw;
	rg55g1_splash_info = info;

	/* Black console (flushed below status); top status strip dark gray. */
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
	rg55g1_pending_fb = info;
	return 0;
}
early_initcall(rg55g1_splash_console_init);

static int rg55g1_splash_fbdev_register(void)
{
	struct fb_info *info = rg55g1_pending_fb;

	if (!info)
		return 0;

	rg55g1_pending_fb = NULL;

	if (register_framebuffer(info) < 0) {
		struct simplefb_par *par = info->par;

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
	{
		extern int rg55g1_sanitize_dt(void);

		rg55g1_sanitize_dt();
	}
	rg55g1_status("DT-OK", 0x00ff00ff);
	pr_emerg("rg55g1: /dev/fb0 ready, boot continues toward ash\n");
	return 0;
}

static int __init rg55g1_splash_fbdev_init(void)
{
	return rg55g1_splash_fbdev_register();
}
fs_initcall(rg55g1_splash_fbdev_init);

/*
 * Truncated initcall path skips fs/subsys levels; register fb0 + fbcon here.
 */
int __init rg55g1_fbmem_ensure(void);

int __init rg55g1_splash_fbdev_bringup(void)
{
	int ret;

	if (!rg55g1_pending_fb)
		return 0;

	ret = rg55g1_fbmem_ensure();
	if (ret)
		return ret;

	return rg55g1_splash_fbdev_register();
}

MODULE_AUTHOR("Stephen Warren <swarren@wwwdotorg.org>");
MODULE_DESCRIPTION("Simple framebuffer driver");
MODULE_LICENSE("GPL v2");
