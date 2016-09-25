/*
 *  arch/arm/include/asm/pgtable-2level.h
 *
 *  Copyright (C) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ASM_PGTABLE_2LEVEL_H
#define _ASM_PGTABLE_2LEVEL_H

#define __PAGETABLE_PMD_FOLDED

/*
 * Hardware-wise, we have a two level page table structure, where the first
 * level has 4096 entries, and the second level has 256 entries.  Each entry
 * is one 32-bit word.  Most of the bits in the second level entry are used
 * by hardware, and there aren't any "accessed" and "dirty" bits.
 *
 * Linux on the other hand has a three level page table structure, which can
 * be wrapped to fit a two level page table structure easily - using the PGD
 * and PTE only.  However, Linux also expects one "PTE" table per page, and
 * at least a "dirty" bit.
 *
 * Therefore, we tweak the implementation slightly - we tell Linux that we
 * have 2048 entries in the first level, each of which is 8 bytes (iow, two
 * hardware pointers to the second level.)  The second level contains two
 * hardware PTE tables arranged contiguously, preceded by Linux versions
 * which contain the state information Linux needs.  We, therefore, end up
 * with 512 entries in the "PTE" level.
 *
 * This leads to the page tables having the following layout:
 *
 *    pgd             pte
 * |        |
 * +--------+
 * |        |       +------------+ +0
 * +- - - - +       | Linux pt 0 |
 * |        |       +------------+ +1024
 * +--------+ +0    | Linux pt 1 |
 * |        |-----> +------------+ +2048
 * +- - - - + +4    |  h/w pt 0  |
 * |        |-----> +------------+ +3072
 * +--------+ +8    |  h/w pt 1  |
 * |        |       +------------+ +4096
 *
 * See L_PTE_xxx below for definitions of bits in the "Linux pt", and
 * PTE_xxx for definitions of bits appearing in the "h/w pt".
 *
 * PMD_xxx definitions refer to bits in the first level page table.
 *
 * The "dirty" bit is emulated by only granting hardware write permission
 * iff the page is marked "writable" and "dirty" in the Linux PTE.  This
 * means that a write to a clean page will cause a permission fault, and
 * the Linux MM layer will mark the page dirty via handle_pte_fault().
 * For the hardware to notice the permission change, the TLB entry must
 * be flushed, and ptep_set_access_flags() does that for us.
 *
 * The "accessed" or "young" bit is emulated by a similar method; we only
 * allow accesses to the page if the "young" bit is set.  Accesses to the
 * page will cause a fault, and handle_pte_fault() will set the young bit
 * for us as long as the page is marked present in the corresponding Linux
 * PTE entry.  Again, ptep_set_access_flags() will ensure that the TLB is
 * up to date.
 *
 * However, when the "young" bit is cleared, we deny access to the page
 * by clearing the hardware PTE.  Currently Linux does not flush the TLB
 * for us in this case, which means the TLB will retain the transation
 * until either the TLB entry is evicted under pressure, or a context
 * switch which changes the user space mapping occurs.
 */
