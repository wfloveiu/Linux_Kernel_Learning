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

#define HOTNESS_TOPN_LIST
struct page_hotness_stat page_hotness_stat = {0};
#define true 1
#define false 0

static void check(struct page_hotness_data* data, bool tracked)
{
    struct list_head *head = &data->hotness_head;
    struct page_info* pi = NULL;
    int cnt = 0;
    int n = data->size + 5;
    list_for_each_entry(pi, head, page_list) {
        BUG_ON(tracked && !PageTracked(get_page_ext(pi)));
        if (cnt < n) {
            cnt ++;
        } else {
            break;
        }
    }
    // BUG_ON(cnt != data->size);
    if (cnt != data->size) {
        printk("head=%p, prev=%p, next=%p\n", head, head->prev, head->next);
    }
}


#ifdef HOTNESS_TOPN_LIST
static void page_hotness_init(struct page_hotness_data* data, int node_id)
{
    INIT_LIST_HEAD(&data->hotness_head);
	data->size = 0;
	data->capacity = PAGE_HOTNESS_MAX_NUM;
    data->node_id = node_id;
    spin_lock_init(&data->page_hotness_lock);
    check(data, false);
}

static int page_hotness_insert(struct page_hotness_data* data, struct page_info* pi, int cpu_id)
{
    struct list_head *head = &data->hotness_head;
    struct page_info *pos = NULL;
    spinlock_t *page_hotness_lock = &data->page_hotness_lock;
    BUG_ON(data->size > data->capacity);
    BUG_ON(!pi->pfn);

    spin_lock_irq(page_hotness_lock);
    // 容量上限时，先比较一下当前页与最冷的页
    if (data->size == data->capacity) {
        // head的上一个元素就是最后一个元素
        struct page_info* tail = list_last_entry(head, struct page_info, page_list);
        if (pi->access_counters[cpu_id] < tail->access_counters[cpu_id]) {
            goto out_failed;
        }
    } else if (data->size == 0) {
        list_add_tail(&pi->page_list, head);
        SetPageTracked(get_page_ext(pi));
        data->size ++;
        goto out_succeed;
    }

    list_for_each_entry(pos, head, page_list) {
        if (pi->access_counters[cpu_id] >= pos->access_counters[cpu_id]) {
            // 将pi插到pos之前
            list_add_tail(&pi->page_list, &pos->page_list);
            SetPageTracked(get_page_ext(pi));
            data->size ++;
            break;
        } else if (list_is_last(&pos->page_list, head) && data->size < data->capacity) {
            // 将pi插到尾节点之后
            list_add(&pi->page_list, &pos->page_list);
            SetPageTracked(get_page_ext(pi));
            data->size ++;

            goto out_succeed;
        }
    }

    if (data->size > data->capacity) {
        // 剔除最后一个元素
        struct page_info* tail = list_last_entry(head, struct page_info, page_list);
        list_del(&tail->page_list);
        INIT_LIST_HEAD(&tail->page_list);
        data->size --;
        ClearPageTracked(get_page_ext(tail));
    }

out_succeed:
    check(data, true);
    spin_unlock_irq(page_hotness_lock);
    BUG_ON(data->size > data->capacity);
    return 1;

out_failed:
    spin_unlock_irq(page_hotness_lock);
    check(data, false);
    return 0;
}

static int page_hotness_del_TopN(struct page_hotness_data* data, struct page_info* pi) {
    spinlock_t *page_hotness_lock = &data->page_hotness_lock;

    spin_lock_irq(page_hotness_lock);
    list_del(&pi->page_list);
    INIT_LIST_HEAD(&pi->page_list);
    ClearPageTracked(get_page_ext(pi));
    data->size --;
    check(data, false);
    spin_unlock_irq(page_hotness_lock);

    return 1;
}

static int page_hotness_update_TopN(struct page_hotness_data* data, struct page_info* pi, int cpu_id)
{
#ifdef PAGE_HOTNESS_DEBUG
    printk("page_hotness_update_TopN, nid=%d, size=%d, capacity=%d\n",
        page_to_nid(get_page_from_page_info(pi)), data->size, data->capacity);
#endif
    // BUG_ON(!PageTracked(get_page_ext(pi)));
    // printk(":ready to del &pi->page_list\n");
    // if (data->size == 1) {
    //     struct list_head *head = &data->hotness_head;
    //     BUG_ON(head != pi);
    //     printk(":there is only one page info\n");
    //     return 1;
    // }

    int status = -1;
    page_hotness_del_TopN(data, pi);
    status = page_hotness_insert(data, pi, cpu_id);
    BUG_ON(status != 1);
    // printk(":ready to insert &pi->page_list\n");
    return status;
}

