/*
 *  linux/arch/arm/mm/mmu.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/sections.h>
#include <asm/cachetype.h>
#include <asm/fixmap.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/tlb.h>
#include <asm/highmem.h>
#include <asm/system_info.h>
#include <asm/traps.h>
#include <asm/procinfo.h>
#include <asm/memory.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/pci.h>
#include <asm/fixmap.h>

#include "mm.h"
#include "tcm.h"

/*
 * empty_zero_page is a special page that is used for
 * zero-initialized data and COW.
 */
struct page *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

/*
 * The pmd table for the upper-most set of pages.
 */
/* IAMROOT-12 fehead (2016-11-16):
 * --------------------------
 * 맨 위에있는 페이지 집합에 대한 pmd 테이블입니다.
 */
pmd_t *top_pmd;

/* IAMROOT-12CD (2016-08-23):
 * --------------------------
 * _PAGE_USER_TABLE	(PMD_TYPE_TABLE | PMD_BIT4 | PMD_DOMAIN(DOMAIN_USER))
 * _PAGE_USER_TABLE = 0x1 | 0x10 | 0x20 = 0x31
 */
pmdval_t user_pmd_table = _PAGE_USER_TABLE;

#define CPOLICY_UNCACHED	0
#define CPOLICY_BUFFERED	1
#define CPOLICY_WRITETHROUGH	2
#define CPOLICY_WRITEBACK	3
#define CPOLICY_WRITEALLOC	4

/* IAMROOT-12D (2016-05-25):
 * --------------------------
 * 라즈베리 파이2는 cachepolicy = 4;	// CPOLICY_WRITEALLOC
 */
static unsigned int cachepolicy __initdata = CPOLICY_WRITEBACK;
static unsigned int ecc_mask __initdata = 0;
pgprot_t pgprot_user;
pgprot_t pgprot_kernel;
pgprot_t pgprot_hyp_device;
pgprot_t pgprot_s2;
pgprot_t pgprot_s2_device;

EXPORT_SYMBOL(pgprot_user);
EXPORT_SYMBOL(pgprot_kernel);

struct cachepolicy {
	const char	policy[16];
	unsigned int	cr_mask;
	pmdval_t	pmd;
	pteval_t	pte;
	pteval_t	pte_s2;
};

#ifdef CONFIG_ARM_LPAE
#define s2_policy(policy)	policy
#else
#define s2_policy(policy)	0
#endif

static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.cr_mask	= CR_W|CR_C,
		.pmd		= PMD_SECT_UNCACHED,
		.pte		= L_PTE_MT_UNCACHED,
		.pte_s2		= s2_policy(L_PTE_S2_MT_UNCACHED),
	}, {
		.policy		= "buffered",
		.cr_mask	= CR_C,
		.pmd		= PMD_SECT_BUFFERED,
		.pte		= L_PTE_MT_BUFFERABLE,
		.pte_s2		= s2_policy(L_PTE_S2_MT_UNCACHED),
	}, {
		.policy		= "writethrough",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WT,
		.pte		= L_PTE_MT_WRITETHROUGH,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITETHROUGH),
	}, {
		.policy		= "writeback",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WB,
		.pte		= L_PTE_MT_WRITEBACK,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITEBACK),
	}, {
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITEBACK),
	}
};

#ifdef CONFIG_CPU_CP15
/* IAMROOT-12D (2016-05-25):
 * --------------------------
 * initial_pmd_value = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ |
 *		PMD_SECT_AF | PMD_SECT_WBWA|PMD_SECT_S;
 */
static unsigned long initial_pmd_value __initdata = 0;

/*
 * Initialise the cache_policy variable with the initial state specified
 * via the "pmd" value.  This is used to ensure that on ARMv6 and later,
 * the C code sets the page tables up with the same policy as the head
 * assembly code, which avoids an illegal state where the TLBs can get
 * confused.  See comments in early_cachepolicy() for more information.
 */
/* IAMROOT-12D (2016-05-25):
 * --------------------------
 * cmd = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | PMD_SECT_AF |
 *	| PMD_SECT_WBWA|PMD_SECT_S;
 */
void __init init_default_cache_policy(unsigned long pmd)
{
	int i;

	initial_pmd_value = pmd;

	pmd &= PMD_SECT_TEX(1) | PMD_SECT_BUFFERABLE | PMD_SECT_CACHEABLE;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++)
		if (cache_policies[i].pmd == pmd) {
			cachepolicy = i;	/* IAMROOT-12D : 4 */
			break;
		}

	if (i == ARRAY_SIZE(cache_policies))
		pr_err("ERROR: could not find cache policy\n");
}

/*
 * These are useful for identifying cache coherency problems by allowing
 * the cache or the cache and writebuffer to be turned off.  (Note: the
 * write buffer should not be on and the cache off).
 */
/* IAMROOT-12CD (2016-09-10):
 * --------------------------
 * 다음은 해제 될 캐시 또는 캐시와 writebuffer을 허용하여 캐시 일관성 문제를
 * 식별하는 데 유용합니다(쓰기 버퍼는 켜지않거나 cache off를 하지 않는것이 좋다)
 */
static int __init early_cachepolicy(char *p)
{
	int i, selected = -1;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(p, cache_policies[i].policy, len) == 0) {
			selected = i;
			break;
		}
	}

	if (selected == -1)
		pr_err("ERROR: unknown or unsupported cache policy\n");

	/*
	 * This restriction is partly to do with the way we boot; it is
	 * unpredictable to have memory mapped using two different sets of
	 * memory attributes (shared, type, and cache attribs).  We can not
	 * change these attributes once the initial assembly has setup the
	 * page tables.
	 */
	if (cpu_architecture() >= CPU_ARCH_ARMv6 && selected != cachepolicy) {
		pr_warn("Only cachepolicy=%s supported on ARMv6 and later\n",
			cache_policies[cachepolicy].policy);
		return 0;
	}

	if (selected != cachepolicy) {
		unsigned long cr = __clear_cr(cache_policies[selected].cr_mask);
		cachepolicy = selected;
		flush_cache_all();
		set_cr(cr);
	}
	return 0;
}
early_param("cachepolicy", early_cachepolicy);

