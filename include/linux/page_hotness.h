/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_HOTNESS_H
#define __LINUX_PAGE_HOTNESS_H

#include <linux/types.h>
#include <linux/stacktrace.h>
#include <linux/stackdepot.h>
#include <linux/page_balancing.h>

#define PAGE_HOTNESS_MAX_NUM 10

// data
struct pglist_data;
struct page_hotness_data {
	struct list_head hotness_head; // struct page_info
	spinlock_t page_hotness_lock;
	int node_id;
	int size;
	int capacity;
};
typedef void (hotness_callback_functype)(struct page_info*, void* arg);

// unused
struct maxheap {
    int* array;
    int capacity; // array capacity
    int size; // element size <= capacity
};

// operation
struct page_hotness_op {
	void (*init)(struct page_hotness_data*, int node_id);
	int (*insert)(struct page_hotness_data*, struct page_info*, int cpu_id);
	int (*update)(struct page_hotness_data*, struct page_info*, int cpu_id);
	void (*traverse_TopN)(struct page_hotness_data* data, int n, hotness_callback_functype f);
	void (*clear)(struct page_hotness_data*);
	void (*dump)(struct page_hotness_data*);
	void (*check)(struct page_hotness_data*);
};

#ifdef CONFIG_PAGE_HOTNESS
extern struct page_hotness_op page_hotness_op;
extern void dump_topN(void);
extern struct page_hotness_op page_hotness_op_type_list;
#else
static inline void dump_topN(void) { }
#endif

#if defined(CONFIG_PAGE_HOTNESS) && defined(CONFIG_NUMA_PREDICT)
extern void update_predict_queue(void);
#else
extern void update_predict_queue(void) { }
#endif

// stat
struct page_hotness_stat {
	int migrate_in_hint_page_fault;
	int all_in_hint_page_fault;
};
extern struct page_hotness_stat page_hotness_stat;

extern void hotness_init(struct pglist_data *pgdat);
extern void dump_predict_queue_stat(void);
extern void debug_node_date(int nid);
extern void page_hotness_migrate_success(struct page *old_page);

#endif /* __LINUX_PAGE_HOTNESS_H */
