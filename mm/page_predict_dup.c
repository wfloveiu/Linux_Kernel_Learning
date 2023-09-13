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

#include "internal.h"

#include <linux/types.h>
#include <linux/page_predict.h>
#include <linux/page_change_prot.h>

static void crc_init_in_numa_dup(struct dup_info *dup)
{
	struct page *page = dup->old_page;
	void *addr;
	addr = page_address(page);
	dup->crc = *(unsigned long *)addr;
}

static bool crc_check_in_numa_dup(struct dup_info *dup)
{
	struct page *page = dup->old_page;
	unsigned long *addr;
    // todo
	addr = page_address(page);
	if (dup->crc == *(unsigned long *)addr) {
		return true;
	}
//	return true;
	return false;
}

// remove_migration_pte(),重建numa duplication上的pte
// 将指向旧页的pte中，task在其他node上的pte设为numa pgf的状态
// set_numa_dup_pte_pgf
static bool do_unmap_dup_page(struct page *page,
				       struct vm_area_struct *vma,
				       unsigned long addr, void *old)
{
	pte_t pte;
	unsigned long nr_pte_updates = 0;
	// struct page *new;
	// swp_entry_t entry;

    // if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
    //         is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
    //     // lmy: 这里应该return true还是false?
    //     // 需不需要在rwc中再加一个回调函数
    //     return true;
    // }

    // if (!vma->vm_mm ||
    //         (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
    //     return true;

    // if (!vma_is_accessible(vma))
    //     return true;

	// VM_BUG_ON_PAGE(PageTail(page), page);

    // lmy: 只unmap远端的pte
    // if (cpu_to_node(vma->vm_mm->owner->cpu) != page_to_nid(page)) {
	// 	// todo: 提前判断page类型，此处只做修改pte
	// 	nr_pte_updates = change_prot_numa(vma, addr, addr+1);
    // } else {
    //     printk("%s: didn't change_prot_numa\n", __func__);
    // }

	test_debug_lock();
	struct page_vma_mapped_walk pvmw = { //存放获取pte的相关信息，找到对应的pte之后保存在pvmw.pte中返回
        .page = page,
        .vma = vma,
        .address = addr,
    };

	// while (page_vma_mapped_walk(&pvmw)) {
		if (cpu_to_node(vma->vm_mm->owner->on_cpu) != page_to_nid(page)){
			if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
				is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
				goto out;
			}

			if (!vma->vm_mm ||
					(vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
					goto out;

			if (!vma_is_accessible(vma))
					goto out;

			// todo: 提前判断page类型，此处只做修改pte

			mmap_read_lock(vma->vm_mm);
			nr_pte_updates = my_change_prot_numa(vma, addr);
			mmap_read_unlock(vma->vm_mm);
			// nr_pte_updates = change_prot_numa(vma, addr,
								// addr+1);
		}
		update_mmu_cache(vma, pvmw.address, pvmw.pte);
	// }

out:
	test_debug_unlock();
	return true;
}

// 对应remove_migration_ptes()，但是功能修改为将其他node上的map修改为numa pgf的状态
// set_page_numa_dup_rmap
void unmap_dup_page(struct page *old_page, struct page *new_page,
				    bool locked)
{

	struct rmap_walk_control rwc = {
		.rmap_one = do_unmap_dup_page,
		.arg = old_page,
	};

	if (locked)
		rmap_walk_locked(new_page, &rwc);
	else
		rmap_walk(new_page, &rwc);
}

// 将page的内容和状态复制到新页
// copy_page_by_numa_duplication
static void copy_dup_page(struct page *newpage,
					 struct page *page,
					 enum migrate_mode mode)
{
	struct address_space *mapping;
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);
	migrate_page_copy(newpage, page);
	// my_migrate_page_copy(newpage, page);
}