static int __init early_nocache(char *__unused)
{
	char *p = "buffered";
	pr_warn("nocache is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nocache", early_nocache);

static int __init early_nowrite(char *__unused)
{
	char *p = "uncached";
	pr_warn("nowb is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nowb", early_nowrite);

#ifndef CONFIG_ARM_LPAE
static int __init early_ecc(char *p)
{
	if (memcmp(p, "on", 2) == 0)
		ecc_mask = PMD_PROTECTION;
	else if (memcmp(p, "off", 3) == 0)
		ecc_mask = 0;
	return 0;
}
early_param("ecc", early_ecc);
#endif

#else /* ifdef CONFIG_CPU_CP15 */

static int __init early_cachepolicy(char *p)
{
	pr_warn("cachepolicy kernel parameter not supported without cp15\n");
}
early_param("cachepolicy", early_cachepolicy);

static int __init noalign_setup(char *__unused)
{
	pr_warn("noalign kernel parameter not supported without cp15\n");
}
__setup("noalign", noalign_setup);

#endif /* ifdef CONFIG_CPU_CP15 / else */

/* IAMROOT-12CD (2016-09-03):
 * --------------------------
 * IO 디바이스 메모리 매핑 영역은 실행불가, 쓰기 가능(DIRTY - DEVICE에서
 *	writable 로사용), 항상 메모리상주(L_PTE_PRESENT) 되어 있어야함. 
 * TODO: L_PTE_YOUNG 
 */
#define PROT_PTE_DEVICE		L_PTE_PRESENT|L_PTE_YOUNG|L_PTE_DIRTY|L_PTE_XN
#define PROT_PTE_S2_DEVICE	PROT_PTE_DEVICE
#define PROT_SECT_DEVICE	PMD_TYPE_SECT|PMD_SECT_AP_WRITE

/* IAMROOT-12CD (2016-08-27):
 * --------------------------
 * mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
 * mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
 * mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
 * mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;
 * mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_XN;
 * 
 *  mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
 *  mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
 *  mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
 *
 * Privileged 모드에서 읽기만 허용, User 모드에서는 접근 불가.
 *	ARM Architecture Reference Manual  B4-9 참고.
 * mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
 * mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
 * mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
 *
 * 공유 설정
 * mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_S;
 * mem_types[MT_DEVICE_WC].prot_pte |= L_PTE_SHARED;
 * mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_S;
 * mem_types[MT_DEVICE_CACHED].prot_pte |= L_PTE_SHARED;
 * mem_types[MT_MEMORY_RWX].prot_sect |= PMD_SECT_S;
 * mem_types[MT_MEMORY_RWX].prot_pte |= L_PTE_SHARED;
 * mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_S;
 * mem_types[MT_MEMORY_RW].prot_pte |= L_PTE_SHARED;
 * mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
 * mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_S;
 * mem_types[MT_MEMORY_RWX_NONCACHED].prot_pte |= L_PTE_SHARED;
 *
 * Non-cacheable Normal is XCB = 001
 * mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_BUFFERED;
 *
 * mem_types[MT_LOW_VECTORS].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
 * mem_types[MT_HIGH_VECTORS].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
 *
 * mem_types[MT_MEMORY_RWX].prot_sect |= PMD_SECT_WBWA
 * mem_types[MT_MEMORY_RWX].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
 * mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_WBWA
 * mem_types[MT_MEMORY_RW].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
 * mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
 * mem_types[MT_ROM].prot_sect |= PMD_SECT_WBWA
 *
 * mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
 */
static struct mem_type mem_types[] = {
	[MT_DEVICE] = {		  /* Strongly ordered / ARMv6 shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
				  L_PTE_SHARED,
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_pte_s2 = L_PTE_SHARED = 0x400
		 */
		.prot_pte_s2	= s2_policy(PROT_PTE_S2_DEVICE) |
				  s2_policy(L_PTE_S2_MT_DEV_SHARED) |
				  L_PTE_SHARED,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_l1	= PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-27):
		 * --------------------------
		 * .prot_sect |= PMD_SECT_XN 
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_S,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_NONSHARED] = { /* ARMv6 non-shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_NONSHARED,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_l1	= PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect |= (PMD_SECT_XN | PMD_SECT_TEX(1))
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_CACHED] = {	  /* ioremap_cached */
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED |
		 *		L_PTE_SHARED(0x400) = 0x66f
		 */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_l1	= PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect |= (PMD_SECT_XN | PMD_SECT_S(0x10000)) = 0x1041e
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_WB,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_WC] = {	/* ioremap_wc */
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC |
		 *		L_PTE_SHARED(0x400) = 0x667
		 * 
		 */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_l1	= PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect = PROT_SECT_DEVICE | PMD_SECT_XN |
		 *	PMD_SECT_BUFFERABLE | PMD_SECT_S = 0x10416
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_UNCACHED] = {
		.prot_pte	= PROT_PTE_DEVICE,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_l1	= PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_IO)
		 */
		.prot_sect	= PMD_TYPE_SECT | PMD_SECT_XN,
		.domain		= DOMAIN_IO,
	},
	[MT_CACHECLEAN] = {
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_APX|
		 *	PMD_SECT_AP_WRITE = 0x2 | 0x10 | 0x8000 | 0x400 = 0x8412
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
#ifndef CONFIG_ARM_LPAE
	[MT_MINICLEAN] = {
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect = PMD_TYPE_SECT | PMD_TYPE_SECT | PMD_SECT_XN |
		 *	PMD_SECT_MINICACHE | PMD_SECT_APX|PMD_SECT_AP_WRITE
		 *	= 0x02 | 0x10 | 0x1008 | 0x8000 | 0x400 = 0x941a
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_MINICACHE,
		.domain    = DOMAIN_KERNEL,
	},
#endif
	[MT_LOW_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_RDONLY,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_USER)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	[MT_HIGH_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_USER | L_PTE_RDONLY,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_USER)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	[MT_MEMORY_RWX] = {
		/* IAMROOT-12CD (2016-10-03):	debug 완료
		 * --------------------------
		 * .prot_pte |= L_PTE_SHARED | L_PTE_MT_WRITEALLOC | L_PTE_SHARED
		 * = 0x45f
		 *	1098 7654 3210
		 *	0100 0101 1111
		 */
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		/* IAMROOT-12CD (2016-10-03):	debug 완료.
		 * --------------------------
		 * = 0x1
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-23):	debug 완료.
		 * --------------------------
		 * .prot_sect |= PMD_SECT_S | (PMD_SECT_TEX(1) | PMD_SECT_CACHEABLE | PMD_SECT_BUFFERABLE)
		 * = 0x1140e
		 *	9876	5432	1098	7654	3210
		 *	0001	0001	0100	0000	1110
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW] = {
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
		 *		L_PTE_XN | L_PTE_SHARED
		 */
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
			     L_PTE_XN,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect |= (PMD_SECT_XN | PMD_SECT_S)
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_ROM] = {
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect = PMD_TYPE_SECT | PMD_SECT_APX|PMD_SECT_AP_WRITE
		 *	= 0x02 | 0x8000 | 0x400 = 0x8402
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RWX_NONCACHED] = {
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_pte |= L_PTE_SHARED
		 */
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_BUFFERABLE,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_sect |= (PMD_SECT_S | PMD_SECT_BUFFERED)
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW_DTCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RWX_ITCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW_SO] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_UNCACHED | L_PTE_XN,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_sect |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_S |
				PMD_SECT_UNCACHED | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DMA_READY] = {
		/* IAMROOT-12CD (2016-08-23):
		 * --------------------------
		 * .prot_pte |= L_PTE_SHARED
		 */
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * .prot_l1 |= PMD_DOMAIN(DOMAIN_KERNEL)
		 */
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
};

const struct mem_type *get_mem_type(unsigned int type)
{
	return type < ARRAY_SIZE(mem_types) ? &mem_types[type] : NULL;
}
EXPORT_SYMBOL(get_mem_type);

/*
 * To avoid TLB flush broadcasts, this uses local_flush_tlb_kernel_range().
 * As a result, this can only be called with preemption disabled, as under
 * stop_machine().
 */
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
	unsigned long vaddr = __fix_to_virt(idx);
	pte_t *pte = pte_offset_kernel(pmd_off_k(vaddr), vaddr);

	/* Make sure fixmap region does not exceed available allocation. */
	BUILD_BUG_ON(FIXADDR_START + (__end_of_fixed_addresses * PAGE_SIZE) >
		     FIXADDR_END);
	BUG_ON(idx >= __end_of_fixed_addresses);

	if (pgprot_val(prot))
		set_pte_at(NULL, vaddr, pte,
			pfn_pte(phys >> PAGE_SHIFT, prot));
	else
		pte_clear(NULL, vaddr, pte);
	local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE);
}

/*
 * Adjust the PMD section entries according to the CPU in use.
 */
