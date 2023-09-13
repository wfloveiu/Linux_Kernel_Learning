#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/page_hotness.h>
#include <linux/page_balancing.h>
#include <linux/nodemask.h>
#include <linux/bug.h>
#include <linux/page_ext.h>

#include <linux/page_predict.h>

struct predict_queue_stat {
    int nr_scan_page;
    int nr_small_counter;
    int nr_equal_nid;
    int nr_enqueue;
};
struct predict_queue_stat predict_queue_stat = {0};

void dump_predict_queue_stat(void)
{
    printk("predict_queue:\n");
	printk("predict_queue_stat.nr_scan_page=%d\n", predict_queue_stat.nr_scan_page);
	printk("predict_queue_stat.nr_small_counter=%d\n", predict_queue_stat.nr_small_counter);
	printk("predict_queue_stat.nr_equal_nid=%d\n", predict_queue_stat.nr_equal_nid);
	printk("predict_queue_stat.nr_enqueue=%d\n", predict_queue_stat.nr_enqueue);
	printk("\n");
}

static void update_single_node_predict_queue(struct page_hotness_data *data, int src_nid) {
    struct page_info* pi;
    int cpu_id;
    struct page* page;
    struct list_head* hotness_head;
    spinlock_t *page_hotness_lock;

    hotness_head = &data->hotness_head;
    page_hotness_lock = &data->page_hotness_lock;

    // 访问topN需要加锁
    spin_lock_irq(page_hotness_lock);
    if (data->size == 0) {
        goto out;
    }

    list_for_each_entry(pi, hotness_head, page_list) {
        int total_counter = pi->total_counter;
        unsigned char *access_counters = pi->access_counters;
        
        page = get_page_from_page_info(pi);
        BUG_ON(!page);
        BUG_ON(src_nid != page_to_nid(page));
        
        for (cpu_id = 0; cpu_id < num_online_cpus(); cpu_id++) {
            int access_counter = access_counters[cpu_id];
            // 这里的cpu_id如果超过实际的cpu数目，会出错
            int target_nid = cpu_to_mem(cpu_id);

            ++predict_queue_stat.nr_scan_page;
            // access_counter=1, total_counter=1
            // todo: 目前这里的阈值并不合理, 后面需要调高, 目前是为了快速触发页面迁移
            if (access_counter < 1 || access_counter * 2 < total_counter) {
                ++predict_queue_stat.nr_small_counter;
                continue;
            }

            if (src_nid == target_nid) {
                ++predict_queue_stat.nr_equal_nid;
                continue;
            }

            // 避免重复添加
            if (test_bit(PAGE_EXT_MIGRATING, &lookup_page_ext(page)->flags)) {
                continue;
            }

            // printk("page=%px, pi=%px, access_counter=%d, total_counter=%d\n", page, pi, access_counter, total_counter);
            // printk("page=%p, from_nid=%d, target_nid=%d\n", page, page_to_nid(page), target_nid);
//                BUG_ON(PageUnevictable(page));

            // 函数里面涉及migrate_info的锁
            struct migration_info *migrate_info = add_migrate_info_to_list(page, target_nid, MIGRATE_DUPLICATION, 1);
            if (migrate_info) {
                migrate_info->type = MIGRATE_DUPLICATION;
                ++predict_queue_stat.nr_enqueue;
            } else {
                printk(KERN_ALERT "migrate_info_alloc failed\n");
            }
            
            set_bit(PAGE_EXT_MIGRATING, &lookup_page_ext(page)->flags);
        }
    }

out:
    spin_unlock_irq(page_hotness_lock);
    return ;
}

void update_predict_queue(void) {
    int src_nid;
    pg_data_t *src_pgdat = NULL;
    struct page_hotness_data *data = NULL;
    // struct page_hotness_op *page_hotness_op = NULL;
    struct list_head *hotness_head = NULL;
    for_each_node(src_nid) {
        // printk("src_nid=%d\n", src_nid);
        src_pgdat = NODE_DATA(src_nid);
        if (!src_pgdat) {
            //printk("src_nid=%d, pgdata is null\n", src_nid);
            continue;
        } else {
            //printk("src_nid=%d, pgdata=%p\n", src_nid, src_pgdat);
        }

        data = &src_pgdat->pglist_hotness_area.page_hotness_data;
        // page_hotness_op = src_pgdat->pglist_hotness_area.page_hotness_op;

        update_single_node_predict_queue(data, src_nid);
    }
}