/*
 * Hardware-wise, we have a two level page table structure, where the first
 * level has 4096 entries, and the second level has 256 entries.  Each entry
 * is one 32-bit word.  Most of the bits in the second level entry are used
 * by hardware, and there aren't any "accessed" and "dirty" bits.
 *
 * 하드웨어 측면에서 보면, 2 level 페이지 테이블 구조입니다. 1 level은 4096 엔트
 * 리를 가지고 있고, 2 level은 256개의 엔트리를 가지고 있습니다.
 * 각각은 32-bit word입니다. 2 level 엔트리의 대부분의 bit는 하드웨어에 의해 쓰
 * 이고, "accessed"는 물론 "dirty" bit도 없습니다.
 *
 * Linux on the other hand has a three level page table structure, which can
 * be wrapped to fit a two level page table structure easily - using the PGD
 * and PTE only.  However, Linux also expects one "PTE" table per page, and
 * at least a "dirty" bit.
 *
 * 반면에, 리눅스에서는 2 레벨 페이지 테이블 구조에 맞게 쉽게 래핑이 가능한 3 레
 * 벨 페이지 테이블 구조(PGD와 PTE만 사용) 입니다. 그러나, 리눅스는 하나의
 * 페이지 당 하나의 "PTE" 테이블과 최소한 "dirty" 비트가 있기를 기대합니다.
 *
 * Therefore, we tweak the implementation slightly - we tell Linux that we
 * have 2048 entries in the first level, each of which is 8 bytes (iow, two
 * hardware pointers to the second level.)  The second level contains two
 * hardware PTE tables arranged contiguously, preceded by Linux versions
 * which contain the state information Linux needs.  We, therefore, end up
 * with 512 entries in the "PTE" level.
 *
 * 따라서, 리눅스에서는 각각 8바이트를 가지는 1 레벨 2048 엔트리를 가지도록 약간
 * 조정합니다. (즉, 2 레벨의 2개의 하드웨어 포인터) 2 레벨은 연속적으로 배치 된
 * 2 레벨 하드웨어 PTE테이블과, 리눅스에 필요한 상태 정보를 포함하는 리눅스 이전
 * 버전 것을 포함하고 있습니다. 따라서, 결국 "PTE" 레벨 512개 엔트리가 됩니다
 *
 * This leads to the page tables having the following layout:
 *
 * 페이지 테이블 레이아웃:
 *
 *    pgd             pte
 * |        |
 * +--------+
 * |        |       +------------+ +0
 * +- - - - +       | Linux pt 0 |
 * |        |       +------------+ +1024
 * +--------+ +0    | Linux pt 1 |
 * |        |-----> +------------+ +2048
 * +- - - - + +4    |  h/w pt 0  |
 * |        |-----> +------------+ +3072
 * +--------+ +8    |  h/w pt 1  |
 * |        |       +------------+ +4096
 *
 * See L_PTE_xxx below for definitions of bits in the "Linux pt", and
 * PTE_xxx for definitions of bits appearing in the "h/w pt".
 *
 * "Linux pt" 비트 정의는 L_PTE_xxx 라고 정의된 것을, 그리고 "h/w pt"를 나타내는
 * 정의는 PTE_xxx를 봐 주세요.
 *
 * PMD_xxx definitions refer to bits in the first level page table.
 *
 * PMD_xxx 정의는 1 레벨 페이지 테이블을 뜻합니다.
 *
 * The "dirty" bit is emulated by only granting hardware write permission
 * iff the page is marked "writable" and "dirty" in the Linux PTE.  This
 * means that a write to a clean page will cause a permission fault, and
 * the Linux MM layer will mark the page dirty via handle_pte_fault().
 * For the hardware to notice the permission change, the TLB entry must
 * be flushed, and ptep_set_access_flags() does that for us.
 *
 * "dirty" 비트는 하드웨어 쓰기 권한을 가졌을 때만 에뮬레이트 됩니다. 이것은 리
 * 눅스 PTE에서 "writable"과 "dirty"이 표시 된것과 동일합니다.(?) 이는 clean 페
 * 이지를 쓰는것은 권한 fault를 야기시키고, 리눅스 MM 레이어는
 * handle_pte_fault()를 통해 dirty 페이지를 표시합니다. 하드웨어는 권한 변경을
 * 통지하는 경우, TLB 엔트리는 flush 되어야 하며, ptep_set_access_flags()가 그것
 * 을 합니다.
 *
 * The "accessed" or "young" bit is emulated by a similar method; we only
 * allow accesses to the page if the "young" bit is set.  Accesses to the
 * page will cause a fault, and handle_pte_fault() will set the young bit
 * for us as long as the page is marked present in the corresponding Linux
 * PTE entry.  Again, ptep_set_access_flags() will ensure that the TLB is
 * up to date.
 *
 * "accessed" 나  "young" 비트는 비슷한 방법으로 에뮬레이트 된다. 페이지에
 * "young" 비트가 설정 되어있으면 접근을 허용합니다. 페이지에 접근 하는것은
 * fault를 야기시키고, handle_pte_fault()는 Linux PTE 엔트리에 해당 되는
 * PRESENT로 표시 되는 지는 한 "young" 비트를 설정 할 것입니다. 또 한편,
 * ptep_set_access_flags() 는 TLB가 최신인 것을 보장합니다.
 *
 * However, when the "young" bit is cleared, we deny access to the page
 * by clearing the hardware PTE.  Currently Linux does not flush the TLB
 * for us in this case, which means the TLB will retain the transation
 * until either the TLB entry is evicted under pressure, or a context
 * switch which changes the user space mapping occurs.
 *
 * 그러나, "young" 비트가 clear일때, 하드웨어 PTE를 clear하여 접근을 거부합니다.
 * 이경우에 리눅스는 TLB를 flush하지 않습니다. - TLB는 TLB엔트리가 퇴거 압박을
 * 받거나, user space 맵핑으로 변경되는 컨텍스트 전환이 되거나 둘중 하나가 될때
 * 까지 트랜잭션을 유지합니다.
 */
