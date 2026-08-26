// SPDX-License-Identifier: GPL-2.0
/*
 * RG55G1 runs LV0–LV5 normally but skips LV6+ (device/late). Manually bring
 * up filesystems that are device_initcall/module_init (LV6) so Rocknix can
 * mount VFAT/ext4/squashfs before MMC probe.
 *
 * bio/genhd/blk-mq/bdi/dquot/inotify are subsys/fs initcalls (LV4/LV5) and
 * must NOT be called again here.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include "blk.h"

int __init init_fat_fs(void);
int __init init_vfat_fs(void);
int __init ext4_init_fs(void);
int __init journal_init(void);
int __init mbcache_init(void);
int __init init_nls_cp437(void);
int __init init_nls_iso8859_1(void);
int __init loop_init(void);
int __init init_squashfs_fs(void);
int __init ovl_init(void);
#if IS_ENABLED(CONFIG_MQ_IOSCHED_DEADLINE)
int __init deadline_init(void);
#endif

int __init rg55g1_subsys_bringup(void)
{
	int ret;

#if IS_ENABLED(CONFIG_MQ_IOSCHED_DEADLINE)
	/* mq-deadline is module_init → device_initcall when built-in. */
	ret = deadline_init();
	if (ret)
		return ret;
#endif

	ret = init_fat_fs();
	if (ret)
		return ret;
	ret = init_nls_cp437();
	if (ret)
		return ret;
	ret = init_nls_iso8859_1();
	if (ret)
		return ret;
	ret = init_vfat_fs();
	if (ret)
		return ret;

	/* JBD2 + mbcache + ext4 are device_initcall when built-in. */
#if IS_ENABLED(CONFIG_JBD2)
	ret = journal_init();
	if (ret)
		return ret;
#endif
#if IS_ENABLED(CONFIG_FS_MBCACHE)
	ret = mbcache_init();
	if (ret)
		return ret;
#endif
	ret = ext4_init_fs();
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_BLK_DEV_LOOP)
	ret = loop_init();
	if (ret)
		return ret;
#endif
#if IS_ENABLED(CONFIG_SQUASHFS)
	ret = init_squashfs_fs();
	if (ret)
		return ret;
#endif
#if IS_ENABLED(CONFIG_OVERLAY_FS)
	ret = ovl_init();
	if (ret)
		return ret;
#endif

	return 0;
}
