#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/mm_inline.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>

#include <linux/migrate.h>
#include <linux/page_predict.h>

#include "internal.h"

// migrate_page_list
static unsigned int migrate_page_list(struct list_head *from, int target_nid,
                                      enum migrate_mode mode)
{
    unsigned int nr_succeeded;
    int err;

    if (list_empty(from))
        return 0;

    if (target_nid == NUMA_NO_NODE)
        return 0;

    /* Demotion ignores all cpuset and mempolicy settings */
    err = migrate_pages(from, alloc_migration_page, NULL,
                        target_nid, mode, MR_PREDICTION,
                        &nr_succeeded);

    __count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);

    return nr_succeeded;
}

// migrate_page_list_by_info
int predict_pages_by_migrate(struct list_head *from, int target_nid, enum migrate_mode mode)
{
	struct pglist_data *target_pgdat;
	struct migration_info *info1, *info2;
	unsigned int nr_succeeded;
	int nr_input_info;

	LIST_HEAD(ret_pages);
	target_pgdat = NODE_DATA(target_nid);
	nr_input_info = 0;
	nr_succeeded = 0;

	if (list_empty(from))
		return 0;

	if (target_nid == NUMA_NO_NODE)
		return 0;
//    printk(KERN_ALERT "migration info from node %d:\n",target_pgdat->node_id);

	list_for_each_entry(info1, from, list) {
		if (isolate_lru_page(info1->page) != 0) {
            printk(KERN_ALERT "isolate page failed\n");
            continue;
        }

		list_add(&info1->page->lru, &ret_pages);
		nr_input_info ++;
	}

	list_for_each_entry_safe(info1, info2, from, list) {
		list_del(&info1->list);
		migrate_info_free(info1);
	}
	BUG_ON(!list_empty(from));

    if (!list_empty(&ret_pages)) {
        nr_succeeded = migrate_page_list(&ret_pages, target_nid, mode);
    }

	__count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
    target_pgdat->nr_migrate_success += nr_succeeded;
    target_pgdat->nr_migrate_fail += nr_input_info - nr_succeeded;

	// 返回值暂时没用用到?
    if (nr_succeeded == nr_input_info)
	    return nr_input_info;
    else
        return nr_succeeded - nr_input_info;
}