// 对应__unmap_and_move()，再加上在node中添加副本元数据
// do_page_numa_duplication
static int do_dup_one_page(struct page *old_page, struct page *new_page,
				    int force, enum migrate_mode mode)
{

	int rc = MIGRATEPAGE_SUCCESS;
	bool page_was_mapped = false;
    struct dup_info *di = NULL;
	struct anon_vma *anon_vma = NULL;
	struct pglist_data *pgdat = page_pgdat(old_page);

    // old_page lock
	if (!trylock_page(old_page)) {
		if (!force || mode == MIGRATE_ASYNC)
			goto failed_lock_old_page;
		lock_page(old_page);
	}

	if (PageWriteback(old_page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		switch (mode) {
		case MIGRATE_SYNC:
		case MIGRATE_SYNC_NO_COPY:
			break;
		default:
			rc = -EBUSY;
			goto out_invalid_page;
		}
		if (!force) {
			rc = -EBUSY;
			goto out_invalid_page;
		}
		wait_on_page_writeback(old_page);
	}

    if (page_mapped(old_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(old_page) && !PageKsm(old_page) && !anon_vma,
			       old_page);
		page_was_mapped = true;
	}
	
    // new_page lock
    if (unlikely(!trylock_page(new_page))) {
		rc = -EAGAIN;
		goto failed_lock_new_page;
	}

    // 设置副本信息
	// printk("init_dup_info\n");
    di = init_dup_info(old_page, new_page);
	// printk("get_page\n");
    get_page(di->old_page);
	// printk("crc_init_in_numa_dup\n");
    // todo: crc校验
    // crc_init_in_numa_dup(di);

	if (!di) {
		printk(KERN_ALERT "can't alloc duplication info\n");
		rc = -EAGAIN;
		goto failed_init_dup_info;
	}

    // 将副本信息加到list中
	// printk("add_page_to_dup_list\n");
    int success = add_page_to_dup_list(old_page, 1);
	if (!success) {
		printk(KERN_ALERT "can't add old_page to dup list\n");
		rc = -EAGAIN;
		goto failed_add_dup_info;
	}

    // 拷贝页面
	// printk("copy_dup_page\n");
    copy_dup_page(new_page, old_page, mode);
    // unmap远端进程的pte
	// printk("unmap_dup_page\n");
	unmap_dup_page(old_page, old_page, false);
	// printk("unmap_dup_page end\n");
    // 设置page flag
	SetPageDuplicate(old_page);
	get_page(new_page);

success:
    // todo 确认迁移成功时，是否需要 put_anon_vma(anon_vma);
	rc = MIGRATEPAGE_SUCCESS;
    unlock_page(new_page);
    unlock_page(old_page);
    return rc;

failed_add_dup_info:
    dup_info_free(di);
    di = NULL;

failed_init_dup_info:
    unlock_page(new_page);
    put_page(old_page);

failed_lock_new_page:
out_invalid_page:
    unlock_page(old_page);

failed_lock_old_page:
    return rc;
}

// 对应migrate_pages()和unmap_and_move()
// page_numa_duplication
int dup_one_page(struct page *page, int target_nid, int force,
			  enum migrate_mode mode)
{
	int rc = MIGRATEPAGE_SUCCESS;
	struct page *newpage = NULL;

	if (PageTransHuge(page) || PageHuge(page))
		return -ENOSYS;

	if (page_count(page) == 1) {
		/* page was freed from under us. So we are done. */
		ClearPageActive(page);
		ClearPageUnevictable(page);
		if (unlikely(__PageMovable(page))) {
			lock_page(page);
			if (!PageMovable(page))
				__ClearPageIsolated(page);
			unlock_page(page);
		}
		goto out;
	}
//    printk(KERN_ALERT "test my_try_to_migrate()\n");
//    my_try_to_migrate(page, 0);
//	printk(KERN_ALERT "----------------page dup start--------------\n");
//    printk(KERN_ALERT "zone_idx:%d\n",page_zonenum(page));

	newpage = alloc_migration_page(page, target_nid);
	if (!newpage)
		return -ENOMEM;

	rc = do_dup_one_page(page, newpage, force, mode);
//	printk(KERN_ALERT "----------------page dup finished,rc = %d--------------\n",rc);
	if (rc != MIGRATEPAGE_SUCCESS) {
		printk(KERN_ALERT "----------------page dup finished,failed, rc = %d--------------\n",rc);
		put_page(newpage);
	}
out:
	return rc;
}

static int dup_page_list(struct list_head *from, unsigned long target_nid,
              enum migrate_mode mode, int reason, unsigned int *ret_succeeded)
{
    int pass = 0;
    int rc;
    int nr_failed = 0;
    struct page *page;
    struct page *page2;
    LIST_HEAD(ret_pages);

    list_for_each_entry_safe(page, page2, from, lru) {
        rc = dup_one_page(page, target_nid, pass > 2, mode);
        switch (rc) {
            case -ENOSYS:
                printk(KERN_ALERT "thp page duplication failed\n");
                nr_failed++;
                break;
            case -ENOMEM:
                printk(KERN_ALERT "alloc new page failed in numa dup\n");
                nr_failed++;
                break;
            case MIGRATEPAGE_SUCCESS:
                ret_succeeded++;
//                printk(KERN_ALERT "handle_dup_page_fault\n");
//                handle_dup_page_fault(page,NULL);
                break;
            default:
                break;
        }
    }
    return nr_failed;
}

static int fill_dup_candidate_pages(struct list_head *input_list, struct list_head *candidate_pages_list)
{
	struct migration_info *info1, *info2;
    int fill_cnt = 0;

	if (list_empty(input_list)) {
		printk("input_list.size=%d\n", 0);
		return 0;
	}

	list_for_each_entry_safe(info1, info2, input_list, list) {
#ifdef PAGE_MIGRATE_DEBUG
		printk(KERN_ALERT
		       "----------------dup page list:page:%lu, nid:%d, mode:%d--------------\n",
		       (unsigned long)info1->page, target_nid, mode);
#endif
        if (isolate_lru_page(info1->page) != 0) {
            printk(KERN_ALERT "isolate page failed\n");
            continue;
        }
        list_add(&info1->page->lru, candidate_pages_list);
        list_del(&info1->list);
        migrate_info_free(info1);
        fill_cnt++;
    }

	printk("fill_cnt=%d\n", fill_cnt);
	return fill_cnt;
}

// dup_page_list_by_info
int predict_pages_by_dup(struct list_head *input_list,
                          int target_nid, enum migrate_mode mode,
                          enum migrate_reason reason)
{
	struct pglist_data *pgdat = NODE_DATA(target_nid);
	int nr_failed;
	unsigned int nr_succeeded;
	unsigned int nr_input_info;
	LIST_HEAD(candidate_pages_list);
    
	if (target_nid == NUMA_NO_NODE)
		return 0;

	pgdat = NODE_DATA(target_nid);
	nr_input_info = fill_dup_candidate_pages(input_list, &candidate_pages_list);
	if (nr_input_info == 0) {
		return 0;
	}

	nr_succeeded = 0;
	nr_failed = dup_page_list(&candidate_pages_list, target_nid, mode, MIGRATE_SYNC, &nr_succeeded);
    if (nr_failed > 0) {
        printk(KERN_ALERT "dup %d pages error\n", nr_failed);
    }

    __count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
    pgdat->nr_dup_success += nr_succeeded;
    pgdat->nr_dup_fail += nr_input_info - nr_succeeded;
    if (nr_succeeded == nr_input_info)
        return nr_input_info;
    else
        return nr_succeeded - nr_input_info;
}


// 对应move_to_new_page(),但是不进行数据页内容的迁移，只更改mapping
// mv_mapping_to_new_page
static int mv_dup_page_mapping_to_local(struct page *new_page, struct page *old_page,
				     enum migrate_mode mode)
{
	struct address_space *mapping;
	int rc = -EAGAIN;
	bool is_lru = !__PageMovable(old_page);

	VM_BUG_ON_PAGE(!PageLocked(old_page), old_page);
	VM_BUG_ON_PAGE(!PageLocked(new_page), new_page);

	mapping = page_mapping(old_page);

	if (likely(is_lru)) {
		if (PageAnon(old_page))
			rc = migrate_page_mapping(mapping, new_page, old_page, mode);
		else
			rc = migrate_file_page_mapping(mapping, new_page, old_page,
						       mode);
	}
	//	else {
	//		/*
	//		 * In case of non-lru page, it could be released after
	//		 * isolation step. In that case, we shouldn't try migration.
	//		 */
	//		VM_BUG_ON_PAGE(!PageIsolated(old_page), old_page);
	//		if (!PageMovable(old_page)) {
	//			rc = MIGRATEPAGE_SUCCESS;
	//			__ClearPageIsolated(old_page);
	//			goto out;
	//		}
	//
	//		rc = mapping->a_ops->migratepage(mapping, new_page,
	//						 old_page, mode);
	//		WARN_ON_ONCE(rc == MIGRATEPAGE_SUCCESS &&
	//			     !PageIsolated(old_page));
	//	}

	/*
	 * When successful, old pagecache page->mapping must be cleared before
	 * page is freed; but stats require that PageAnon be left as PageAnon.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		if (__PageMovable(old_page)) {
			VM_BUG_ON_PAGE(!PageIsolated(old_page), old_page);

			/*
			 * We clear PG_movable under page_lock so any compactor
			 * cannot try to migrate this page.
			 */
			__ClearPageIsolated(old_page);
		}

		/*
		 * Anonymous and movable page->mapping will be cleared by
		 * free_pages_prepare so don't reset it here for keeping
		 * the type to work PageAnon, for example.
		 */
		if (!PageMappingFlags(old_page))
			old_page->mapping = NULL;

		if (likely(!is_zone_device_page(new_page))) {
			int i, nr = compound_nr(new_page);

			for (i = 0; i < nr; i++)
				flush_dcache_page(new_page + i);
		}
	}
	
