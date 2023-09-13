#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/ksm.h>
#include <linux/uaccess.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

#include "internal.h"

#include <linux/page_balancing.h>
#include <linux/page_change_prot.h>

static void my_print_bad_pte(struct vm_area_struct *vma, unsigned long addr,
			  pte_t pte, struct page *page)
{
	pgd_t *pgd = pgd_offset(vma->vm_mm, addr);
	p4d_t *p4d = p4d_offset(pgd, addr);
	pud_t *pud = pud_offset(p4d, addr);
	pmd_t *pmd = pmd_offset(pud, addr);
	struct address_space *mapping;
	pgoff_t index;
	static unsigned long resume;
	static unsigned long nr_shown;
	static unsigned long nr_unshown;

	/*
	 * Allow a burst of 60 reports, then keep quiet for that minute;
	 * or allow a steady drip of one report per second.
	 */
	if (nr_shown == 60) {
		if (time_before(jiffies, resume)) {
			nr_unshown++;
			return;
		}
		if (nr_unshown) {
			pr_alert("BUG: Bad page map: %lu messages suppressed\n",
				 nr_unshown);
			nr_unshown = 0;
		}
		nr_shown = 0;
	}
	if (nr_shown++ == 0)
		resume = jiffies + 60 * HZ;

	mapping = vma->vm_file ? vma->vm_file->f_mapping : NULL;
	index = linear_page_index(vma, addr);

	pr_alert("BUG: Bad page map in process %s  pte:%08llx pmd:%08llx\n",
		 current->comm,
		 (long long)pte_val(pte), (long long)pmd_val(*pmd));
	if (page)
		dump_page(page, "bad pte");
	pr_alert("addr:%px vm_flags:%08lx anon_vma:%px mapping:%px index:%lx\n",
		 (void *)addr, vma->vm_flags, vma->anon_vma, mapping, index);
	pr_alert("file:%pD fault:%ps mmap:%ps readpage:%ps\n",
		 vma->vm_file,
		 vma->vm_ops ? vma->vm_ops->fault : NULL,
		 vma->vm_file ? vma->vm_file->f_op->mmap : NULL,
		 mapping ? mapping->a_ops->readpage : NULL);
	dump_stack();
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
}

struct page *my_vm_normal_page(struct vm_area_struct *vma, unsigned long addr,
			    pte_t pte)
{
	unsigned long pfn = pte_pfn(pte);

	if (IS_ENABLED(CONFIG_ARCH_HAS_PTE_SPECIAL)) {
		if (likely(!pte_special(pte))) {
			goto check_pfn;
		}
		if (vma->vm_ops && vma->vm_ops->find_special_page) {
			return vma->vm_ops->find_special_page(vma, addr);
		}
		if (vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)) {
			return NULL;
		}
		if (is_zero_pfn(pfn)) {
			return NULL;
		}
		if (pte_devmap(pte)) {
			return NULL;
		}

		my_print_bad_pte(vma, addr, pte, NULL);
		BUG_ON(1);
		return NULL;
	}

	/* !CONFIG_ARCH_HAS_PTE_SPECIAL case follows: */

	if (unlikely(vma->vm_flags & (VM_PFNMAP|VM_MIXEDMAP))) {
		if (vma->vm_flags & VM_MIXEDMAP) {
			if (!pfn_valid(pfn))
				return NULL;
			goto out;
		} else {
			unsigned long off;
			off = (addr - vma->vm_start) >> PAGE_SHIFT;
			if (pfn == vma->vm_pgoff + off)
				return NULL;
			if (!is_cow_mapping(vma->vm_flags))
				return NULL;
		}
	}

	if (is_zero_pfn(pfn)) {
		return NULL;
	}

check_pfn:
	if (unlikely(pfn > highest_memmap_pfn)) {
		printk("highest_memmap_pfn=0x%lx\n", highest_memmap_pfn);
		my_print_bad_pte(vma, addr, pte, NULL);
		BUG_ON(1);
		return NULL;
	}

	/*
	 * NOTE! We still have PageReserved() pages in the page tables.
	 * eg. VDSO mappings can cause them to exist.
	 */
out:
	return pfn_to_page(pfn);
}