static void __init build_mem_type_table(void)
{
	struct cachepolicy *cp;
	/* IAMROOT-12CD (2016-08-27):
	 * --------------------------
	 * 라즈베리파이2 기본값 : 0x10c5387d
	 */
	unsigned int cr = get_cr();
	pteval_t user_pgprot, kern_pgprot, vecs_pgprot;
	pteval_t hyp_device_pgprot, s2_pgprot, s2_device_pgprot;
	/* IAMROOT-12CD (2016-08-27):
	 * --------------------------
	 * cpu_arch = CPU_ARCH_ARMv7; // 9
	 */
	int cpu_arch = cpu_architecture();
	int i;

	if (cpu_arch < CPU_ARCH_ARMv6) {
#if defined(CONFIG_CPU_DCACHE_DISABLE)
		if (cachepolicy > CPOLICY_BUFFERED)
			cachepolicy = CPOLICY_BUFFERED;
#elif defined(CONFIG_CPU_DCACHE_WRITETHROUGH)
		if (cachepolicy > CPOLICY_WRITETHROUGH)
			cachepolicy = CPOLICY_WRITETHROUGH;
#endif
	}
	if (cpu_arch < CPU_ARCH_ARMv5) {
		if (cachepolicy >= CPOLICY_WRITEALLOC)
			cachepolicy = CPOLICY_WRITEBACK;
		ecc_mask = 0;
	}

	if (is_smp()) {
		if (cachepolicy != CPOLICY_WRITEALLOC) {
			pr_warn("Forcing write-allocate cache policy for SMP\n");
			cachepolicy = CPOLICY_WRITEALLOC;
		}
		if (!(initial_pmd_value & PMD_SECT_S)) {
			pr_warn("Forcing shared mappings for SMP\n");
			initial_pmd_value |= PMD_SECT_S;
		}
	}

	/*
	 * Strip out features not present on earlier architectures.
	 * Pre-ARMv5 CPUs don't have TEX bits.  Pre-ARMv6 CPUs or those
	 * without extended page tables don't have the 'Shared' bit.
	 */
	if (cpu_arch < CPU_ARCH_ARMv5)
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_TEX(7);
	if ((cpu_arch < CPU_ARCH_ARMv6 || !(cr & CR_XP)) && !cpu_is_xsc3())
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_S;

	/*
	 * ARMv5 and lower, bit 4 must be set for page tables (was: cache
	 * "update-able on write" bit on ARM610).  However, Xscale and
	 * Xscale3 require this bit to be cleared.
	 */
	if (cpu_is_xscale() || cpu_is_xsc3()) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			mem_types[i].prot_sect &= ~PMD_BIT4;
			mem_types[i].prot_l1 &= ~PMD_BIT4;
		}
	} else if (cpu_arch < CPU_ARCH_ARMv6) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			if (mem_types[i].prot_l1)
				mem_types[i].prot_l1 |= PMD_BIT4;
			if (mem_types[i].prot_sect)
				mem_types[i].prot_sect |= PMD_BIT4;
		}
	}

	/*
	 * Mark the device areas according to the CPU/architecture.
	 */
	if (cpu_is_xsc3() || (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP))) {
		if (!cpu_is_xsc3()) {
			/*
			 * Mark device regions on ARMv6+ as execute-never
			 * to prevent speculative instruction fetches.
			 */
			/* IAMROOT-12CD (2016-08-27):
			 * --------------------------
			 * 위험한 명령어 fetches를 방지하기 위해 실행 방지로
			 * ARMv6+에 표시 디바이스 영역 표시한다.
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;

			/* Also setup NX memory mapping */
			mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_XN;
		}
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/*
			 * For ARMv7 with TEX remapping,
			 * - shared device is SXCB=1100
			 * - nonshared device is SXCB=0100
			 * - write combine device mem is SXCB=0001
			 * (Uncached Normal memory)
			 */
			/* IAMROOT-12CD (2016-08-27):
			 * --------------------------
			 * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ddi0211k/Babifihd.html
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
		} else if (cpu_is_xsc3()) {
			/*
			 * For Xscale3,
			 * - shared device is TEXCB=00101
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Inner/Outer Uncacheable in xsc3 parlance)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1) | PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		} else {
			/*
			 * For ARMv6 and ARMv7 without TEX remapping,
			 * - shared device is TEXCB=00001
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Uncached Normal in ARMv6 parlance).
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		}
	} else {
		/*
		 * On others, write combining is "Uncached/Buffered"
		 */
		mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
	}

	/*
	 * Now deal with the memory-type mappings
	 */
	/* IAMROOT-12CD (2016-09-03):
	 * --------------------------
	 * cp = {
	 * 	.policy		= "writealloc",
	 * 	.cr_mask	= 0,
	 * 	.pmd		= PMD_SECT_WBWA,
	 * 	.pte		= L_PTE_MT_WRITEALLOC,
	 * 	.pte_s2		= 0, 라즈베리파이는 쓰이지 않는다.
	 * }
	 */
	cp = &cache_policies[cachepolicy];
	/* IAMROOT-12CD (2016-09-03):
	 * --------------------------
	 * vecs_pgprot = kern_pgprot = user_pgprot = L_PTE_MT_WRITEALLOC
	 */
	vecs_pgprot = kern_pgprot = user_pgprot = cp->pte;
	/* IAMROOT-12CD (2016-08-23):
	 * --------------------------
	 * s2_pgprot = cp->pte_s2 = 0
	 */
	s2_pgprot = cp->pte_s2;
	/* IAMROOT-12CD (2016-08-23):
	 * --------------------------
	 * hyp_device_pgprot = PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
	 *	L_PTE_SHARED = 0x653
	 * s2_device_pgprot = L_PTE_SHARED = 0x400
	 *
	 * 하이퍼바이저(hypervisor)는 호스트 컴퓨터에서 다수의 운영 체제(
	 *	operating system)를 동시에 실행하기 위한 논리적 플랫폼(platform)
	 *	을 말한다. 가상화 머신 모니터(virtual machine monitor, 줄여서
	 *	VMM)라고도 부른다.
	 */
	hyp_device_pgprot = mem_types[MT_DEVICE].prot_pte;
	s2_device_pgprot = mem_types[MT_DEVICE].prot_pte_s2;

#ifndef CONFIG_ARM_LPAE
	/*
	 * We don't use domains on ARMv6 (since this causes problems with
	 * v6/v7 kernels), so we must use a separate memory type for user
	 * r/o, kernel r/w to map the vectors page.
	 */
	if (cpu_arch == CPU_ARCH_ARMv6)
		vecs_pgprot |= L_PTE_MT_VECTORS;

	/*
	 * Check is it with support for the PXN bit
	 * in the Short-descriptor translation table format descriptors.
	 */
	/* IAMROOT-12CD (2016-08-23):
	 * --------------------------
	 * read_cpuid_ext(CPUID_EXT_MMFR0)  ->  0x10201105
	 *	mrc        p15, 0, %0, c0, c1, 4
	 *	[3:0] VMSA support
	 * 	Indicates support for a Virtual Memory System Architecture(VMSA)
	 */
	if (cpu_arch == CPU_ARCH_ARMv7 &&
		(read_cpuid_ext(CPUID_EXT_MMFR0) & 0xF) == 4) {
		user_pmd_table |= PMD_PXNTABLE;
	}
#endif

	/*
	 * ARMv6 and above have extended page tables.
	 */
	/* IAMROOT-12CD (2016-08-23):
	 * --------------------------
	 * cr = 0x10c5387d, CR_XP = 0x800000
	 * cr & CR_XP = 0x800000
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP)) {
#ifndef CONFIG_ARM_LPAE
		/*
		 * Mark cache clean areas and XIP ROM read only
		 * from SVC mode and no access from userspace.
		 */
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * APX	AP[1:0](RW)	Privileged 권한	user 권한
		 * 1	0b01		Read only	No access
		 */
		mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
#endif

		/*
		 * If the initial page tables were created with the S bit
		 * set, then we need to do the same here for the same
		 * reasons given in early_cachepolicy().
		 */
/* IAMROOT-12CD (2016-09-10):
 * --------------------------
 * 초기 페이지 테이블이 S 비트 세트를 만든 경우, 우리는 early_cachepolicy()에
 * 주어진 같은 이유로 여기에 동일한 작업을 수행해야합니다.
 * initial_pmd_value = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ |
 *		PMD_SECT_AF | PMD_SECT_WBWA|PMD_SECT_S;
 *	PMD_SEC_S : shared 가 아닐까?
 */
		if (initial_pmd_value & PMD_SECT_S) {
			/* IAMROOT-12CD (2016-09-10):
			 * --------------------------
			 * user_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
			 * kern_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
			 * vecs_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
			 * s2_pgprot = L_PTE_SHARED
			 */
			user_pgprot |= L_PTE_SHARED;
			kern_pgprot |= L_PTE_SHARED;
			vecs_pgprot |= L_PTE_SHARED;
			s2_pgprot |= L_PTE_SHARED;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_WC].prot_pte |= L_PTE_SHARED;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_CACHED].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RWX].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RWX].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RW].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_pte |= L_PTE_SHARED;
		}
	}

	/*
	 * Non-cacheable Normal - intended for memory areas that must
	 * not cause dirty cache line writebacks when used
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6) {
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/* Non-cacheable Normal is XCB = 001 */
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |=
				PMD_SECT_BUFFERED;
		} else {
			/* For both ARMv6 and non-TEX-remapping ARMv7 */
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |=
				PMD_SECT_TEX(1);
		}
	} else {
		mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_BUFFERABLE;
	}

