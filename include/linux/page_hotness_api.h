/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_HOTNESS_API_H
#define __LINUX_PAGE_HOTNESS_API_H

#include <linux/types.h>
#include <linux/stacktrace.h>
#include <linux/stackdepot.h>
#include <linux/page_balancing.h>


#if defined(CONFIG_PAGE_HOTNESS) && defined(CONFIG_NUMA_PREDICT)
extern void update_predict_queue(void);
#else
extern void update_predict_queue(void) { }
#endif

extern void hotness_init(struct pglist_data *pgdat);
extern void dump_predict_queue_stat(void);
extern void debug_node_date(int nid);
extern void page_hotness_migrate_success(struct page *old_page);

#endif /* __LINUX_PAGE_HOTNESS_API_H */
