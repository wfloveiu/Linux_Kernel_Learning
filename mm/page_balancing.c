#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/uaccess.h>
#include <linux/jump_label.h>
#include <linux/memcontrol.h>
#include <linux/node.h>

// lmy
#include <linux/page_balancing.h>
#include <linux/page_owner.h>
#include <linux/page_ext.h>
#include <linux/page_hotness.h>
#include <linux/printk.h>

#include "internal.h"

#ifdef CONFIG_PAGE_HOTNESS
static inline struct page_info *get_page_info(struct page_ext *page_ext)
{
	return (void *)page_ext + page_info_ops.offset;
}

struct page_ext *get_page_ext(struct page_info *page_info)
{
	return (void *)page_info - page_info_ops.offset;
}

struct page *get_page_from_page_info(struct page_info *page_info)
{
	if (page_info->pfn)
		return pfn_to_page(page_info->pfn);
	else
		return NULL;
}

struct page_info *get_page_info_from_page(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (!page_ext)
		return NULL;
	return get_page_info(page_ext);
}



static inline void __init_page_info(struct page_ext *page_ext)
{
	struct page_info *pi = get_page_info(page_ext);
	pi->pfn = 0;
	pi->last_cpu = -1;
	pi->total_counter = 0;
	memset(pi->access_counters, 0, sizeof(unsigned char) * MY_CPU_NUMS);
	pi->last_accessed = 0;

	ClearPageTracked(page_ext);
	INIT_LIST_HEAD(&pi->page_list);
	// 为什么这里不需要初始化page_list?
}

static void init_pages_in_zone(pg_data_t *pgdat, struct zone *zone)
{
	unsigned long pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count = 0;

	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	for (; pfn < end_pfn; ) {
		unsigned long block_end_pfn;

		if (!pfn_valid(pfn)) {
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = ALIGN(pfn + 1, pageblock_nr_pages);
		block_end_pfn = min(block_end_pfn, end_pfn);

		for (; pfn < block_end_pfn; pfn++) {
			struct page *page = pfn_to_page(pfn);
			struct page_ext *page_ext;

			if (page_zone(page) != zone)
				continue;

			/*
			 * To avoid having to grab zone->lock, be a little
			 * careful when reading buddy page order. The only
			 * danger is that we skip too much and potentially miss
			 * some early allocated pages, which is better than
			 * heavy lock contention.
			 */
			if (PageBuddy(page)) {
				unsigned long order = buddy_order_unsafe(page);

				if (order > 0 && order < MAX_ORDER)
					pfn += (1UL << order) - 1;
				continue;
			}

			if (PageReserved(page))
				continue;

			page_ext = lookup_page_ext(page);
			if (unlikely(!page_ext))
				continue;

			/* Maybe overlapping zone */
			if (PageTracked(page_ext))
				continue;

			/* Found early allocated page */
			__init_page_info(page_ext);
			count++;
		}
		cond_resched();
	}

	pr_info("Node %d, zone %8s: page owner found early allocated %lu pages\n",
		pgdat->node_id, zone->name, count);
}

static void init_zones_in_node(pg_data_t *pgdat)
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;

		init_pages_in_zone(pgdat, zone);
	}
}

static void init_early_allocated_pages(void)
{
	pg_data_t *pgdat;

	for_each_online_pgdat(pgdat)
		init_zones_in_node(pgdat);
}

static void init_page_balancing(void)
{
	init_early_allocated_pages();
	printk("init_page_balancing\n");
}

static bool need_page_balancing(void)
{
	return true;
}

struct page_ext_operations page_info_ops = {
	.size = sizeof(struct page_info),
	.need = need_page_balancing,
	.init = init_page_balancing,
};

static unsigned int inc_page_access_counter(struct page_info *pi, int cpu_id)
{
	u8 old_access_counter = pi->access_counters[cpu_id];

	pi->total_counter += (old_access_counter < 255);
	pi->access_counters[cpu_id] += (old_access_counter < 255);
	pi->last_accessed = 0; // reset last_accessed
#ifdef CONFIG_PAGE_HOTNESS_DEBUG
	// printk("pfn=%lx, access_counter update from %d to %d\n", pi->pfn, old_access_counter, pi->access_counters[cpu_id]);
#endif
	return pi->access_counters[cpu_id];
}