#ifdef CONFIG_ARM_LPAE
	/*
	 * Do not generate access flag faults for the kernel mappings.
	 */
	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		mem_types[i].prot_pte |= PTE_EXT_AF;
		if (mem_types[i].prot_sect)
			mem_types[i].prot_sect |= PMD_SECT_AF;
	}
	kern_pgprot |= PTE_EXT_AF;
	vecs_pgprot |= PTE_EXT_AF;

	/*
	 * Set PXN for user mappings
	 */
	user_pgprot |= PTE_EXT_PXN;
#endif

	for (i = 0; i < 16; i++) {
		pteval_t v = pgprot_val(protection_map[i]);
		/* IAMROOT-12CD (2016-09-10):
		 * --------------------------
		 * user_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
		 */
		protection_map[i] = __pgprot(v | user_pgprot);
	}

	/* IAMROOT-12CD (2016-09-10):
	 * --------------------------
	 * user_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * kern_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * vecs_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * s2_pgprot = L_PTE_SHARED
	 */
	mem_types[MT_LOW_VECTORS].prot_pte |= vecs_pgprot;
	mem_types[MT_HIGH_VECTORS].prot_pte |= vecs_pgprot;

	/* IAMROOT-12CD (2016-09-10):
	 * --------------------------
	 * pgprot_user = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * pgprot_kernel = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY | L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * pgprot_s2 = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_SHARED
	 * pgprot_s2_device = L_PTE_SHARED
	 * pgprot_hyp_device  = PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED | L_PTE_SHARED
	 */
	pgprot_user   = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | user_pgprot);
	pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
				 L_PTE_DIRTY | kern_pgprot);
	pgprot_s2  = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | s2_pgprot);
	pgprot_s2_device  = __pgprot(s2_device_pgprot);
	pgprot_hyp_device  = __pgprot(hyp_device_pgprot);

	mem_types[MT_LOW_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_HIGH_VECTORS].prot_l1 |= ecc_mask;
	/* IAMROOT-12CD (2016-09-10):
	 * --------------------------
	 * cp->pmd = PMD_SECT_WBWA,
	 * kern_pgprot = L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * 
	 * mem_types[MT_MEMORY_RWX].prot_sect |= PMD_SECT_WBWA
	 * mem_types[MT_MEMORY_RWX].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_WBWA
	 * mem_types[MT_MEMORY_RW].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_MT_WRITEALLOC | L_PTE_SHARED
	 * mem_types[MT_ROM].prot_sect |= PMD_SECT_WBWA
	 */
	mem_types[MT_MEMORY_RWX].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RWX].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RW].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RW].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_DMA_READY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= ecc_mask;
	mem_types[MT_ROM].prot_sect |= cp->pmd;

	switch (cp->pmd) {
	case PMD_SECT_WT:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WT;
		break;
	case PMD_SECT_WB:
	case PMD_SECT_WBWA:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
		break;
	}
	/* IAMROOT-12CD (2016-09-10):
	 * --------------------------
	 * ecc_mask = 0
	 * cp->policy = "writealloc",
	 */
	pr_info("Memory policy: %sData cache %s\n",
		ecc_mask ? "ECC enabled, " : "", cp->policy);

	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		struct mem_type *t = &mem_types[i];
		if (t->prot_l1)
			t->prot_l1 |= PMD_DOMAIN(t->domain);
		if (t->prot_sect)
			t->prot_sect |= PMD_DOMAIN(t->domain);
	}
}

#ifdef CONFIG_ARM_DMA_MEM_BUFFERABLE
pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);
#endif

#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)

static void __init *early_alloc_aligned(unsigned long sz, unsigned long align)
{
	void *ptr = __va(memblock_alloc(sz, align));
	memset(ptr, 0, sz);
	return ptr;
}

static void __init *early_alloc(unsigned long sz)
{
	return early_alloc_aligned(sz, sz);
}

static pte_t * __init early_pte_alloc(pmd_t *pmd, unsigned long addr, unsigned long prot)
{
	if (pmd_none(*pmd)) {
		pte_t *pte = early_alloc(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE);
		__pmd_populate(pmd, __pa(pte), prot);
	}
	BUG_ON(pmd_bad(*pmd));
	return pte_offset_kernel(pmd, addr);
}