void page_hotness_migrate_success(struct page *old_page)
{
    struct pglist_data *pgdat = page_pgdat(old_page);
    struct page_info *pi = get_page_info_from_page(old_page);
    struct page_ext *page_ext = lookup_page_ext(old_page);
    struct page_hotness_data *data = &pgdat->pglist_hotness_area.page_hotness_data;
    spinlock_t *page_hotness_lock = &data->page_hotness_lock;
    struct page_info *page_info1;
    struct list_head *prev, *next, *entry;

    entry = &pi->page_list;
    prev = entry->prev;
    next = entry->next;
    
    spin_lock_irq(page_hotness_lock);

    // del pi from TopN list
    printk(KERN_ALERT "page_hotness_migrate_success,list_del(&pi->page_list);");
//    list_for_each_entry(page_info1,&data->hotness_head,page_list){
//        printk(KERN_ALERT "page_info:%px,prev:%px,next:%px",page_info1,page_info1->page_list.prev,page_info1->page_list.next);
//    }
    list_del(&pi->page_list);
    INIT_LIST_HEAD(&pi->page_list);
//    __list_del_entry(&pi->page_list);
//    if(prev->next != entry){
//        printk(KERN_ALERT "list_del corruption. prev->next should be %px, but was %px\n",
//                entry, prev->next);
//    }
//    else if(next->prev != entry){
//        printk(KERN_ALERT "list_del corruption. next->prev should be %px, but was %px\n",
//                entry, next->prev);
//    }
//    printk(KERN_ALERT "pi->page_list.next->prev:%px",((pi->page_list).next)->prev);
//    printk(KERN_ALERT "pi:%px,next:%px,prev:%px",&pi->page_list,pi->page_list.next,pi->page_list.prev);
//    printk(KERN_ALERT "pi->page_list.prev->next:%px",pi->page_list.prev->next);
//    pi->page_list.next->prev = pi->page_list.prev;
//    printk(KERN_ALERT "pi->page_list.next->prev = prev;");
//    WRITE_ONCE(pi->page_list.prev->next, pi->page_list.next);
//    printk(KERN_ALERT "WRITE_ONCE(pi->page_list.prev->next, pi->page_list.next);");
//    pi->page_list.next = LIST_POISON1;
//    pi->page_list.prev = LIST_POISON2;
//    printk(KERN_ALERT "LIST_POISON");
    data->size --;
    printk(KERN_ALERT "list_del(&pi->page_list) end in node %d",pgdat->node_id);

    // clear migrating flag
    BUG_ON(!test_bit(PAGE_EXT_MIGRATING, &page_ext->flags));
    clear_bit(PAGE_EXT_MIGRATING, &page_ext->flags);

    // clear page tracked flag
    // BUG_ON(!PageTracked(lookup_page_ext(old_page)));

    ClearPageTracked(get_page_ext(pi));
    reset_page_access_counter(old_page);
    reset_page_last_access(old_page);
    if(spin_is_locked(page_hotness_lock)){
//        printk(KERN_ALERT "unlock page_hotness_lock\n");
        spin_unlock_irq(page_hotness_lock);
//        printk(KERN_ALERT "unlock page_hotness_lock end\n");
    }
}

#ifdef CONFIG_PAGE_HOTNESS
void hotness_init(struct pglist_data *pgdat)
{
	// struct page_hotness_data* data_arr = NULL;
	struct page_hotness_data* data = NULL;
	int nid;
	printk("hotness_init\n");

	for_each_node(nid) {
		// N_MEMORY
		debug_node_date(nid);
	}

	pgdat->pglist_hotness_area.page_hotness_op = &page_hotness_op_type_list;
	data = &pgdat->pglist_hotness_area.page_hotness_data;
	pgdat->pglist_hotness_area.page_hotness_op->init(data, pgdat->node_id);

	// data = kmalloc(sizeof(spinlock_t) * nr_cpus, GFP_KERNEL);
	spin_lock_init(&pgdat->migrate_info_lock);
	INIT_LIST_HEAD(&pgdat->migrate_list);
}


void debug_node_date(int nid)
{
	printk("debug_node_date\n");
	printk("nid=%d\n", nid);
	if (node_state(nid, N_POSSIBLE))
		printk("N_POSSIBLE\n");
	if (node_state(nid, N_ONLINE))
		printk("N_ONLINE\n");
	if (node_state(nid, N_NORMAL_MEMORY))
		printk("N_NORMAL_MEMORY\n");
	#ifdef CONFIG_HIGHMEM
	if (node_state(nid, N_HIGH_MEMORY))
		printk("N_HIGH_MEMORY\n");
	#else
	if (node_state(nid, N_HIGH_MEMORY))
		printk("N_HIGH_MEMORY(==N_NORMAL_MEMORY)\n");
	#endif
	if (node_state(nid, N_MEMORY))
		printk("N_MEMORY\n");
	if (node_state(nid, N_CPU))
		printk("N_CPU\n");
	if (node_state(nid, N_GENERIC_INITIATOR))
		printk("N_GENERIC_INITIATOR\n");
}

#endif