#define PTRS_PER_PTE		512
#define PTRS_PER_PMD		1
#define PTRS_PER_PGD		2048

#define PTE_HWTABLE_PTRS	(PTRS_PER_PTE)
#define PTE_HWTABLE_OFF		(PTE_HWTABLE_PTRS * sizeof(pte_t))
#define PTE_HWTABLE_SIZE	(PTRS_PER_PTE * sizeof(u32))

/*
 * PMD_SHIFT determines the size of the area a second-level page table can map
 * PGDIR_SHIFT determines what a third-level page table entry can map
 */
/* IAMROOT-12CD (2016-09-10):
 * --------------------------
 * 2단계 페이지 테이블 구조
 *  31        21 20     12 11        0
 * +------------+---------+-----------+
 * | PGD = PMD  |  PTE    | offset    |
 * +------------+---------+-----------+
 */
#define PMD_SHIFT		21
#define PGDIR_SHIFT		21

/* IAMROOT-12CD (2016-09-24):
 * --------------------------
 * PMD_SIZE = 2M
 */
#define PMD_SIZE		(1UL << PMD_SHIFT)
#define PMD_MASK		(~(PMD_SIZE-1))
#define PGDIR_SIZE		(1UL << PGDIR_SHIFT)
#define PGDIR_MASK		(~(PGDIR_SIZE-1))

/*
 * section address mask and size definitions.
 */
#define SECTION_SHIFT		20
#define SECTION_SIZE		(1UL << SECTION_SHIFT)
#define SECTION_MASK		(~(SECTION_SIZE-1))

/*
 * ARMv6 supersection address mask and size definitions.
 */
#define SUPERSECTION_SHIFT	24
#define SUPERSECTION_SIZE	(1UL << SUPERSECTION_SHIFT)
#define SUPERSECTION_MASK	(~(SUPERSECTION_SIZE-1))

#define USER_PTRS_PER_PGD	(TASK_SIZE / PGDIR_SIZE)

/*
 * "Linux" PTE definitions.
 *
 * We keep two sets of PTEs - the hardware and the linux version.
 * This allows greater flexibility in the way we map the Linux bits
 * onto the hardware tables, and allows us to have YOUNG and DIRTY
 * bits.
 *
 * The PTE table pointer refers to the hardware entries; the "Linux"
 * entries are stored 1024 bytes below.
 */
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * 페이지가 메모리에 상주해 있고 스왑 아웃되지 않은 상태
 */
#define L_PTE_VALID		(_AT(pteval_t, 1) << 0)		/* Valid */
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * 페이지가 메모리에 상주해 있고 스왑 아웃되지 않은 상태
 */
#define L_PTE_PRESENT		(_AT(pteval_t, 1) << 0)
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * 페이지에 접근이 된 경우
 */
#define L_PTE_YOUNG		(_AT(pteval_t, 1) << 1)
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * 페이지가 변경된 경우
 */
#define L_PTE_DIRTY		(_AT(pteval_t, 1) << 6)
#define L_PTE_RDONLY		(_AT(pteval_t, 1) << 7)
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * user process가 접근 가능한 경우 1, 커널만 접근 가능하게 할 경우 0
 */
#define L_PTE_USER		(_AT(pteval_t, 1) << 8)
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * Excute Never로 실행 금지
 */
