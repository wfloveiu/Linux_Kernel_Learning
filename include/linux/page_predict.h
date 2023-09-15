#ifndef __LINUX_PAGE_PREDICT_H
#define __LINUX_PAGE_PREDICT_H

#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/rmap.h>
#include <linux/mm_types.h>
#include <linux/migrate.h>

// migrate
enum migrate_type {
    MIGRATE_UNDEFINED = -1,
    MIGRATE_NORMAL = 0,
    MIGRATE_DUPLICATION = 1,
};

extern spinlock_t my_debug_lock;
extern void test_debug_lock(void);
extern void test_debug_unlock(void);

struct migration_info {
	struct page *page;
	int target_nid;
	int type;
	struct list_head list;
};

// migration_info operation
extern void migrate_info_init(void);
extern struct migration_info *migrate_info_alloc(gfp_t gfp);
extern inline void migrate_info_free(struct migration_info *info);
extern struct migration_info *add_migrate_info_to_list(struct page *page, int target_nid, int type, int lock);

extern int generate_migrate_info_migrate(struct pglist_data *pgdat, struct list_head *list);
extern int generate_migrate_info_dup(struct pglist_data *pgdat, struct list_head *list);
extern int migrate_pages_by_migration_info(struct list_head *info_list, struct pglist_data *pgdat);

// mm/page_predict_migrate.c
// page_numa_duplication
extern int predict_pages_by_migrate(struct list_head *from, int target_nid, enum migrate_mode mode);

// dup
struct dup_info {
	struct list_head list;
	struct page* old_page;
	struct page* dup_page;
	unsigned long crc;
};

// dup_info operation
extern void dup_info_cache_init(void);
extern struct dup_info *dup_info_alloc(gfp_t gfp);
struct dup_info *init_dup_info(struct page *old_page, struct page *new_page);
extern inline void dup_info_free(struct dup_info *dup_info);
extern int add_page_to_dup_list(struct page *page, int lock);
extern int add_dup_info_to_dup_list(struct dup_info *di, struct pglist_data *pgdat, int lock);

// mm/page_predict_dup.c
extern int predict_pages_by_dup(struct list_head *from,
                          int target_nid, enum migrate_mode mode,
                          enum migrate_reason reason);
extern int handle_dup_page_fault(struct page *page, struct vm_area_struct *vma, struct vm_fault *vmf);


extern struct page *alloc_migration_page(struct page *page, unsigned long node);
extern int migrate_page_mapping(struct address_space *mapping, struct page *newpage,
			 struct page *page, enum migrate_mode mode);
extern int migrate_file_page_mapping(struct address_space *mapping,
			      struct page *newpage, struct page *page,
			      enum migrate_mode mode);
extern int del_miss_dup_info(struct pglist_data *pgdat);

// mm/page_predict.c
extern void kpredictd_run(int nid);
extern void kpredictd_stop(int nid);

extern void dump_dup_info(struct dup_info *dup_info);
extern void dump_predict_page(struct page* page);

inline bool my_remove_migration_pte(struct page *page, struct vm_area_struct *vma,
				 unsigned long addr, void *old);

#endif // __LINUX_PAGE_PREDICT_H