static unsigned long my_change_pte(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long addr, pgprot_t newprot,
		unsigned long cp_flags)
{
	pte_t *pte, oldpte;
	spinlock_t *ptl;
	unsigned long pages = 0;
	int target_node = NUMA_NO_NODE;
	bool dirty_accountable = cp_flags & MM_CP_DIRTY_ACCT;
	bool prot_numa = cp_flags & MM_CP_PROT_NUMA;
	bool uffd_wp = cp_flags & MM_CP_UFFD_WP;
	bool uffd_wp_resolve = cp_flags & MM_CP_UFFD_WP_RESOLVE;

#ifdef CONFIG_PAGE_CHANGE_PROT_DEBUG
	printk("my_change_pte: addr=0x%lx\n", addr);
#endif
	++pte_stat.pte_all_page;

	/*
	 * Can be called with only the mmap_lock for reading by
	 * prot_numa so we must check the pmd isn't constantly
	 * changing from under us from pmd_none to pmd_trans_huge
	 * and/or the other way around.
	 */
	if (pmd_trans_unstable(pmd)) {
		++pte_stat.nr_pmd_trans_unstable;
		return 0;
	}

	/*
	 * The pmd points to a regular pte so the pmd can't change
	 * from under us even if the mmap_lock is only hold for
	 * reading.
	 */
	// todo: solve deadlock
	// printk("%s: ready to get ptl lock\n", __func__);
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	// printk("%s: get ptl lock\n", __func__);

	/* Get target node for single threaded private VMAs */
	if (prot_numa && !(vma->vm_flags & VM_SHARED) &&
	    atomic_read(&vma->vm_mm->mm_users) == 1)
		target_node = numa_node_id();

	flush_tlb_batched_pending(vma->vm_mm);
	arch_enter_lazy_mmu_mode();

	oldpte = *pte;
	if (pte_present(oldpte)) {
		pte_t ptent;
		bool preserve_write = prot_numa && pte_write(oldpte);
		++pte_stat.nr_pte_present;

		/*
		* Avoid trapping faults against the zero or KSM
		* pages. See similar comment in change_huge_pmd.
		*/
		if (prot_numa) {
			struct page *page;
			// lmy
			int hotness_counter;

			page = my_vm_normal_page(vma, addr, oldpte);
			if (!page || PageKsm(page)) {
				++pte_stat.nr_ksm_page;
				goto out;
			}

			/* Also skip shared copy-on-write pages */
			if (is_cow_mapping(vma->vm_flags) &&
				page_count(page) != 1) {
				++pte_stat.nr_cow_mapping;
				goto out;
			}

			/*
				* While migration can move some dirty pages,
				* it cannot move them all from MIGRATE_ASYNC
				* context.
				*/
			if (page_is_file_lru(page) && PageDirty(page)) {
				++pte_stat.nr_file_page;
				goto out;
			}
			
			/* Avoid TLB flush if possible */
			int cpu_id = task_cpu(current);
			BUG_ON(!page);
			if (pte_protnone(oldpte)) {
				++pte_stat.nr_pte_protnone;

				// lmy
				hotness_counter = mod_page_access_counter(page, 0, cpu_id);
				add_page_for_tracking(page, 0, cpu_id);

				goto out;
			}
			// lmy
			hotness_counter = mod_page_access_counter(page, 1, cpu_id);
			add_page_for_tracking(page, 1, cpu_id);
			
			/*
			* Don't mess with PTEs if page is already on the node
			* a single-threaded process is running on.
			*/
			if (target_node == page_to_nid(page)) {
				++pte_stat.nr_target_equal_page;
				goto debug;
			}
		}

debug:
		oldpte = ptep_modify_prot_start(vma, addr, pte);
		ptent = pte_modify(oldpte, newprot);
		if (preserve_write)
			ptent = pte_mk_savedwrite(ptent);

		if (uffd_wp) {
			ptent = pte_wrprotect(ptent);
			ptent = pte_mkuffd_wp(ptent);
		} else if (uffd_wp_resolve) {
			/*
				* Leave the write bit to be handled
				* by PF interrupt handler, then
				* things like COW could be properly
				* handled.
				*/
			ptent = pte_clear_uffd_wp(ptent);
		}

		/* Avoid taking write faults for known dirty pages */
		if (dirty_accountable && pte_dirty(ptent) &&
				(pte_soft_dirty(ptent) ||
					!(vma->vm_flags & VM_SOFTDIRTY))) {
			ptent = pte_mkwrite(ptent);
		}
		ptep_modify_prot_commit(vma, addr, pte, oldpte, ptent);
		pages++;
	} else if (is_swap_pte(oldpte)) {
		swp_entry_t entry = pte_to_swp_entry(oldpte);
		pte_t newpte;
		++pte_stat.nr_swap_pte;		

		if (is_writable_migration_entry(entry)) {
			/*
				* A protection check is difficult so
				* just be safe and disable write
				*/
			entry = make_readable_migration_entry(
						swp_offset(entry));
			newpte = swp_entry_to_pte(entry);
			if (pte_swp_soft_dirty(oldpte))
				newpte = pte_swp_mksoft_dirty(newpte);
			if (pte_swp_uffd_wp(oldpte))
				newpte = pte_swp_mkuffd_wp(newpte);
		} else if (is_writable_device_private_entry(entry)) {
			/*
				* We do not preserve soft-dirtiness. See
				* copy_one_pte() for explanation.
				*/
			entry = make_readable_device_private_entry(
						swp_offset(entry));
			newpte = swp_entry_to_pte(entry);
			if (pte_swp_uffd_wp(oldpte))
				newpte = pte_swp_mkuffd_wp(newpte);
		} else if (is_writable_device_exclusive_entry(entry)) {
			entry = make_readable_device_exclusive_entry(
						swp_offset(entry));
			newpte = swp_entry_to_pte(entry);
			if (pte_swp_soft_dirty(oldpte))
				newpte = pte_swp_mksoft_dirty(newpte);
			if (pte_swp_uffd_wp(oldpte))
				newpte = pte_swp_mkuffd_wp(newpte);
		} else {
			newpte = oldpte;
		}

		if (uffd_wp)
			newpte = pte_swp_mkuffd_wp(newpte);
		else if (uffd_wp_resolve)
			newpte = pte_swp_clear_uffd_wp(newpte);

		if (!pte_same(oldpte, newpte)) {
			set_pte_at(vma->vm_mm, addr, pte, newpte);
			pages++;
		}
	}

out:
	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(pte - 1, ptl);


	return pages;
}


