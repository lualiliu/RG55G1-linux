// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/fb.h>

#include <drm/drm_drv.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_prime.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_kms.h"

/* RG55G1 ABL continuous splash (drivers/soc/qcom/rg55g1_splash.c) */
extern bool rg55g1_preserve_abl_display;
extern bool rg55g1_kms_scanout_ok;
extern void __iomem *rg55g1_splash_hw;
void rg55g1_splash_blit_rgb32(const void *src, unsigned int src_pitch,
			      unsigned int x1, unsigned int y1,
			      unsigned int x2, unsigned int y2);

static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);

/*
 * fbdev funcs, to implement legacy fbdev interface on top of drm driver
 */

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(msm_fbdev,
				   drm_fb_helper_damage_range,
				   drm_fb_helper_damage_area)

static int msm_fbdev_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct drm_gem_object *bo = msm_framebuffer_bo(helper->fb, 0);

	return drm_gem_prime_mmap(bo, vma);
}

static void msm_fbdev_fb_destroy(struct fb_info *info)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct drm_framebuffer *fb = helper->fb;
	struct drm_gem_object *bo = msm_framebuffer_bo(fb, 0);
	bool splash_direct = info->screen_buffer == (void *)rg55g1_splash_hw;

	DBG();

	drm_fb_helper_fini(helper);

	/* Splash-backed fbdev never took a GEM vaddr. */
	if (!splash_direct)
		msm_gem_put_vaddr(bo);
	drm_framebuffer_remove(fb);

	drm_client_release(&helper->client);
}

static const struct fb_ops msm_fb_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DEFERRED_OPS_RDWR(msm_fbdev),
	DRM_FB_HELPER_DEFAULT_OPS,
	__FB_DEFAULT_DEFERRED_OPS_DRAW(msm_fbdev),
	.fb_mmap = msm_fbdev_mmap,
	.fb_destroy = msm_fbdev_fb_destroy,
};

static int msm_fbdev_fb_dirty(struct drm_fb_helper *helper,
			      struct drm_clip_rect *clip)
{
	struct drm_device *dev = helper->dev;
	struct fb_info *info = helper->info;
	int ret;

	/* Call damage handlers only if necessary */
	if (!(clip->x1 < clip->x2 && clip->y1 < clip->y2))
		return 0;

	/*
	 * Until CTL_FLUSH clears, ABL still scans splash RAM. Draw/mirror into
	 * that buffer (self-blit becomes a cache clean when already mapped).
	 */
	if (rg55g1_preserve_abl_display && !rg55g1_kms_scanout_ok &&
	    rg55g1_splash_hw && info && info->screen_buffer) {
		rg55g1_splash_blit_rgb32(info->screen_buffer,
					 info->fix.line_length,
					 clip->x1, clip->y1,
					 clip->x2, clip->y2);
		return 0;
	}

	if (helper->fb->funcs->dirty) {
		ret = helper->fb->funcs->dirty(helper->fb, NULL, 0, 0, clip, 1);
		if (drm_WARN_ONCE(dev, ret, "Dirty helper failed: ret=%d\n", ret))
			return ret;
	}

	return 0;
}

static const struct drm_fb_helper_funcs msm_fbdev_helper_funcs = {
	.fb_dirty = msm_fbdev_fb_dirty,
};

int msm_fbdev_driver_fbdev_probe(struct drm_fb_helper *helper,
				 struct drm_fb_helper_surface_size *sizes)
{
	struct drm_device *dev = helper->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct fb_info *fbi = helper->info;
	struct drm_framebuffer *fb = NULL;
	struct drm_gem_object *bo;
	uint64_t paddr;
	uint32_t format;
	int ret, pitch;

	format = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);

	DBG("create fbdev: %dx%d@%d (%dx%d)", sizes->surface_width,
			sizes->surface_height, sizes->surface_bpp,
			sizes->fb_width, sizes->fb_height);

	pitch = align_pitch(sizes->surface_width, sizes->surface_bpp);
	fb = msm_alloc_stolen_fb(dev, sizes->surface_width,
			sizes->surface_height, pitch, format);

	if (IS_ERR(fb)) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate fb\n");
		return PTR_ERR(fb);
	}

	bo = msm_framebuffer_bo(fb, 0);

	/*
	 * NOTE: if we can be guaranteed to be able to map buffer
	 * in panic (ie. lock-safe, etc) we could avoid pinning the
	 * buffer now:
	 */
	ret = msm_gem_get_and_pin_iova(bo, priv->kms->vm, &paddr);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to get buffer obj iova: %d\n", ret);
		goto fail;
	}

	DBG("fbi=%p, dev=%p", fbi, dev);

	helper->funcs = &msm_fbdev_helper_funcs;
	helper->fb = fb;

	fbi->fbops = &msm_fb_ops;

	drm_fb_helper_fill_info(fbi, helper, sizes);

	/*
	 * ABL scans 0xb8000000 @ 1080×1920×4. Point fbdev there so console
	 * writes are HW-visible without waiting for CTL_FLUSH. Keep a GEM fb
	 * for DRM bookkeeping; plane/CTL programming is skipped under ABL.
	 */
	if (rg55g1_preserve_abl_display && !rg55g1_kms_scanout_ok &&
	    rg55g1_splash_hw) {
		fbi->screen_buffer = (void *)rg55g1_splash_hw;
		fbi->flags |= FBINFO_VIRTFB;
		fbi->screen_size = (size_t)1080 * 1920 * 4;
		fbi->fix.line_length = 1080 * 4;
		fbi->fix.smem_start = 0xb8000000UL;
		fbi->fix.smem_len = fbi->screen_size;
	} else {
		fbi->screen_buffer = msm_gem_get_vaddr(bo);
		if (IS_ERR(fbi->screen_buffer)) {
			ret = PTR_ERR(fbi->screen_buffer);
			goto fail;
		}
		fbi->flags |= FBINFO_VIRTFB;
		fbi->screen_size = bo->size;
		fbi->fix.smem_start = paddr;
		fbi->fix.smem_len = bo->size;
	}

	DBG("par=%p, %dx%d", fbi->par, fbi->var.xres, fbi->var.yres);
	DBG("allocated %dx%d fb", fb->width, fb->height);

	return 0;

fail:
	drm_framebuffer_remove(fb);
	return ret;
}
