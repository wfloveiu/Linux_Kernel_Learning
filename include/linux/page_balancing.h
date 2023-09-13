#ifndef __LINUX_PAGE_BALANCING_H
#define __LINUX_PAGE_BALANCING_H

#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched/sysctl.h>
#include <linux/types.h>

#define MY_CPU_NUMS 100

struct page_info {
	struct list_head page_list; // top N
	unsigned long pfn;
	int8_t last_cpu; // for free_promote area
	int last_accessed; // whether the page was accessed in last scan period
	int total_counter;
    unsigned char access_counters[MY_CPU_NUMS];
};

#ifdef CONFIG_PAGE_HOTNESS
extern struct page_ext_operations page_info_ops;

extern struct page *get_page_from_page_info(struct page_info *page_info);
extern struct page_info *get_page_info_from_page(struct page *page);
extern struct page_ext *get_page_ext(struct page_info *page_info);

extern unsigned int __get_page_access_counter(struct page_info *pi, int cpu_id);
extern unsigned int mod_page_access_counter(struct page *page, unsigned int accessed, int cpu_id);
extern void reset_page_access_counter(struct page *page);
extern void reset_page_last_access(struct page *page);

extern void set_page_to_page_info(struct page *page, struct page_info *page_info);

extern void add_page_for_tracking(struct page *page, int accessed, int from_cpu);

extern int PageTracked(struct page_ext *page_ext);
extern void SetPageTracked(struct page_ext *page_ext);
extern void ClearPageTracked(struct page_ext *page_ext);

#else

static inline struct page *get_page_from_page_info(struct page_info *page_info) { return NULL; }

static inline struct page_info *get_page_info_from_page(struct page *page) { return NULL; }

static inline struct page_ext *get_page_ext(struct page_info *page_info) { return NULL; }

static inline unsigned int __get_page_access_counter(struct page_info *pi, int cpu_id) { return 0; }

static inline unsigned int mod_page_access_counter(struct page *page, unsigned int accessed, int cpu_id) { return 0; }

extern inline void reset_page_access_counter(struct page *page);

extern inline void reset_page_last_access(struct page *page);

static inline void set_page_to_page_info(struct page *page, struct page_info *page_info) { }

static inline void add_page_for_tracking(struct page *page, int accessed, int from_cpu) { }

static inline int PageTracked(struct page_ext *page_ext) { return -1; }

static inline void SetPageTracked(struct page_ext *page_ext) { }

static inline void ClearPageTracked(struct page_ext *page_ext) { }
#endif

#endif /* __LINUX_PAGE_BALANCING_H */