/*
 * Used when setting automatic NUMA hinting protection where it is
 * critical that a numa hinting PMD is not confused with a bad PMD.
 */
static int my_pmd_none_or_clear_bad_unless_trans_huge(pmd_t *pmd)
{
	pmd_t pmdval = pmd_read_atomic(pmd);

	/* See pmd_none_or_trans_huge_or_clear_bad for info on barrier */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	barrier();
#endif

	if (pmd_none(pmdval))
		return 1;
	if (pmd_trans_huge(pmdval))
		return 0;
	if (unlikely(pmd_bad(pmdval))) {
		pmd_clear_bad(pmd);
		return 1;
	}

	return 0;
}

static unsigned long my_change_pmd(struct vm_area_struct *vma,
		pud_t *pud, unsigned long addr,
		pgprot_t newprot, unsigned long cp_flags)
{
	pmd_t *pmd;
	unsigned long pages = 0;
	unsigned long nr_huge_updates = 0;
	struct mmu_notifier_range range;
    unsigned long end;

	range.start = 0;
	pmd = pmd_offset(pud, addr);
	end = pmd_addr_end(addr, addr + PAGE_SIZE);

	/*
	* Automatic NUMA balancing walks the tables with mmap_lock
	* held for read. It's possible a parallel update to occur
	* between pmd_trans_huge() and a pmd_none_or_clear_bad()
	* check leading to a false positive and clearing.
	* Hence, it's necessary to atomically read the PMD value
	* for all the checks.
	*/
	if (!is_swap_pmd(*pmd) && !pmd_devmap(*pmd) &&
			my_pmd_none_or_clear_bad_unless_trans_huge(pmd))
		goto out;

	/* invoke the mmu notifier if the pmd is populated */
	if (!range.start) {
		mmu_notifier_range_init(&range,
			MMU_NOTIFY_PROTECTION_VMA, 0,
			vma, vma->vm_mm, addr, end);
		mmu_notifier_invalidate_range_start(&range);
	}

	if (is_swap_pmd(*pmd) || pmd_trans_huge(*pmd) || pmd_devmap(*pmd)) {
		return pages;
		// if (next - addr != HPAGE_PMD_SIZE) {
		// 	__split_huge_pmd(vma, pmd, addr, false, NULL);
		// } else {
			// int nr_ptes = change_huge_pmd(vma, pmd, addr,
			// 					newprot, cp_flags);

			// if (nr_ptes) {
			// 	if (nr_ptes == HPAGE_PMD_NR) {
			// 		pages += HPAGE_PMD_NR;
			// 		nr_huge_updates++;
			// 	}

			// 	/* huge pmd was handled */
			// 	goto next;
			// }
		// }
		/* fall through, the trans huge pmd just split */
	}
	pages = my_change_pte(vma, pmd, addr, newprot,
						cp_flags);
	pages += pages;

out:
	if (range.start)
		mmu_notifier_invalidate_range_end(&range);

	if (nr_huge_updates)
		count_vm_numa_events(NUMA_HUGE_PTE_UPDATES, nr_huge_updates);
	return pages;
}