out:
	return rc;
}

// 本地对副本发生访问时，将修改副本状态
// do_mv_page_in_numa_dup
static bool do_handle_dup_page_fault(struct dup_info *di)
{
	struct page *old_page, *new_page;
	bool locked = false;
	struct rmap_walk_control rwc = {
		.rmap_one = my_remove_migration_pte, // migrate.c中已有的函数
		.arg = old_page,
	};

	old_page = di->old_page;
	new_page = di->dup_page;

	if (!trylock_page(old_page)) {
		goto failed_lock_old_page;
	}

	if (!trylock_page(new_page)) {
		goto failed_lock_new_page;
	}

	try_to_migrate(old_page, 0); // rmap.c中已有的函数
	mv_dup_page_mapping_to_local(new_page, old_page, MIGRATE_SYNC);

	if (locked)
		rmap_walk_locked(new_page, &rwc);
	else
		rmap_walk(new_page, &rwc);

    ClearPageDuplicate(old_page);

success:
	unlock_page(new_page);

failed_lock_new_page:
	unlock_page(old_page);

failed_lock_old_page:
	return true;
}


// pgfault_handle_in_numa_dup
bool handle_dup_page_fault(struct page *page, struct vm_area_struct *vma, struct vm_fault *vmf)
{
	bool rc = false;
	bool locked = false;
	struct dup_info *di = NULL, *next_di;
	struct pglist_data *pgdat = page_pgdat(page);
    struct anon_vma *anon_vma = NULL;
	
    // page lock
    if (!trylock_page(page)) {
		goto failed_lock_page;
	}

    if (PageAnon(page) && !PageKsm(page))
		anon_vma = page_get_anon_vma(page);

	if (!spin_trylock_irq(&pgdat->duplist_lock)) {
		goto failed_lock_duplist;
	} else {
		// printk("%s: get duplist_lock\n", __func__);
	}

	// todo: check these codes
	list_for_each_entry_safe(di, next_di, &pgdat->dupinfo_list, list) {
		// 不同页，继续遍历
		if (page == di->old_page) {
            break;
        }
	}

	if (page != di->old_page) {
		goto fail_find_dup_info;
	}

	if (!crc_check_in_numa_dup(di)) {
		put_page(di->old_page);
		goto crc_error;
	}

	rc = do_handle_dup_page_fault(di);
	BUG_ON(!rc);
	if (!rc) {
		goto failed_handle_fault;
	}

success:
	mod_node_page_state(page_pgdat(page), NR_ISOLATED_ANON +
			page_is_file_lru(page), -1);
	list_del(&di->list);
	dup_info_free(di);

    spin_unlock_irq(&pgdat->duplist_lock);
	// printk("%s realease duplist_lock", __func__);
    page_hotness_migrate_success(page);
	return rc;

failed_handle_fault:
failed_lock_dup_page:
crc_error:
fail_find_dup_info:
    if (anon_vma)
		put_anon_vma(anon_vma);

failed_lock_page:
    spin_unlock_irq(&pgdat->duplist_lock);
	// printk("%s: release duplist_lock\n", __func__);

failed_lock_duplist:
	return rc;
}