static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  const struct mem_type *type)
{
	pte_t *pte = early_pte_alloc(pmd, addr, type->prot_l1);
	do {
		set_pte_ext(pte, pfn_pte(pfn, __pgprot(type->prot_pte)), 0);
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

/* IAMROOT-12CD (2016-10-03):
 * --------------------------
 * page table에 매핑할 하드웨어 주소와 메모리 속성을 설정한다.
 *
 * type = &mem_types[MT_MEMORY_RWX]
 *  pmd		addr		end		phys		*pmd
 *  0x80006000	0x80000000	0x80200000	0		0 | prot_sect
 *  0x80006004	0x80100000			0x100000	0x100000 | ..
 *
 *  0x80006008	0x80200000	0x80400000	0x200000	0x200000 | ..
 *  0x8000600c	0x80300000			0x300000	0x300000 | ..
 *
 */
static void __init __map_init_section(pmd_t *pmd, unsigned long addr,
			unsigned long end, phys_addr_t phys,
			const struct mem_type *type)
{
	pmd_t *p = pmd;

#ifndef CONFIG_ARM_LPAE
	/*
	 * In classic MMU format, puds and pmds are folded in to
	 * the pgds. pmd_offset gives the PGD entry. PGDs refer to a
	 * group of L1 entries making up one logical pointer to
	 * an L2 table (2MB), where as PMDs refer to the individual
	 * L1 entries (1MB). Hence increment to get the correct
	 * offset for odd 1MB sections.
	 * (See arch/arm/include/asm/pgtable-2level.h)
	 */
	/* IAMROOT-12CD (2016-10-03):
	 * --------------------------
	 * SECTION_SIZE = (1UL << SECTION_SHIFT) = 0x100000
	 */
	if (addr & SECTION_SIZE)
		pmd++;
#endif
	do {
		*pmd = __pmd(phys | type->prot_sect);
		phys += SECTION_SIZE;
	} while (pmd++, addr += SECTION_SIZE, addr != end);

	flush_pmd_entry(p);
}

/* IAMROOT-12CD (2016-10-03):
 * --------------------------
 * pgd 0x80006000, addr=0x80000000, end = 0x80200000, phys = 0
 * type = &mem_types[MT_MEMORY_RWX]
 *  pud		addr		end		phys
 *  0x80006000	0x80000000	0x80200000	0
 *  0x80006008	0x80200000	0x80400000	0x200000
 *  0x8000600c	0x80600000	0x80600000	0x400000
 *  ...
 */
static void __init alloc_init_pmd(pud_t *pud, unsigned long addr,
				      unsigned long end, phys_addr_t phys,
				      const struct mem_type *type)
{
	/* IAMROOT-12CD (2016-10-03):
	 * --------------------------
	 * pmd = pud
	 */
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		/*
		 * With LPAE, we must loop over to map
		 * all the pmds for the given range.
		 */
		/* IAMROOT-12CD (2016-10-03):
		 * --------------------------
		 * next = end
		 */
		next = pmd_addr_end(addr, end);

		/*
		 * Try a section mapping - addr, next and phys must all be
		 * aligned to a section boundary.
		 */
		if (type->prot_sect &&
				((addr | next | phys) & ~SECTION_MASK) == 0) {
			__map_init_section(pmd, addr, next, phys, type);
		} else {
			alloc_init_pte(pmd, addr, next,
						__phys_to_pfn(phys), type);
		}

		phys += next - addr;

	} while (pmd++, addr = next, addr != end);
}

/* IAMROOT-12CD (2016-10-03):
 * --------------------------
 * pgd 0x80006000, addr=0x80000000, end = 0x80200000, phys = 0
 * type = &mem_types[MT_MEMORY_RWX]
 * 
 * pgd		addr		end		phys
 * 0x80006000	0x80000000	0x80200000	0
 * 0x80006008	0x80200000	0x80400000	0x200000
 * 0x8000600c	0x80400000	0x80600000	0x400000
 * 0x80006010	0x80600000	0x80800000	0x600000
 * 0x80006018	0x80800000	0x80c00000	0x800000
 * 0x8000601c	0x80900000	0x81000000	0x1000000
 */
static void __init alloc_init_pud(pgd_t *pgd, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  const struct mem_type *type)
{
	/* IAMROOT-12CD (2016-10-03):
	 * --------------------------
	 * pud = 0x80006000
	 */
	pud_t *pud = pud_offset(pgd, addr);
	unsigned long next;

	do {
		/* IAMROOT-12CD (2016-10-03):
		 * --------------------------
		 * next = end
		 */
		next = pud_addr_end(addr, end);
		alloc_init_pmd(pud, addr, next, phys, type);
		phys += next - addr;
	} while (pud++, addr = next, addr != end);
}

#ifndef CONFIG_ARM_LPAE
static void __init create_36bit_mapping(struct map_desc *md,
					const struct mem_type *type)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	pgd_t *pgd;

	addr = md->virtual;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length);

	if (!(cpu_architecture() >= CPU_ARCH_ARMv6 || cpu_is_xsc3())) {
		pr_err("MM: CPU does not support supersection mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/* N.B.	ARMv6 supersections are only defined to work with domain 0.
	 *	Since domain assignments can in fact be arbitrary, the
	 *	'domain == 0' check below is required to insure that ARMv6
	 *	supersections are only allocated for domain 0 regardless
	 *	of the actual domain assignments in use.
	 */
	if (type->domain) {
		pr_err("MM: invalid domain in supersection mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	if ((addr | length | __pfn_to_phys(md->pfn)) & ~SUPERSECTION_MASK) {
		pr_err("MM: cannot create mapping for 0x%08llx at 0x%08lx invalid alignment\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * Shift bits [35:32] of address into bits [23:20] of PMD
	 * (See ARMv6 spec).
	 */
	phys |= (((md->pfn >> (32 - PAGE_SHIFT)) & 0xF) << 20);

	pgd = pgd_offset_k(addr);
	end = addr + length;
	do {
		pud_t *pud = pud_offset(pgd, addr);
		pmd_t *pmd = pmd_offset(pud, addr);
		int i;

		for (i = 0; i < 16; i++)
			*pmd++ = __pmd(phys | type->prot_sect | PMD_SECT_SUPER);

		addr += SUPERSECTION_SIZE;
		phys += SUPERSECTION_SIZE;
		pgd += SUPERSECTION_SIZE >> PGDIR_SHIFT;
	} while (addr != end);
}
#endif	/* !CONFIG_ARM_LPAE */

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections and
 * supersections.
 */
/* IAMROOT-12CD (2016-09-24):
 * --------------------------
 * kernel 실행 영역.
 * md.pfn = 0
 * md.virtual = 0x80000000
 * md.length = 0x900000
 * md.type = MT_MEMORY_RWX
 */
static void __init create_mapping(struct map_desc *md)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	const struct mem_type *type;
	pgd_t *pgd;

	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * 사용자 영역에 맵핑을 만드는 경우 에러.
	 */
	if (md->virtual != vectors_base() && md->virtual < TASK_SIZE) {
		pr_warn("BUG: not creating mapping for 0x%08llx at 0x%08lx in user region\n",
			(long long)__pfn_to_phys((u64)md->pfn), md->virtual);
		return;
	}

	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * MT_DEVICE이거나 MT_ROM이면서 vmalloc 공간을 벗어날 영역이면 에러.
	 */
	if ((md->type == MT_DEVICE || md->type == MT_ROM) &&
	    md->virtual >= PAGE_OFFSET &&
	    (md->virtual < VMALLOC_START || md->virtual >= VMALLOC_END)) {
		pr_warn("BUG: mapping for 0x%08llx at 0x%08lx out of vmalloc space\n",
			(long long)__pfn_to_phys((u64)md->pfn), md->virtual);
	}

	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * md.type = MT_MEMORY_RWX
	 * 여기까지 12CD 끝입니다.
	 * AB팀으로 합칩니다.
	 */
	type = &mem_types[md->type];

#ifndef CONFIG_ARM_LPAE
	/*
	 * Catch 36-bit addresses
	 */
	/* IAMROOT-12CD (2016-10-03):
	 * --------------------------
	 * 32비트 주소를 넘길때.
	 */
	if (md->pfn >= 0x100000) {
		create_36bit_mapping(md, type);
		return;
	}
#endif

	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * md->virtual = 0x80000000
	 * md->length = 0x900000
	 * addr = 0x80000000
	 * phys = 0
	 * length = 0x900000 + (0x80000000 & ~PAGE_MASK) = 0x900000
	 */
	addr = md->virtual & PAGE_MASK;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));

	/* IAMROOT-12AB:
	 * -------------
	 * 메모리 타입이 섹션 매핑만 지원되는 타입이 있다.
	 * 그런 경우 섹션 align 되어 있지 않은 섹션 매핑 요청은 에러
	 */
	if (type->prot_l1 == 0 && ((addr | phys | length) & ~SECTION_MASK)) {
		pr_warn("BUG: map for 0x%08llx at 0x%08lx can not be mapped using pages, ignoring.\n",
			(long long)__pfn_to_phys(md->pfn), addr);
		return;
	}

	/* IAMROOT-12AB:
	 * -------------
	 * pgd: pgd 엔트리 주소 
	 *	rpi2: 0x8000_4000 ~ 0x8000_8000
	 */
	/* IAMROOT-12CD (2016-10-03):
	 * --------------------------
	 * pgd = 0x80006000
	 * end = 0x80900000
	 */
	pgd = pgd_offset_k(addr);
	end = addr + length;
	/* IAMROOT-12CD (2016-10-03):
	 * --------------------------
	 * next		pgd		addr		phys
	 * 0x80200000	0x80006000	0x80000000	0
	 * 0x80400000	0x80006008	0x80200000	0x200000
	 * 0x80600000	0x8000600c	0x80400000	0x400000
	 * 0x80800000	0x80006010	0x80600000	0x600000
	 * 0x80c00000	0x80006018	0x80800000	0x800000
	 * 0x81000000	0x8000601c	0x80900000	0x1000000
	 */
	do {
		unsigned long next = pgd_addr_end(addr, end);

		/* IAMROOT-12CD (2016-10-03):
		 * --------------------------
		 * pgd 0x80006000, addr=0x80000000, next = 0x80200000, phys = 0
		 * type = &mem_types[MT_MEMORY_RWX]
		 */
		alloc_init_pud(pgd, addr, next, phys, type);

		phys += next - addr;
		addr = next;
	} while (pgd++, addr != end);
}

/*
 * Create the architecture specific mappings
 */
/* IAMROOT-12D (2016-10-04):
 * --------------------------
 * map.pfn = 0x3b800
 * map.virtual = 0xbb800000
 * map.length = 0x800000
 * map.type = MT_MEMORY_DMA_READY
 * nr = 1
 */
void __init iotable_init(struct map_desc *io_desc, int nr)
{
	struct map_desc *md;
	struct vm_struct *vm;
	struct static_vm *svm;

	if (!nr)
		return;

	svm = early_alloc_aligned(sizeof(*svm) * nr, __alignof__(*svm));

	for (md = io_desc; nr; md++, nr--) {
		create_mapping(md);

		vm = &svm->vm;
		vm->addr = (void *)(md->virtual & PAGE_MASK);
		vm->size = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
		vm->phys_addr = __pfn_to_phys(md->pfn);
		vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING;
		vm->flags |= VM_ARM_MTYPE(md->type);
		vm->caller = iotable_init;
		add_static_vm_early(svm++);
	}
}

void __init vm_reserve_area_early(unsigned long addr, unsigned long size,
				  void *caller)
{
	struct vm_struct *vm;
	struct static_vm *svm;

	svm = early_alloc_aligned(sizeof(*svm), __alignof__(*svm));

	vm = &svm->vm;
	vm->addr = (void *)addr;
	vm->size = size;
	vm->flags = VM_IOREMAP | VM_ARM_EMPTY_MAPPING;
	vm->caller = caller;
	add_static_vm_early(svm);
}

#ifndef CONFIG_ARM_LPAE

/*
 * The Linux PMD is made of two consecutive section entries covering 2MB
 * (see definition in include/asm/pgtable-2level.h).  However a call to
 * create_mapping() may optimize static mappings by using individual
 * 1MB section mappings.  This leaves the actual PMD potentially half
 * initialized if the top or bottom section entry isn't used, leaving it
 * open to problems if a subsequent ioremap() or vmalloc() tries to use
 * the virtual space left free by that unused section entry.
 *
 * Let's avoid the issue by inserting dummy vm entries covering the unused
 * PMD halves once the static mappings are in place.
 */

static void __init pmd_empty_section_gap(unsigned long addr)
{
	vm_reserve_area_early(addr, SECTION_SIZE, pmd_empty_section_gap);
}

static void __init fill_pmd_gaps(void)
{
	struct static_vm *svm;
	struct vm_struct *vm;
	unsigned long addr, next = 0;
	pmd_t *pmd;

	list_for_each_entry(svm, &static_vmlist, list) {
		vm = &svm->vm;
		addr = (unsigned long)vm->addr;
		if (addr < next)
			continue;

		/*
		 * Check if this vm starts on an odd section boundary.
		 * If so and the first section entry for this PMD is free
		 * then we block the corresponding virtual address.
		 */
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr);
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr & PMD_MASK);
		}

		/*
		 * Then check if this vm ends on an odd section boundary.
		 * If so and the second section entry for this PMD is empty
		 * then we block the corresponding virtual address.
		 */
		addr += vm->size;
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr) + 1;
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr);
		}

		/* no need to look at any vm entry until we hit the next PMD */
		next = (addr + PMD_SIZE - 1) & PMD_MASK;
	}
}