static unsigned long my_change_pud(struct vm_area_struct *vma,
		p4d_t *p4d, unsigned long addr,
		pgprot_t newprot, unsigned long cp_flags)
{
	pud_t *pud;
	unsigned long pages = 0;

	pud = pud_offset(p4d, pud_addr_end(addr, addr + PAGE_SIZE));
	if (pud_none_or_clear_bad(pud))
		goto out;

	pages += my_change_pmd(vma, pud, addr, newprot,
					cp_flags);
out:
	return pages;
}

static unsigned long my_change_p4d(struct vm_area_struct *vma,
		pgd_t *pgd, unsigned long addr,
		pgprot_t newprot, unsigned long cp_flags)
{
	p4d_t *p4d;
	unsigned long pages = 0;

	p4d = p4d_offset(pgd, p4d_addr_end(addr, addr + PAGE_SIZE));
	if (p4d_none_or_clear_bad(p4d))
		goto out;

	pages += my_change_pud(vma, p4d, addr, newprot,
					cp_flags);

out:
	return pages;
}


static unsigned long __my_change_protection(struct vm_area_struct *vma,
		unsigned long addr, pgprot_t newprot,
		unsigned long cp_flags)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long pages = 0;
	pgd = pgd_offset(mm, addr);
	flush_cache_range(vma, addr, pgd_addr_end(addr, addr + PAGE_SIZE));
	inc_tlb_flush_pending(mm);

	if (pgd_none_or_clear_bad(pgd))
		goto out;

	pages += my_change_p4d(vma, pgd, addr, newprot,
					cp_flags);

out:
	/* Only flush the TLB if we actually modified any entries: */
	if (pages)
		flush_tlb_range(vma, addr, pgd_addr_end(addr, addr + PAGE_SIZE));
	dec_tlb_flush_pending(mm);

	return pages;
}

static unsigned long my_change_protection(struct vm_area_struct *vma, unsigned long addr,
		       pgprot_t newprot, unsigned long cp_flags)
{
	unsigned long pages;

	BUG_ON((cp_flags & MM_CP_UFFD_WP_ALL) == MM_CP_UFFD_WP_ALL);

	if (is_vm_hugetlb_page(vma)) {
		BUG_ON("unimplemented\n");
	}
	else {
		pages = __my_change_protection(vma, addr, newprot,
						cp_flags);
	}

	return pages;
}

unsigned long my_change_prot_numa(struct vm_area_struct *vma,
			unsigned long addr)
{
	int nr_updated;

	nr_updated = my_change_protection(vma, addr, PAGE_NONE, MM_CP_PROT_NUMA);

	return nr_updated;
}

