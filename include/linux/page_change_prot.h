#ifndef __LINUX_PAGE_CHANGE_PROT_H
#define __LINUX_PAGE_CHANGE_PROT_H

#include <linux/rmap.h>
#include <asm/traps.h>

#define MY_UNMAP_PAGE_NUM 100

struct prot_stat {
    int nr_all_page;
    int nr_success_change;

	// 先检查page(将change pte中的检查提前)
    int nr_dma_zone_page;
    int nr_compound_page;
    int nr_ksm_page;
};
extern struct prot_stat prot_stat;

struct vma_stat {
	// 检查vma(autonuma中做的检查)
    int nr_valid_vma;
    int nr_invalid_vma;
	int nr_vma_not_migratable;
	int nr_vma_policy_null;
	int nr_vma_hugetlb_page;
	int nr_vma_MIXEDMAP;
	int nr_vma_vm_mm_null;
	int nr_vma_file_readonly;
	int nr_vma_not_accessible;

	// 检查vaddr
	int nr_addr_in_kernel;
};
extern struct vma_stat vma_stat;

struct pte_stat {
	int pte_all_page;
	int pte_success_change;

	int nr_pmd_trans_unstable;
	int nr_pte_present;
	int nr_swap_pte;
	int nr_ksm_page;
	int nr_cow_mapping;
	int nr_file_page;
	int nr_pte_protnone;
	int nr_target_equal_page;
};
extern struct pte_stat pte_stat;

extern void dump_unmap_page_stat(void);
extern void change_active_list_prot(void);
extern unsigned long my_change_prot_numa(struct vm_area_struct *vma,
			unsigned long addr);

extern void dump_vma_stat(void);
extern void dump_rmap_stat(void);
extern void rmap_walk(struct page *page, struct rmap_walk_control *rwc);

#endif