#else
#define fill_pmd_gaps() do { } while (0)
#endif

#if defined(CONFIG_PCI) && !defined(CONFIG_NEED_MACH_IO_H)
static void __init pci_reserve_io(void)
{
	struct static_vm *svm;

	svm = find_static_vm_vaddr((void *)PCI_IO_VIRT_BASE);
	if (svm)
		return;

	vm_reserve_area_early(PCI_IO_VIRT_BASE, SZ_2M, pci_reserve_io);
}
#else
#define pci_reserve_io() do { } while (0)
#endif

#ifdef CONFIG_DEBUG_LL
void __init debug_ll_io_init(void)
{
	struct map_desc map;

	debug_ll_addr(&map.pfn, &map.virtual);
	if (!map.pfn || !map.virtual)
		return;
	map.pfn = __phys_to_pfn(map.pfn);
	map.virtual &= PAGE_MASK;
	map.length = PAGE_SIZE;
	map.type = MT_DEVICE;
	iotable_init(&map, 1);
}
#endif

/* IAMROOT-12CD (2016-07-23):
 * --------------------------
 * vmalloc_min = 0xEF800000
 *  3.83G 정도 되고 이 영역은 커널 하이메모리 시작 주소 바로 아래 부분이다.
 *	하이메모리 영역은 3.893G~4G영역이다. 그래서 하이메모리 바로 아래에 위치한다.
 */
static void * __initdata vmalloc_min =
	(void *)(VMALLOC_END - (240 << 20) - VMALLOC_OFFSET);

/*
 * vmalloc=size forces the vmalloc area to be exactly 'size'
 * bytes. This can be used to increase (or decrease) the vmalloc
 * area - the default is 240m.
 */