#define L_PTE_XN		(_AT(pteval_t, 1) << 9)
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * 공유된 페이지
 */
#define L_PTE_SHARED		(_AT(pteval_t, 1) << 10)	/* shared(v6), coherent(xsc3) */
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * 페이지가 있으나 access 할 수 없는 페이지
 */
#define L_PTE_NONE		(_AT(pteval_t, 1) << 11)

/*
 * These are the memory types, defined to be compatible with
 * pre-ARMv6 CPUs cacheable and bufferable bits:   XXCB
 */
/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * XXCB --> XX(TEX), C(cacheable), B(bufferable)
 *	TEX	C	B
 *	0	1	0	Write Through(no Write Allocate)
 *	0	1	1	Write Back(no Write Allocate)
 *	1	1	1	Write Back, Write Allocate
 */
#define L_PTE_MT_UNCACHED	(_AT(pteval_t, 0x00) << 2)	/* 0000 */
#define L_PTE_MT_BUFFERABLE	(_AT(pteval_t, 0x01) << 2)	/* 0001 */
#define L_PTE_MT_WRITETHROUGH	(_AT(pteval_t, 0x02) << 2)	/* 0010 */
#define L_PTE_MT_WRITEBACK	(_AT(pteval_t, 0x03) << 2)	/* 0011 */
#define L_PTE_MT_MINICACHE	(_AT(pteval_t, 0x06) << 2)	/* 0110 (sa1100, xscale) */
#define L_PTE_MT_WRITEALLOC	(_AT(pteval_t, 0x07) << 2)	/* 0111 */
#define L_PTE_MT_DEV_SHARED	(_AT(pteval_t, 0x04) << 2)	/* 0100 */
#define L_PTE_MT_DEV_NONSHARED	(_AT(pteval_t, 0x0c) << 2)	/* 1100 */
#define L_PTE_MT_DEV_WC		(_AT(pteval_t, 0x09) << 2)	/* 1001 */
#define L_PTE_MT_DEV_CACHED	(_AT(pteval_t, 0x0b) << 2)	/* 1011 */
#define L_PTE_MT_VECTORS	(_AT(pteval_t, 0x0f) << 2)	/* 1111 */
#define L_PTE_MT_MASK		(_AT(pteval_t, 0x0f) << 2)

#ifndef __ASSEMBLY__

/*
 * The "pud_xxx()" functions here are trivial when the pmd is folded into
 * the pud: the pud entry is never bad, always exists, and can't be set or
 * cleared.
 */
#define pud_none(pud)		(0)
#define pud_bad(pud)		(0)
#define pud_present(pud)	(1)
#define pud_clear(pudp)		do { } while (0)
#define set_pud(pud,pudp)	do { } while (0)

static inline pmd_t *pmd_offset(pud_t *pud, unsigned long addr)
{
	return (pmd_t *)pud;
}

#define pmd_large(pmd)		(pmd_val(pmd) & 2)
#define pmd_bad(pmd)		(pmd_val(pmd) & 2)

#define copy_pmd(pmdpd,pmdps)		\
	do {				\
		pmdpd[0] = pmdps[0];	\
		pmdpd[1] = pmdps[1];	\
		flush_pmd_entry(pmdpd);	\
	} while (0)

#define pmd_clear(pmdp)			\
	do {				\
		pmdp[0] = __pmd(0);	\
		pmdp[1] = __pmd(0);	\
		clean_pmd_entry(pmdp);	\
	} while (0)

/* we don't need complex calculations here as the pmd is folded into the pgd */
#define pmd_addr_end(addr,end) (end)

#define set_pte_ext(ptep,pte,ext) cpu_set_pte_ext(ptep,pte,ext)
#define pte_special(pte)	(0)
static inline pte_t pte_mkspecial(pte_t pte) { return pte; }

/*
 * We don't have huge page support for short descriptors, for the moment
 * define empty stubs for use by pin_page_for_write.
 */
#define pmd_hugewillfault(pmd)	(0)
#define pmd_thp_or_huge(pmd)	(0)

#endif /* __ASSEMBLY__ */

#endif /* _ASM_PGTABLE_2LEVEL_H */
