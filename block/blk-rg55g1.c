// SPDX-License-Identifier: GPL-2.0
/*
 * RG55G1 skips subsys/device initcalls (level 4+). Manually bring up
 * block layer, writeback, and SD-card filesystems before MMC/USB probe.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include "blk.h"

int __init init_fat_fs(void);
int __init init_vfat_fs(void);
int __init ext4_init_fs(void);
int __init init_nls_cp437(void);
int __init init_nls_iso8859_1(void);
int __init default_bdi_init(void);
int __init cgwb_init(void);
int __init blk_ioc_init(void);
#if IS_ENABLED(CONFIG_BLK_CGROUP_PUNT_BIO)
int __init blkcg_punt_bio_init(void);
#endif
#if IS_ENABLED(CONFIG_LEDS_CLASS)
int __init leds_init(void);
#endif
#if IS_ENABLED(CONFIG_MQ_IOSCHED_DEADLINE)
int __init deadline_init(void);
#endif

int __init rg55g1_subsys_bringup(void)
{
	int ret;

	ret = init_bio();
	if (ret)
		return ret;
	ret = genhd_device_init();
	if (ret)
		return ret;
	ret = blk_mq_init();
	if (ret)
		return ret;
	ret = blk_ioc_init();
	if (ret)
		return ret;
	ret = default_bdi_init();
	if (ret)
		return ret;
	ret = cgwb_init();
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_BLK_CGROUP_PUNT_BIO)
	ret = blkcg_punt_bio_init();
	if (ret)
		return ret;
#endif
#if IS_ENABLED(CONFIG_LEDS_CLASS)
	ret = leds_init();
	if (ret)
		return ret;
#endif
#if IS_ENABLED(CONFIG_MQ_IOSCHED_DEADLINE)
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
	ret = ext4_init_fs();
	if (ret)
		return ret;

	return 0;
}