// put_miss_page_back_in_numa_dup
static int put_miss_page_back_in_numa_dup(struct list_head *page_list, struct pglist_data *pgdat)
{
    int nr_moved = 0;
	int ret = -EBUSY;

    struct page *page, *next_page;
    // struct lruvec *lruvec = &pgdat->__lruvec;

	list_for_each_entry_safe(page, next_page, page_list, lru) {
		VM_BUG_ON_PAGE(!page_count(page), page);
		WARN_RATELIMIT(PageTail(page), "trying to isolate tail page");
		if (!trylock_page(page)) {
			printk(KERN_ALERT "----try lock page failed----\n");
			continue ;
		}

		ClearPageDuplicate(page);
		list_del(&page->lru);

		SetPageLRU(page);
		lru_cache_add(page);

		put_page(page);
		
//		if (TestClearPageLRU(page)) {
//			lruvec = lock_page_lruvec_irq(page);
//			add_page_to_lru_list(page, lruvec);
//			unlock_page_lruvec_irq(lruvec);
//			put_page(page);
////			printk(KERN_ALERT
////			       "put_miss_page_back: page:%lu page_refcount:%d\n",
////			       (unsigned long)page, page_count(page));
//			nr_moved += 1;
//		}
		unlock_page(page);
		nr_moved += 1;
	}
//	unlock_page_lruvec_irq(lruvec);

	return nr_moved;
}