struct pte_stat pte_stat = {0};
void dump_pte_stat(void)
{
	printk("pte_stat.pte_all_page=%d\n", pte_stat.pte_all_page);
	printk("pte_stat.pte_success_change=%d\n", pte_stat.pte_success_change);
	printk("pte_stat.nr_pmd_trans_unstable=%d\n", pte_stat.nr_pmd_trans_unstable);
	printk("pte_stat.nr_pte_present=%d\n", pte_stat.nr_pte_present);
	printk("pte_stat.nr_swap_pte=%d\n", pte_stat.nr_swap_pte);
	printk("pte_stat.nr_ksm_page=%d\n", pte_stat.nr_ksm_page);
	printk("pte_stat.nr_cow_mapping=%d\n", pte_stat.nr_cow_mapping);
	printk("pte_stat.nr_file_page=%d\n", pte_stat.nr_file_page);
	printk("pte_stat.nr_pte_protnone=%d\n", pte_stat.nr_pte_protnone);
	printk("pte_stat.nr_target_equal_page=%d\n", pte_stat.nr_target_equal_page);
}


// invalid return true
// valid return false
static bool my_invalid_vma(struct vm_area_struct *vma, void *arg)
{
#ifdef CONFIG_PAGE_CHANGE_PROT_DEBUG
	struct page *arg_page = (struct page*)arg;
	printk("my_invalid_vma: arg_page=0x%lx\n", arg_page);
#endif

	// check vma (the code is copied from task_numa_work)
	if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
		is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
		goto out_invalid;
	}
    
	if (!vma->vm_mm ||
		(vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
		goto out_invalid;

	if (!vma_is_accessible(vma))
		goto out_invalid;

out_valid:
	vma_stat.nr_valid_vma ++;
	return false;

out_invalid:
	vma_stat.nr_invalid_vma ++;
	vma_stat.nr_vma_not_migratable += !vma_migratable(vma);
	vma_stat.nr_vma_policy_null += !vma_policy_mof(vma);
	vma_stat.nr_vma_hugetlb_page += !is_vm_hugetlb_page(vma);
	vma_stat.nr_vma_MIXEDMAP += (vma->vm_flags & VM_MIXEDMAP);

	vma_stat.nr_vma_vm_mm_null += !vma->vm_mm;
	vma_stat.nr_vma_file_readonly += (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ));
	vma_stat.nr_vma_not_accessible += !vma_is_accessible(vma);

	return true;
}

static bool should_change_prot(struct page *page)
{
	prot_stat.nr_all_page ++;
	if (page_zonenum(page) == ZONE_DMA)
	{
		prot_stat.nr_dma_zone_page ++;
		goto skip;
	}

	if (PageCompound(page) || PageHead(page) || PageTail(page)) {
		prot_stat.nr_compound_page ++;
		goto skip;
	}

	if (!page || PageKsm(page)) {
		prot_stat.nr_ksm_page ++;
		goto skip;
	}

	// 理论上不需要判断文件
	// if (page_is_file_lru(page) && PageDirty(page)) {
	// 	goto skip;
	// }

	// change_pte_range本身在prot_numa情况下,需要跳过一些paga，这里提前过滤

	// if (fault_in_kernel_space(page_address(page))) {
	// 	prot_stat.nr_kernel_page++;
	// 	res = false;
	// 	// return false;
	// }

	// if (!access_ok(page, PAGE_SIZE)) {
	// 	prot_stat.nr_access_not_ok ++;
	// 	return false;
	// }


	// if (PageReported(page)) {
	// 	return false;
	// }

	// 是否要过滤掉内核页
	// if (page_to_pfn())
out:
	return true;

skip:
	return false;
}

static bool change_one_page_prot(struct page *page, struct vm_area_struct *vma,
					unsigned long addr, void *arg)
{
	unsigned long nr_pte_updates = 0;
	struct page *arg_page = (struct page*)arg;
	BUG_ON(page != arg_page);

#ifdef CONFIG_PAGE_CHANGE_PROT_DEBUG
	printk("change_one_page_prot: arg_page=%px\n", arg_page);
#endif

	if (fault_in_kernel_space(addr)) {
		++vma_stat.nr_addr_in_kernel;
		// return nr_pte_updates;
	}

	// 根据注释，正常prot_numa情况下拥有mmap_read_lock锁
	mmap_read_lock(vma->vm_mm);
	// nr_pte_updates = change_prot_numa(vma, start, end);
	nr_pte_updates = my_change_prot_numa(vma, addr);
	mmap_read_unlock(vma->vm_mm);

	prot_stat.nr_success_change += nr_pte_updates;

	return nr_pte_updates;
}