static unsigned int record_page_last_accessed(struct page_info *pi)
{
	pi->last_accessed += (pi->last_accessed < 255);
	// printk("pfn=%lx, last_accessed is decreased to %d\n", pi->pfn, pi->last_accessed);
	
	// 设置一个阈值，如果大于这个阈值就认为它已经冷了
	if (pi->last_accessed > 8) { // 暂时硬编码为8
		pi->total_counter -= (pi->total_counter > 0);
		// printk("pfn=%lx, total_counter is decreased to %d\n", pi->pfn, pi->total_counter);

		if (pi->total_counter == 0){
			pi->last_accessed = 0; // pi is reseted
			memset(pi->access_counters, 0, sizeof(unsigned char) * MY_CPU_NUMS);
#ifdef CONFIG_PAGE_HOTNESS_DEBUG
			// printk("pfn=%lx, pi is reseted\n", pi->pfn);
#endif
		}
	}
	return pi->total_counter;
}

unsigned int mod_page_access_counter(struct page *page, unsigned int accessed, int cpu_id)
{
	struct page_ext *page_ext = NULL;
	struct page_info *pi = NULL;
	BUG_ON(cpu_id > MY_CPU_NUMS);

	page_ext = lookup_page_ext(page);
	pi = get_page_info(page_ext);
	set_page_to_page_info(page, pi);

	if (accessed) {
		return inc_page_access_counter(pi, cpu_id);
	} else {
		record_page_last_accessed(pi);
		return pi->access_counters[cpu_id];
	}
}

void reset_page_access_counter(struct page *page)
{
	struct page_ext *page_ext;
	struct page_info *pi;
	page_ext = lookup_page_ext(page);
	pi = get_page_info(page_ext);

	pi->total_counter = 0;
	memset(pi->access_counters, 0, sizeof(unsigned char) * MY_CPU_NUMS);
}

void reset_page_last_access(struct page *page)
{
	struct page_ext *page_ext;
	struct page_info *pi;

	page_ext = lookup_page_ext(page);
	pi = get_page_info(page_ext);

	pi->last_accessed = 0;
}

unsigned int __get_page_access_counter(struct page_info *pi, int cpu_id)
{
	BUG_ON(cpu_id > MY_CPU_NUMS);
	u8 counter = pi->access_counters[cpu_id];
	return counter;
}

void set_page_to_page_info(struct page *page,
				struct page_info *page_info)
{
	page_info->pfn = page_to_pfn(page);
}

// PAGE_EXT_TRACKED
inline int PageTracked(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

inline void SetPageTracked(struct page_ext *page_ext)
{
	return set_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

inline void ClearPageTracked(struct page_ext *page_ext)
{
	__clear_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

// PAGE_EXT_MIGRATING
inline int PageMigrating(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
}

inline void SetPageMigrating(struct page_ext *page_ext)
{
	return set_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
}

inline void ClearPageMigrating(struct page_ext *page_ext)
{
	__clear_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
}


void add_page_for_tracking(struct page *page, int accessed, int from_cpu)
{
	struct page_ext *page_ext = NULL;
	struct page_info *pi = NULL;
	struct pglist_data *pgdat = NULL;
	struct page_hotness_op	*page_hotness_op = NULL;
	int access_counter;

	// BUG_ON(!PagePresent(page));
	/* Skip tail pages */
	if (PageTail(page)) {
		return;
	}

	// 这个不太理解，为什么大于1的就不管了，是认为它比较热吗
	// 好像是在过滤compound page
	if (page_count(page) > 1) {
		return;
	}

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext)) {
		// 为什么page_ext会有空的情况
		printk("page_ext is NULL\n");
        // todo: why page_ext is null?
        BUG_ON(1);
		return;
	}
	pgdat = page_pgdat(page);
	page_hotness_op = pgdat->pglist_hotness_area.page_hotness_op;

	// spin_lock_irq(&pgdat->__lruvec.lru_lock);

	pi = get_page_info(page_ext);
	set_page_to_page_info(page, pi);
	access_counter = __get_page_access_counter(pi, from_cpu);

	if (PageTracked(page_ext)) {
		// 通过last_cpu是否可以获取到hotness_data

		// 表示其已经在TopN里面了
		int success = page_hotness_op->update(&pgdat->pglist_hotness_area.page_hotness_data, pi, from_cpu);
		// printk("pfn:%lx, access_counter=%d, is update\n", pi->pfn, access_counter);
	} else {
		BUG_ON(!pi->pfn);
		int success = page_hotness_op->insert(&pgdat->pglist_hotness_area.page_hotness_data, pi, from_cpu);
        // todo: update stat

		// if (success) {
			// printk("pfn:%lx, access_counter=%d, is inserted to hotness top N\n", pi->pfn, access_counter);
		// }
		// else {
		// 	// printk("pfn:%lx, access_counter=%d, is not inserted to hotness top N", pi->pfn, access_counter);
		// }
	}

	// spin_unlock_irq(&pgdat->__lruvec.lru_lock);

	// my_info("add_page_for_tracking finish, dump top N\n");
	// dump_topN();

    return ;
}
#endif