int del_miss_dup_info(struct pglist_data *pgdat)
{
	struct dup_info *dup1, *dup2;
	struct page *page1, *page2;
	int list_len = 0;
    int page_len = 0;
    int info_len = 0;
	LIST_HEAD(tmp_page_list);

	test_debug_lock();
//	printk(KERN_ALERT
//	       "--------------del_miss_dup_info start---------------\n");
//	list_for_each_entry_safe (page1, page2, &pgdat->duppage_list, lru) {
//		printk(KERN_ALERT "------tmp_page_list:%lu\n", (unsigned long)page1);
//	}
	
	if (!spin_trylock_irq(&pgdat->duplist_lock)){
        printk(KERN_ALERT "get duplist_lock failed\n");
        return -EAGAIN;
    }

    // printk(KERN_ALERT "%s: get duplist_lock of pgdat%d\n", __func__, pgdat->node_id);
    // if (unlikely(list_empty(&pgdat->dupinfo_list) )) {
    if (unlikely(list_empty(&pgdat->dupinfo_list) && list_empty(&pgdat->duppage_list))) {
		// BUG_ON(!list_empty(&pgdat->duppage_list));
        goto unlock;
    }

    list_for_each_entry_safe (page1, page2, &pgdat->duppage_list, lru) {
        //printk(KERN_ALERT "------page list:%lu\n", (unsigned long)page1);
        page_len += 1;
    }
    list_for_each_entry_safe (dup1, dup2, &pgdat->dupinfo_list, list) {
        //printk(KERN_ALERT "------dupinfo list:%lu\n", (unsigned long)dup1->old_page);
        info_len += 1;
    }
	// printk(KERN_ALERT "page_len:%d,info_len:%d\n",page_len, info_len);

	list_for_each_entry_safe (dup1, dup2, &pgdat->dupinfo_list, list) {
		list_len += 1;

		// todo: dup info中的dup_page，与duppage_list中的page的区别
		// 这里put_page了一次，put_miss_page_back_in_numa_dup也put_back了一次
		// 所以duppage_list放的是原页面吧
		put_page(dup1->dup_page);
		
		list_del(&dup1->list);
		dup_info_free(dup1);
	}

    list_splice(&tmp_page_list, &pgdat->duppage_list);

    put_miss_page_back_in_numa_dup(&tmp_page_list, pgdat);
    
	INIT_LIST_HEAD(&pgdat->duppage_list);

	if (likely(list_empty(&pgdat->dupinfo_list) &&
		     list_empty(&pgdat->duppage_list))) {
		printk(KERN_ALERT "--------------del_miss_dup_info success---------------\n");
		goto unlock;
	}

    printk(KERN_ALERT "dupinfo_list empty:%d, duppage_list empty:%d\n",
		list_empty(&pgdat->dupinfo_list),
		list_empty(&pgdat->duppage_list));
    printk(KERN_ALERT "delete dup info failed in node%d\n", pgdat->node_id);

unlock:
	spin_unlock_irq(&pgdat->duplist_lock);
	// printk(KERN_ALERT "%s: release duplist_lock of pgdat%d\n", __func__, pgdat->node_id);
	test_debug_unlock();
	return list_len;
}