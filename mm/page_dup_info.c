#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/slab.h>

#include <linux/page_predict.h>

static struct kmem_cache *dup_info_cachep;

void dup_info_cache_init(void)
{
	dup_info_cachep = KMEM_CACHE(dup_info, SLAB_PANIC | SLAB_ACCOUNT);
}

struct dup_info *dup_info_alloc(gfp_t gfp)
{
	struct dup_info *info;
	info = kmem_cache_alloc(dup_info_cachep, gfp);
	if (info) {
		info->old_page = NULL;
		info->dup_page = NULL;
		INIT_LIST_HEAD(&info->list);
		info->crc = 0;
	}

	return info;
}

// set_numa_duplication_info
struct dup_info *init_dup_info(struct page *old_page, struct page *new_page)
{
	struct dup_info *di = dup_info_alloc(GFP_KERNEL);
    BUG_ON(!di);
    
    di->old_page = old_page;
    di->dup_page = new_page;

    return di;
}

inline void dup_info_free(struct dup_info *dup_info)
{
	kmem_cache_free(dup_info_cachep, dup_info);
}


// mv_page_to_dup_list
int add_page_to_dup_list(struct page *page, int lock)
{
	struct pglist_data *pgdat = page_pgdat(page);

	if (lock) {
		spin_lock_irq(&pgdat->duplist_lock);
		if (!spin_is_locked(&pgdat->duplist_lock)) {
        	printk(KERN_ALERT "get duplist_lock failed\n");
			dump_stack();
			BUG_ON(1);
        	return 0;
		}
	}

	list_add(&page->lru, &pgdat->duppage_list);

	if (lock) {
		spin_unlock_irq(&pgdat->duplist_lock);
	}

	return 1;
}


void dump_dup_info(struct dup_info *dup_info)
{
	if ((unsigned long)dup_info->old_page < VMEMMAP_START || 
			(unsigned long)dup_info->dup_page < VMEMMAP_START) {
		printk("dup_info=%px, old_page=%px, dup_page=%px\n", dup_info, dup_info->old_page, dup_info->dup_page);
	}
}

void dump_predict_page(struct page *page)
{
	if ((unsigned long)page < VMEMMAP_START) {
		printk("page=%px\n", page);
	}
}