void change_active_list_prot(void)
{
	int nid;
	struct rmap_walk_control rwc = {
		.rmap_one = change_one_page_prot,
		.arg = NULL,
		.anon_lock = NULL,
		.done = NULL,
		.invalid_vma = my_invalid_vma,
	};
	
//	dump_unmap_page_stat();
//	dump_rmap_stat();
//	dump_vma_stat();
//	dump_pte_stat();
//	dump_predict_queue_stat();
	
	// printk("start to change_active_list_prot\n");
	for_each_node(nid) {
		int i = 0;
		int len = 0;
		struct lruvec *lruvec;
		struct page* page;
		struct page* pages[MY_UNMAP_PAGE_NUM];
		struct pglist_data *pgdat = NODE_DATA(nid);

		if (!pgdat) {
			// printk("nid=%d, pgdat is null\n", nid);
			continue;
		} else {
			// printk("nid=%d, pgdat=%p\n", nid, pgdat);
		}

		if (mem_cgroup_disabled()) {
			lruvec = &pgdat->__lruvec;
		} else {
			// printk("mem_cgroup is enabled\n");
			lruvec = mem_cgroup_lruvec(NULL, pgdat);
		}

		lru_add_drain();
		spin_lock(&lruvec->lru_lock);
		list_for_each_entry(page, &lruvec->lists[LRU_ACTIVE_ANON], lru) {
		// list_for_each_entry(page, &lruvec->lists[LRU_INACTIVE_ANON], lru) {
			if (len == MY_UNMAP_PAGE_NUM) {
				break;
			}
			if (!should_change_prot(page)) {
				continue;
			}

			// printk("len=%d, page=%p\n", len, page);
			pages[len++] = page;
		}
		// 解锁暂时往后面移一下
		// spin_unlock(&lruvec->lru_lock);

		for (i = 0; i < len; i++) {
			// printk("change_page_prot page=%p", pages[i]);
            BUG_ON(PageUnevictable(pages[i]));
			rwc.arg = pages[i];
			rmap_walk(pages[i], &rwc);
			rwc.arg = NULL;
		}
		spin_unlock(&lruvec->lru_lock);
	}
}

struct prot_stat prot_stat = {0};
void dump_unmap_page_stat(void)
{
	printk("prot_stat:\n");
	printk("prot_stat.nr_all_page=%d\n", prot_stat.nr_all_page);
	printk("prot_stat.nr_success_change=%d\n", prot_stat.nr_success_change);
	printk("prot_stat.nr_dma_zone_page=%d\n", prot_stat.nr_dma_zone_page);
	printk("prot_stat.nr_compound_page=%d\n", prot_stat.nr_compound_page);
	printk("prot_stat.nr_ksm_page=%d\n", prot_stat.nr_ksm_page);
	printk("\n");
}

struct vma_stat vma_stat = {0};
void dump_vma_stat(void) {
	printk("vma:\n");
	printk("vma_stat.nr_valid_vma=%d\n", vma_stat.nr_valid_vma);
	printk("vma_stat.nr_invalid_vma=%d\n", vma_stat.nr_invalid_vma);
	printk("vma_stat.nr_vma_not_migratable=%d\n", vma_stat.nr_vma_not_migratable);
	printk("vma_stat.nr_vma_policy_null=%d\n", vma_stat.nr_vma_policy_null);
	printk("vma_stat.nr_vma_hugetlb_page=%d\n", vma_stat.nr_vma_hugetlb_page);
	printk("vma_stat.nr_vma_MIXEDMAP=%d\n", vma_stat.nr_vma_MIXEDMAP);
	printk("vma_stat.nr_vma_vm_mm_null=%d\n", vma_stat.nr_vma_vm_mm_null);
	printk("vma_stat.nr_vma_file_readonly=%d\n", vma_stat.nr_vma_file_readonly);
	printk("vma_stat.nr_vma_not_accessible=%d\n", vma_stat.nr_vma_not_accessible);
	printk("addr:\n");
	printk("vma_stat.nr_addr_in_kernel=%d\n", vma_stat.nr_addr_in_kernel);
	printk("\n");
}