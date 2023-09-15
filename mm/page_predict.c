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
#include <linux/page_hotness_api.h>
#include <linux/freezer.h>

#include "internal.h"

spinlock_t my_debug_lock;
int first = 1;
static void init_my_debug_lock(void)
{
	if (first) {
		spin_lock_init(&my_debug_lock);
		first = 0;
	}
}

void test_debug_lock(void)
{
	spin_lock_irq(&my_debug_lock);
}

void test_debug_unlock(void)
{
	spin_unlock_irq(&my_debug_lock);
}

int set_pmem_node(int nid)
{
	printk("%s\n", __func__);
	NODE_DATA(nid)->pm_node=1;
	return 0;
}
EXPORT_SYMBOL(set_pmem_node);

// migrate_pages_by_migration_info
static int predict_pages(struct list_head *info_list, struct pglist_data *from_pgdat)
{
	struct migration_info *info1, *info2;
	struct list_head *migrate_list; // 待迁移的页面, 链表数组
	struct list_head *dup_list; // 待产生副本的页面
	int target_nid;
    int nr_migrate_info = 0;
    struct page *page, *page2;

	migrate_list = kmalloc(sizeof(struct list_head) * num_possible_nodes(), GFP_KERNEL);
	dup_list = kmalloc(sizeof(struct list_head) * num_possible_nodes(), GFP_KERNEL);

	for_each_online_node(target_nid) {
		INIT_LIST_HEAD(&migrate_list[target_nid]);
		INIT_LIST_HEAD(&dup_list[target_nid]);
	}

//    printk(KERN_ALERT "---target_nid:%d start migration---\n",from_pgdat->node_id);
	list_for_each_entry_safe(info1, info2, info_list, list){
        //printk(KERN_ALERT "page:%p,target_nid:%d,type:%d",info1->page,info1->target_nid,info1->type);
		if (info1->type == MIGRATE_NORMAL) {
			list_move(&info1->list, &migrate_list[info1->target_nid]);
		} else if (info1->type == MIGRATE_DUPLICATION) {
			list_move(&info1->list, &dup_list[info1->target_nid]);
		} else {
			BUG_ON(1);
		}
        nr_migrate_info++;
	}

    if (nr_migrate_info != 0) {
        printk(KERN_ALERT "target_nid:%d, nr_migrate_info:%d in this round\n",
			from_pgdat->node_id, nr_migrate_info);
    }

	for_each_online_node(target_nid) {
		predict_pages_by_migrate(&migrate_list[target_nid], target_nid, MIGRATE_SYNC);
        predict_pages_by_dup(&dup_list[target_nid], target_nid, MIGRATE_SYNC, MR_PREDICTION);
	}
	
	kfree(migrate_list);
	kfree(dup_list);

	return 0;
}


/************************************************************************************
 * *************************** kpredict thread **************************************
 * **********************************************************************************/ 
static void kpredictd_try_to_sleep(pg_data_t *pgdat)
{
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kpredictd_wait, &wait, TASK_INTERRUPTIBLE);
	if (!kthread_should_stop()){
//        if(pgdat->node_id==0)
            schedule_timeout(3*HZ);
//        else
//            schedule_timeout(MAX_SCHEDULE_TIMEOUT);
    }

	finish_wait(&pgdat->kpredictd_wait, &wait);
}

extern struct scan_control;
static int kpredictd(void *p)
{
    int have_info;
    int round;
	pg_data_t *pgdat = (pg_data_t*)p;
	LIST_HEAD(migrate_info_list);
	struct task_struct *tsk = current;
	struct mem_cgroup_reclaim_cookie reclaim = {
		.pgdat = pgdat,
		// todo:等待修改
		//.priority = DEF_PRIORITY,
	};

	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	struct mem_cgroup *root = NULL;
	struct mem_cgroup *memcg = mem_cgroup_iter(root, NULL, &reclaim);
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	struct zone *zone = &pgdat->node_zones[ZONE_NORMAL];
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);
	current->reclaim_state = &reclaim_state;

	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;

//	if(pmem_node_id!=-1 && pgdat->node_id!=pmem_node_id)
//                return 0;
    round = 0;
	for ( ; ; ) {
        have_info = 0;
		// 0.删除已有的dup_info(预测失败的page)，其中的page放回lruvec
//		printk(KERN_INFO "do sth in kpredictd\n");

		// lmy: 此处仅仅修改头节点的话，会有问题吗？（有没有什么地方用错）其它节点的next与prev节点没有被重置
        INIT_LIST_HEAD(&pgdat->migrate_list);
        BUG_ON(!list_empty(&pgdat->migrate_list));

		if (round > 7){
            if (del_miss_dup_info(pgdat) < 0){
//                printk(KERN_ALERT "del miss dup info failed\n");
                goto sleep;
            }
            round = 0;
		}

        update_predict_queue();

		// 1.对所有的pgdat,都需要分析内部的热度信息，以便传递给Dram层
		have_info += generate_migrate_info_dup(pgdat, &migrate_info_list);

		// 2.根据markov传入的migration_info加入到migrate_info_list中
        have_info += generate_migrate_info_migrate(pgdat, &migrate_info_list);

		// 注：migrate_info中应该有一位指示是使用migrate还是dup的bool

		// 3.
		// 将migrate_info_list中的page迁移到对应的node中
        if(have_info) {
            predict_pages(&migrate_info_list, pgdat);
            round++;
        }
#ifdef PAGE_MIGRATE_COUNT
        print_migrate_count(pgdat);
#endif
sleep:
		if (kthread_should_stop())
			break;
		kpredictd_try_to_sleep(pgdat);
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD);
	current->reclaim_state = NULL;

	return 0;
}

void kpredictd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

//	if (pgdat->kpredictd || pmem_node_id == -1)
	if (pgdat->kpredictd)
		return;

	pgdat->kpredictd = kthread_run(kpredictd, pgdat, "kpredictd%d", nid);

	if (IS_ERR(pgdat->kpredictd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state < SYSTEM_RUNNING);
		pr_err("Failed to start kpredictd on node %d\n", nid);
		pgdat->kpredictd = NULL;
	}
}

void kpredictd_stop(int nid)
{
	struct task_struct *kpredictd = NODE_DATA(nid)->kpredictd;

	if (kpredictd) {
		kthread_stop(kpredictd);
		NODE_DATA(nid)->kpredictd = NULL;
	}
}

static int __init kpredictd_init(void)
{
	int nid;
	printk("migrate_info_init\n");
	migrate_info_init();

	init_my_debug_lock();

	for_each_node_state (nid, N_MEMORY) {
		printk("nid=%d, kpredictd_run\n", nid);
		kpredictd_run(nid);
	}
	// kmarkovd_init();
	return 0;
}
module_init(kpredictd_init)