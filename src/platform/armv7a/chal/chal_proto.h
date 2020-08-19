/* Architecture-specific prototypes that go into the kernel */
#ifndef CHAL_PROTO_H
#define CHAL_PROTO_H

/* Page table platform-dependent definitions - for ARM, some of the x86 page flags are not useful */
#define PGTBL_PGTIDX_SHIFT (20)
#define PGTBL_PAGEIDX_SHIFT (12)
#define PGTBL_FRAME_BITS (32 - PGTBL_PAGEIDX_SHIFT)
#define PGTBL_FLAG_MASK ((1 << PGTBL_PAGEIDX_SHIFT) - 1)
#define PGTBL_FRAME_MASK (~PGTBL_FLAG_MASK)
#define PGTBL_DEPTH 2
#define PGTBL_ENTRY_ORDER 8
#define PGTBL_ENTRY (1 << PGTBL_ENTRY_ORDER)
#define SUPER_PAGE_FLAG_MASK  (0x3FFFFF)
#define SUPER_PAGE_PTE_MASK   (0x3FF000)

/* FIXME:find a better way to do this */
#define EXTRACT_SUB_PAGE(super) ((super) & SUPER_PAGE_PTE_MASK)

struct pgtbl
{
	u32_t 		pgtbl[1024];
};

/* Page table related prototypes & structs */
/* make it an opaque type...not to be touched */
typedef struct pgtbl *pgtbl_t;

struct pgtbl_info {
	asid_t  asid;
	pgtbl_t pgtbl;
} __attribute__((packed));

/* identical to the capability structure */
struct cap_pgtbl {
	struct cap_header h;
	u32_t             refcnt_flags; /* includes refcnt and flags */
	pgtbl_t           pgtbl;
	u32_t             lvl;       /* what level are the pgtbl nodes at? */
	struct cap_pgtbl *parent;    /* if !null, points to parent cap */
	u64_t             frozen_ts; /* timestamp when frozen is set. */
} __attribute__((packed));

static inline void
chal_pgtbl_update(struct pgtbl_info *ptinfo)
{
	paddr_t ttbr0 = chal_va2pa(ptinfo->pgtbl) | 0x4a;

	//asm volatile("mcr p15, 0, %0, c2, c0, 0" :: "r" (ttbr0));
	//asm volatile("mcr p15, 0, r0, c8, c7, 0"); /* TLBIALL */

	asm volatile("mcr p15, 0, %0, c13, c0, 1" :: "r" (0));
	asm volatile("isb");
	asm volatile("mcr p15, 0, %0, c2, c0, 0" :: "r" (ttbr0));
	asm volatile("isb");
	/* NOTE: PROCID unused */
	asm volatile("mcr p15, 0, %0, c13, c0, 1" :: "r" (ptinfo->asid));
}

extern asid_t free_asid;
static inline asid_t
chal_asid_alloc(void)
{
	if (unlikely(free_asid >= MAX_NUM_ASID)) assert(0);
	return cos_faa((int *)&free_asid, 1);
}

#endif /* CHAL_PROTO_H */