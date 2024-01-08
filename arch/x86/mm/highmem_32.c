// SPDX-License-Identifier: GPL-2.0-only
#include <linux/highmem.h>
#include <linux/export.h>
#include <linux/swap.h> /* for totalram_pages */
#include <linux/memblock.h>

void __init set_highmem_pages_init(void)
{
	struct zone *zone;
	int nid;

	/*
	 * Explicitly reset zone->managed_pages because set_highmem_pages_init()
	 * is invoked before memblock_free_all()
	 */
	// 将所有node的所有zone的managed_pages置为0，即将所有管理区的所管理页数量设置为0
	reset_all_zones_managed_pages();
	for_each_zone(zone) {
		unsigned long zone_start_pfn, zone_end_pfn;

		if (!is_highmem(zone)) // 只初始化高端内存
			continue;

		zone_start_pfn = zone->zone_start_pfn;
		zone_end_pfn = zone_start_pfn + zone->spanned_pages;

		/* 该管理区所属的node结点号 */
		nid = zone_to_nid(zone);
		printk(KERN_INFO "Initializing %s for node %d (%08lx:%08lx)\n",
				zone->name, nid, zone_start_pfn, zone_end_pfn);

		add_highpages_with_active_regions(nid, zone_start_pfn,
				 zone_end_pfn);
	}
}