static int __init early_vmalloc(char *arg)
{
	unsigned long vmalloc_reserve = memparse(arg, NULL);

	if (vmalloc_reserve < SZ_16M) {
		vmalloc_reserve = SZ_16M;
		pr_warn("vmalloc area too small, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	if (vmalloc_reserve > VMALLOC_END - (PAGE_OFFSET + SZ_32M)) {
		vmalloc_reserve = VMALLOC_END - (PAGE_OFFSET + SZ_32M);
		pr_warn("vmalloc area is too big, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	vmalloc_min = (void *)(VMALLOC_END - vmalloc_reserve);
	return 0;
}
early_param("vmalloc", early_vmalloc);

/* IAMROOT-12CD (2016-08-06):
 * arm_lowmem_limit: 0x3c000000(960mb)
 */
phys_addr_t arm_lowmem_limit __initdata = 0;

/* IAMROOT-12CD (2016-07-23):
 * --------------------------
 * meminfo 에 근본적인 문제가 있는지 체크.
 */
void __init sanity_check_meminfo(void)
{
	phys_addr_t memblock_limit = 0;
	int highmem = 0;

	/* IAMROOT-12CD (2016-07-23):
	 * --------------------------
	 * vmalloc_limit = 0x6F800000	3.835G
	 */
	phys_addr_t vmalloc_limit = __pa(vmalloc_min - 1) + 1;
	struct memblock_region *reg;

	/* IAMROOT-12CD (2016-07-23):
	 * --------------------------
	 * for (reg = memblock.memory.regions;
	 *      reg < (memblock.memory.regions + memblock.memory.cnt);
	 *      reg++)
	 * reg.base: 0x0
	 * reg.size: 0x3c000000 (960mb)
	 * reg.flags: 0
	 */
	for_each_memblock(memory, reg) {
		phys_addr_t block_start = reg->base;
		phys_addr_t block_end = reg->base + reg->size;
		phys_addr_t size_limit = reg->size;

		if (reg->base >= vmalloc_limit)
			highmem = 1;
		else
			/* IAMROOT-12CD (2016-08-06):
			 * size_limit = 0x6F800000(3.835G) - 0;
			 */
			size_limit = vmalloc_limit - reg->base;


		if (!IS_ENABLED(CONFIG_HIGHMEM) || cache_is_vipt_aliasing()) {

			if (highmem) {
				pr_notice("Ignoring RAM at %pa-%pa (!CONFIG_HIGHMEM)\n",
					  &block_start, &block_end);
				memblock_remove(reg->base, reg->size);
				continue;
			}

			/* IAMROOT-12CD (2016-08-06):
			 * reg->size: 960mb, size_limit: 3.835G
			 */
			if (reg->size > size_limit) {
				phys_addr_t overlap_size = reg->size - size_limit;

				pr_notice("Truncating RAM at %pa-%pa to -%pa",
					  &block_start, &block_end, &vmalloc_limit);
				memblock_remove(vmalloc_limit, overlap_size);
				block_end = vmalloc_limit;
			}
		}
		/* IAMROOT-12CD (2016-08-06):
		 * block_end: 960mb, arm_lowmem_limit: 0
		 */

		if (!highmem) {
			if (block_end > arm_lowmem_limit) {
				if (reg->size > size_limit)
					arm_lowmem_limit = vmalloc_limit;
				else
					/* IAMROOT-12CD (2016-08-14):
					 * --------------------------
					 * arm_lowmem_limit: 0x3c000000(960mb)
					 */
					arm_lowmem_limit = block_end;
			}

			/*
			 * Find the first non-pmd-aligned page, and point
			 * memblock_limit at it. This relies on rounding the
			 * limit down to be pmd-aligned, which happens at the
			 * end of this function.
			 *
			 * With this algorithm, the start or end of almost any
			 * bank can be non-pmd-aligned. The only exception is
			 * that the start of the bank 0 must be section-
			 * aligned, since otherwise memory would need to be
			 * allocated when mapping the start of bank 0, which
			 * occurs before any free memory is mapped.
			 */
			if (!memblock_limit) {
				if (!IS_ALIGNED(block_start, PMD_SIZE))
					memblock_limit = block_start;
				else if (!IS_ALIGNED(block_end, PMD_SIZE))
					memblock_limit = arm_lowmem_limit;
			}

		}
	}

	/* IAMROOT-12CD (2016-08-06):
	 * high_memory = (0x3c000000 - 1) + 0x80000000 + 1
	 *             = 0xBC000000
	 */
	high_memory = __va(arm_lowmem_limit - 1) + 1;

	/*
	 * Round the memblock limit down to a pmd size.  This
	 * helps to ensure that we will allocate memory from the
	 * last full pmd, which should be mapped.
	 */
	/* IAMROOT-12CD (2016-08-06):
	 * memblock_limit = 960mb
	 */
	if (memblock_limit)
		memblock_limit = round_down(memblock_limit, PMD_SIZE);
	if (!memblock_limit)
		memblock_limit = arm_lowmem_limit;

	memblock_set_current_limit(memblock_limit);
}

static inline void prepare_page_table(void)
{
	unsigned long addr;
	phys_addr_t end;

	/*
	 * Clear out all the mappings below the kernel image.
	 */
	/* IAMROOT-12CD (2016-09-10):
	 * --------------------------
	 * 커널이미지 아래 모든 매핑을 청소(data clean)합니다.
	 * MODULES_VADDR = 2G-16M = 0x7f000000
	 * PMD_SIZE = (1 << 21) = 2M
	 *
	 *	addr		pmd_off_k
	 *	0		0x80004000
	 *	0x200000	0x80004008
	 *	0x400000	0x80004010
	 *	0x600000	0x80004018
	 *	...
	 *	0x7ee00000	0x80005fb8
	 */
	for (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

#ifdef CONFIG_XIP_KERNEL
	/* The XIP kernel is mapped in the module area -- skip over it */
	addr = ((unsigned long)_etext + PMD_SIZE - 1) & PMD_MASK;
#endif
	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 *	addr		pmd_off_k
	 *	0x7f000000	0x80005fc0
	 *	0x7f200000	0x80005fc8
	 *	0x7f400000	0x80005fd0
	 *	...
	 *	0x7fe00000	0x80005ff8
	 */
	for ( ; addr < PAGE_OFFSET; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Find the end of the first block of lowmem.
	 */
	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * .memory
	 * {cnt = 0x1, max = 0x80, total_size = 0x3c000000, regions = {
	 *   [0] = {base = 0x0, size = 0x3c000000, flags = 0x0}, 0 ~ 960M 영역.
	 *   [1] = {base = 0x0, size = 0x0, flags = 0x0},
	 *   ...
	 *
	 * end = 960M
	 */
	end = memblock.memory.regions[0].base + memblock.memory.regions[0].size;
	if (end >= arm_lowmem_limit)
		end = arm_lowmem_limit;

	/*
	 * Clear out all the kernel space mappings, except for the first
	 * memory bank, up to the vmalloc region.
	 */
	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * end = 960M, addr = 0x80000000 + 960M = 2.96G
	 * VMALLOC_START = 0xbc800000(968M의 가상주소) = 3.968G
	 * PMD_SIZE = 2M
	 *
	 * 2.96G ~ 3.968G 커널 영역의 Cache clear
	 */
	for (addr = __phys_to_virt(end);
	     addr < VMALLOC_START; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
}

#ifdef CONFIG_ARM_LPAE
/* the first page is reserved for pgd */
#define SWAPPER_PG_DIR_SIZE	(PAGE_SIZE + \
				 PTRS_PER_PGD * PTRS_PER_PMD * sizeof(pmd_t))
#else
#define SWAPPER_PG_DIR_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))
#endif

/*
 * Reserve the special regions of memory
 */
void __init arm_mm_memblock_reserve(void)
{
	/*
	 * Reserve the page tables.  These are already in use,
	 * and can only be in node 0.
	 */
	/* IAMROOT-12CD (2016-08-20):
	 * --------------------------
	 * swapper_pg_dir = 0x80004000 ,
	 * memblock_reserve(0x4000, 0x4000);
	 */
	memblock_reserve(__pa(swapper_pg_dir), SWAPPER_PG_DIR_SIZE);

#ifdef CONFIG_SA1111
	/*
	 * Because of the SA1111 DMA bug, we want to preserve our
	 * precious DMA-able memory...
	 */
	memblock_reserve(PHYS_OFFSET, __pa(swapper_pg_dir) - PHYS_OFFSET);
#endif
}

/*
 * Set up the device mappings.  Since we clear out the page tables for all
 * mappings above VMALLOC_START, we will remove any debug device mappings.
 * This means you have to be careful how you debug this function, or any
 * called function.  This means you can't use any function or debugging
 * method which may touch any device, otherwise the kernel _will_ crash.
 */
static void __init devicemaps_init(const struct machine_desc *mdesc)
{
	struct map_desc map;
	unsigned long addr;
	void *vectors;

	/*
	 * Allocate the vector page early.
	 */
	vectors = early_alloc(PAGE_SIZE * 2);

	early_trap_init(vectors);

	for (addr = VMALLOC_START; addr; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Map the kernel if it is XIP.
	 * It is always first in the modulearea.
	 */
#ifdef CONFIG_XIP_KERNEL
	map.pfn = __phys_to_pfn(CONFIG_XIP_PHYS_ADDR & SECTION_MASK);
	map.virtual = MODULES_VADDR;
	map.length = ((unsigned long)_etext - map.virtual + ~SECTION_MASK) & SECTION_MASK;
	map.type = MT_ROM;
	create_mapping(&map);
#endif

	/*
	 * Map the cache flushing regions.
	 */
#ifdef FLUSH_BASE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS);
	map.virtual = FLUSH_BASE;
	map.length = SZ_1M;
	map.type = MT_CACHECLEAN;
	create_mapping(&map);
#endif
#ifdef FLUSH_BASE_MINICACHE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS + SZ_1M);
	map.virtual = FLUSH_BASE_MINICACHE;
	map.length = SZ_1M;
	map.type = MT_MINICLEAN;
	create_mapping(&map);
#endif

	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
#ifdef CONFIG_KUSER_HELPERS
	map.type = MT_HIGH_VECTORS;
#else
	map.type = MT_LOW_VECTORS;
#endif
	create_mapping(&map);

	if (!vectors_high()) {
		map.virtual = 0;
		map.length = PAGE_SIZE * 2;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}

	/* Now create a kernel read-only mapping */
	map.pfn += 1;
	map.virtual = 0xffff0000 + PAGE_SIZE;
	map.length = PAGE_SIZE;
	map.type = MT_LOW_VECTORS;
	create_mapping(&map);

	/*
	 * Ask the machine support to map in the statically mapped devices.
	 */
	if (mdesc->map_io)
		mdesc->map_io();
	else
		debug_ll_io_init();
	fill_pmd_gaps();

	/* Reserve fixed i/o space in VMALLOC region */
	pci_reserve_io();

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state wrt the writebuffer.  This also ensures that
	 * any write-allocated cache lines in the vector page are written
	 * back.  After this point, we can start to touch devices again.
	 */
	local_flush_tlb_all();
	flush_cache_all();
}

static void __init kmap_init(void)
{
#ifdef CONFIG_HIGHMEM
	pkmap_page_table = early_pte_alloc(pmd_off_k(PKMAP_BASE),
		PKMAP_BASE, _PAGE_KERNEL_TABLE);
#endif

	early_pte_alloc(pmd_off_k(FIXADDR_START), FIXADDR_START,
			_PAGE_KERNEL_TABLE);
}

static void __init map_lowmem(void)
{
	struct memblock_region *reg;
	/* IAMROOT-12CD (2016-09-24):
	 * --------------------------
	 * _stext = 0x80008240, kernel_x_start = 0x0
	 * __init_end = 0x8080c000, kernel_x_end = 0x900000
	 * 커널 코드와 데이터는 섹션 매핑을 사용한다.
	 */
	phys_addr_t kernel_x_start = round_down(__pa(_stext), SECTION_SIZE);
	phys_addr_t kernel_x_end = round_up(__pa(__init_end), SECTION_SIZE);

	/* Map all the lowmem memory banks. */
	for_each_memblock(memory, reg) {
		/* IAMROOT-12CD (2016-08-20):
		 * --------------------------
		 * .memory
		 * {cnt = 0x1, max = 0x80, total_size = 0x3c000000, regions = {
		 *   [0] = {base = 0x0, size = 0x3c000000, flags = 0x0}, 0 ~ 960M 영역.
		 *   [1] = {base = 0x0, size = 0x0, flags = 0x0},
		 *   ...
		 * }}
		 */
		phys_addr_t start = reg->base;
		phys_addr_t end = start + reg->size;
		struct map_desc map;

		if (end > arm_lowmem_limit)
			end = arm_lowmem_limit;
		if (start >= end)
			break;

		if (end < kernel_x_start) {
			map.pfn = __phys_to_pfn(start);
			map.virtual = __phys_to_virt(start);
			map.length = end - start;
			map.type = MT_MEMORY_RWX;

			create_mapping(&map);
		} else if (start >= kernel_x_end) {
			map.pfn = __phys_to_pfn(start);
			map.virtual = __phys_to_virt(start);
			map.length = end - start;
			map.type = MT_MEMORY_RW;

			create_mapping(&map);
		} else {
			/* This better cover the entire kernel */
			/* IAMROOT-12AB:
			 * -------------
			 * 가장 일반적인 커널이 포함된 memblock으로 커널 부분만
			 * RWX로 매핑하고 나머지 영역은 RW로 매핑한다.
			 * rpi2: 가장 아래 커널이 위치한 공간이 RWX로 매핑되고
			 *       나머지 공간은 RW로 매핑된다.
			 */
			/* IAMROOT-12CD (2016-09-24):
			 * --------------------------
			 * 커널 전체를 커버 해야한다.
			 *
			 * start = 0, end = 960M
			 * _stext = 0x80008240, kernel_x_start = 0x0
			 * __init_end = 0x8080c000, kernel_x_end = 0x900000
			 */
			if (start < kernel_x_start) {
				map.pfn = __phys_to_pfn(start);
				map.virtual = __phys_to_virt(start);
				map.length = kernel_x_start - start;
				map.type = MT_MEMORY_RW;

				create_mapping(&map);
			}

			/* IAMROOT-12CD (2016-09-24):
			 * --------------------------
			 * map.pfn = 0
			 * map.virtual = 0x80000000
			 * map.length = 0x900000
			 * map.type = MT_MEMORY_RWX
			 */
			map.pfn = __phys_to_pfn(kernel_x_start);
			map.virtual = __phys_to_virt(kernel_x_start);
			map.length = kernel_x_end - kernel_x_start;
			map.type = MT_MEMORY_RWX;

			create_mapping(&map);

			if (kernel_x_end < end) {
				/* IAMROOT-12CD (2016-10-03):
				 * --------------------------
				 * map = {virtual = 0x80900000, pfn = 0x900,
				 *	length = 0x3b700000, type = 0xa}
				 */
				map.pfn = __phys_to_pfn(kernel_x_end);
				map.virtual = __phys_to_virt(kernel_x_end);
				map.length = end - kernel_x_end;
				map.type = MT_MEMORY_RW;

				create_mapping(&map);
			}
		}
	}
}

#ifdef CONFIG_ARM_LPAE
/*
 * early_paging_init() recreates boot time page table setup, allowing machines
 * to switch over to a high (>4G) address space on LPAE systems
 */
void __init early_paging_init(const struct machine_desc *mdesc,
			      struct proc_info_list *procinfo)
{
	pmdval_t pmdprot = procinfo->__cpu_mm_mmu_flags;
	unsigned long map_start, map_end;
	pgd_t *pgd0, *pgdk;
	pud_t *pud0, *pudk, *pud_start;
	pmd_t *pmd0, *pmdk;
	phys_addr_t phys;
	int i;

	if (!(mdesc->init_meminfo))
		return;

	/* remap kernel code and data */
	map_start = init_mm.start_code & PMD_MASK;
	map_end   = ALIGN(init_mm.brk, PMD_SIZE);

	/* get a handle on things... */
	pgd0 = pgd_offset_k(0);
	pud_start = pud0 = pud_offset(pgd0, 0);
	pmd0 = pmd_offset(pud0, 0);

	pgdk = pgd_offset_k(map_start);
	pudk = pud_offset(pgdk, map_start);
	pmdk = pmd_offset(pudk, map_start);

	mdesc->init_meminfo();

	/* Run the patch stub to update the constants */
	fixup_pv_table(&__pv_table_begin,
		(&__pv_table_end - &__pv_table_begin) << 2);

	/*
	 * Cache cleaning operations for self-modifying code
	 * We should clean the entries by MVA but running a
	 * for loop over every pv_table entry pointer would
	 * just complicate the code.
	 */
	flush_cache_louis();
	dsb(ishst);
	isb();

	/*
	 * FIXME: This code is not architecturally compliant: we modify
	 * the mappings in-place, indeed while they are in use by this
	 * very same code.  This may lead to unpredictable behaviour of
	 * the CPU.
	 *
	 * Even modifying the mappings in a separate page table does
	 * not resolve this.
	 *
	 * The architecture strongly recommends that when a mapping is
	 * changed, that it is changed by first going via an invalid
	 * mapping and back to the new mapping.  This is to ensure that
	 * no TLB conflicts (caused by the TLB having more than one TLB
	 * entry match a translation) can occur.  However, doing that
	 * here will result in unmapping the code we are running.
	 */
	pr_warn("WARNING: unsafe modification of in-place page tables - tainting kernel\n");
	add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);

	/*
	 * Remap level 1 table.  This changes the physical addresses
	 * used to refer to the level 2 page tables to the high
	 * physical address alias, leaving everything else the same.
	 */
	for (i = 0; i < PTRS_PER_PGD; pud0++, i++) {
		set_pud(pud0,
			__pud(__pa(pmd0) | PMD_TYPE_TABLE | L_PGD_SWAPPER));
		pmd0 += PTRS_PER_PMD;
	}

	/*
	 * Remap the level 2 table, pointing the mappings at the high
	 * physical address alias of these pages.
	 */
	phys = __pa(map_start);
	do {
		*pmdk++ = __pmd(phys | pmdprot);
		phys += PMD_SIZE;
	} while (phys < map_end);

	/*
	 * Ensure that the above updates are flushed out of the cache.
	 * This is not strictly correct; on a system where the caches
	 * are coherent with each other, but the MMU page table walks
	 * may not be coherent, flush_cache_all() may be a no-op, and
	 * this will fail.
	 */
	flush_cache_all();

	/*
	 * Re-write the TTBR values to point them at the high physical
	 * alias of the page tables.  We expect __va() will work on
	 * cpu_get_pgd(), which returns the value of TTBR0.
	 */
	cpu_switch_mm(pgd0, &init_mm);
	cpu_set_ttbr(1, __pa(pgd0) + TTBR1_OFFSET);

	/* Finally flush any stale TLB values. */
	local_flush_bp_all();
	local_flush_tlb_all();
}

#else

/* IAMROOT-12CD (2016-07-23):
 * --------------------------
 * pi2에서는 mdesc->init_meminfo 값이 NULL 이다.
 */
void __init early_paging_init(const struct machine_desc *mdesc,
			      struct proc_info_list *procinfo)
{
	if (mdesc->init_meminfo)
		mdesc->init_meminfo();
}

#endif

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps, and sets up the zero page, bad page and bad page tables.
 */
void __init paging_init(const struct machine_desc *mdesc)
{
	void *zero_page;

	build_mem_type_table();
	prepare_page_table();
	map_lowmem();
	dma_contiguous_remap();
	devicemaps_init(mdesc);
	kmap_init();
	tcm_init();

	top_pmd = pmd_off_k(0xffff0000);

	/* allocate the zero page. */
	zero_page = early_alloc(PAGE_SIZE);

	bootmem_init();

	empty_zero_page = virt_to_page(zero_page);
	__flush_dcache_page(NULL, empty_zero_page);
}
