#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/slab.h>

#include <linux/page_predict.h>

static struct kmem_cache *migrate_info_cachep;

void migrate_info_init(void)
{
	migrate_info_cachep = KMEM_CACHE(migration_info, SLAB_PANIC | SLAB_ACCOUNT);
}

inline struct migration_info *migrate_info_alloc(gfp_t gfp)
{
	struct migration_info *info;
	info = kmem_cache_alloc(migrate_info_cachep, gfp);
	if (info) {
		info->page = NULL;
		info->target_nid = NUMA_NO_NODE;
		info->type = MIGRATE_UNDEFINED;
		INIT_LIST_HEAD(&info->list);
	}
	return info;
}

inline void migrate_info_free(struct migration_info *info)
{
	kmem_cache_free(migrate_info_cachep, info);
}

struct migration_info *add_migrate_info_to_list(struct page *page, int target_nid, int type, int lock)
{
	pg_data_t *pgdat = page_pgdat(page);;
	struct migration_info *info;
	info = migrate_info_alloc(GFP_KERNEL);

	info->page = page;
	info->target_nid = target_nid;
	info->type = type;

	if (lock) {
		spin_lock_irq(&pgdat->migrate_info_lock);
	}
	list_add(&info->list, &pgdat->migrate_list);
	if (lock) {
		spin_unlock_irq(&pgdat->migrate_info_lock);
	}
	
	return info;
}

int generate_migrate_info_migrate(struct pglist_data *pgdat, struct list_head *ret_list)
{
	int rc = 0;
	// 获取抓取的热度数据
	// 通过热度数据计算需要制作副本的page和目标nid(migration_info)
	// 将migrate_info加入到队列中
	// add_migrate_info_to_list(page,nid,MIGRATE_NORMAL,ret_list);

    struct migration_info *info1,*info2;

    if(!spin_trylock_irq(&pgdat->migrate_info_lock)) {
        printk(KERN_ALERT "kpredictd%d can't get migrate_info_lock,skip",pgdat->node_id);
        return 0;
    }
    
	if (list_empty(&pgdat->migrate_list)) {
        goto unlock;
    }
	
//    list_for_each_entry_safe (info1, info2, &pgdat->migrate_list, ret_list) {
//        printk(KERN_ALERT "page:%p,page_nid:%d,target_nid:%d,type:%d,page_lru:%d", info1->page,
//               page_to_nid(info1->page), info1->target_nid, info1->type, page_lru(info1->page));
//    }
    list_splice(&pgdat->migrate_list, ret_list);
//    printk(KERN_ALERT "-----------migration info in pgdat %d-----------",pgdat->node_id);
//    printk(KERN_INFO "there are some entries in pgdat[%d]->migrate_list\n",pgdat->node_id);
    rc = 1;

	list_for_each_entry(info1, ret_list, list) {
		if ((unsigned long)info1->page < __PAGE_OFFSET) {
			printk("info1=%px, page=%px\n", info1, info1->page);
		}
	}

unlock:
    spin_unlock_irq(&pgdat->migrate_info_lock);
	return rc;
}


int generate_migrate_info_dup(struct pglist_data *pgdat, struct list_head *ret_list)
{
	int rc = 0;
	// 获取抓取的trace
	// 通过trace计算需要迁移的page和目标nid(migration_info)
	// 将migrate_info加入到队列中

	return rc;
}

// PAGE_MIGRATE_COUNT
#ifdef NUMA_PREDICT_COUNT
static void print_migrate_count(pg_data_t *pgdat)
{
    printk(KERN_INFO "nr_migrate_success:%d",pgdat->nr_migrate_success);
//    printk(KERN_INFO "nr_dup_success:%d",pgdat->nr_dup_success);
//    printk(KERN_INFO "nr_migrate_fail:%d",pgdat->nr_migrate_fail);
//    printk(KERN_INFO "nr_dup_fail:%d",pgdat->nr_dup_fail);
    return;
}
#endif