static void page_hotness_traverse_TopN(struct page_hotness_data* data, int n, hotness_callback_functype f)
{
    struct list_head *head = &data->hotness_head;
    struct page_info* pos = NULL;
    int cnt = 0;
    spinlock_t *page_hotness_lock = &data->page_hotness_lock;

    spin_lock_irq(page_hotness_lock);
    list_for_each_entry(pos, head, page_list) {
        if (cnt ++ < n) {
            f(pos, NULL);
        } else {
            break;
        }
    }
    spin_unlock_irq(page_hotness_lock);
}

static void page_hotness_clear(struct page_hotness_data* data)
{
    printk("page_hotness_clear is not implemented\n");
    return ;
}

static void dump_one(struct page_info* pi, void* arg)
{
    int cpu_id;
    printk("pfn:%lx, total:%d", pi->pfn, pi->total_counter);
    for (cpu_id = 0; cpu_id < num_online_cpus(); cpu_id++) {
        printk(KERN_CONT ", [%d]=%d", cpu_id, pi->access_counters[cpu_id]);
    }
    printk(KERN_CONT "\n");
}

static void page_hotness_dump(struct page_hotness_data* data)
{
    if (data->size == 0) {
        return ;
    }
    page_hotness_traverse_TopN(data, data->size, &dump_one);
}

#elif defined(HOTNESS_TOPN_MAXHEAP)
static void maxheap_insert(struct maxheap *heap, struct page_info *pi)
{
    if (heap->size == heap->capacity) {
        printf("Heap is full. Cannot insert more elements.\n");
        return;
    }

    heap->array[heap->size] = value;
    heapifyUp(heap, heap->size);
    heap->size++;
}

void page_hotness_init()
{
    maxheap = vmalloc(sizeof(MaxHeap));

}

void page_hotness_insert(struct maxheap *heap, struct page_info *pi)
{
    maxheap_insert();
}

void page_hotness_getTopN(int n, struct list_head *list)
{
    if (list == NULL) {
        printk("error: page_hotness_getTopN is NULL\n");
    }
    maxheap_getTopN();
}

void page_hotness_clear()
{
    return;
}

#endif // HOTNESS_TOPN_LIST

static void dump_stat(void) {
    printk("dump global stat: \n");
    printk("all req in hint page fault=%d", page_hotness_stat.all_in_hint_page_fault);
    printk("migration in hint pagefault=%d", page_hotness_stat.migrate_in_hint_page_fault);
}

void dump_topN()
{
    int nid;
    printk("dump_topN\n");
    dump_stat();

    for_each_node(nid) {
        int cpu_id;
        pg_data_t *pgdat = NODE_DATA(nid);

        if (!pgdat) {
            printk("nid=%d, pgdata is null\n", nid);
            continue;
        } else {
            struct page_hotness_data *data = &pgdat->pglist_hotness_area.page_hotness_data;
            struct page_hotness_op	*page_hotness_op = pgdat->pglist_hotness_area.page_hotness_op;
            printk(">>> nid=%d, dump top N begin:\n", nid);
            printk("hotness_data: size=%d, capacity=%d ", data->size, data->capacity);
            if (data->size == 0) {
                printk(KERN_CONT "cpu_id=%d, data->size = 0\n", cpu_id);
            } else if (!page_hotness_op) {
                printk(KERN_CONT "cpu_id=%d, page_hotness_op is null\n", cpu_id);
            } else {
                printk(KERN_CONT "cpu_id=%d, data->size = %d\n", cpu_id, data->size);
                page_hotness_op->dump(data);
            }
            printk(">>> end\n");
        }
    }
}


struct page_hotness_op page_hotness_op_type_list = {
	.init = page_hotness_init,
	.insert = page_hotness_insert,
    .update = page_hotness_update_TopN,
	.traverse_TopN = page_hotness_traverse_TopN,
	.clear = page_hotness_clear,
    .dump = page_hotness_dump,
};
