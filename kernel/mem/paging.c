/* =================================================================
 *  Lyth OS — 64-bit 4-level paging (PML4 → PDPT → PD → PT)
 *
 *  Boot identity map: 4 GB with 2 MB pages (set up in boot64.s).
 *  User space: 4 KB pages via full PML4→PDPT→PD→PT chain.
 *  Per-process: own PML4/PDPT/PD for the first 1 GB; shared kernel PDs.
 * ================================================================= */

#include "paging.h"
#include "physmem.h"
#include <stddef.h>

/* ── Constants ──────────────────────────────────────────────────── */
#define ENTRIES_PER_TABLE 512U
#define PAGE_SIZE_2MB     0x200000UL

#define PG_PRESENT  0x001UL
#define PG_WRITABLE 0x002UL
#define PG_USER     0x004UL
#define PG_PS       0x080UL   /* 2 MB page */
#define PG_COW      0x200UL
#define PG_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Index extraction from virtual address */
#define PML4_IDX(va) (((va) >> 39) & 0x1FFU)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FFU)
#define PD_IDX(va)   (((va) >> 21) & 0x1FFU)
#define PT_IDX(va)   (((va) >> 12) & 0x1FFU)

/* User space spans PD entries 8..9 in the first 1 GB PD
   (PAGING_USER_BASE = 0x01000000, 0x01000000 >> 21 = 8) */
#define USER_PD_START  (PAGING_USER_BASE >> 21)
#define USER_PD_COUNT  2   /* 2 × 2 MB = 4 MB user region */

/* ── Boot page tables (set up in boot64.s) ──────────────────────── */
extern uint64_t boot_pml4[];

static uint64_t* kernel_pml4 = 0;
static uint64_t* current_pml4 = 0;
static int paging_enabled = 0;
static uintptr_t paging_bytes_mapped = 0;

/* ── Helpers ────────────────────────────────────────────────────── */

static void paging_invalidate_page(uintptr_t address) {
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

static uintptr_t align_down(uintptr_t value, uintptr_t alignment) {
    return value & ~(alignment - 1UL);
}

/* Access a physical frame as a uint64_t* (identity-mapped) */
static uint64_t* phys_to_virt(uint64_t phys) {
    return (uint64_t*)(uintptr_t)(phys & PG_ADDR_MASK);
}

/* Read a subtable pointer from a page table entry */
static uint64_t* entry_subtable(uint64_t entry) {
    if ((entry & PG_PRESENT) == 0) return 0;
    return phys_to_virt(entry);
}

/* ── Kernel PD pointers (derived from boot tables at init) ────── */
static uint64_t* kernel_pdpt = 0;   /* boot PDPT covering first 512 GB */
static uint64_t* kernel_pd0  = 0;   /* boot PD covering first 1 GB */

/* ── Initialisation ─────────────────────────────────────────────── */

void paging_init(multiboot_info_t* mbi) {
    (void)mbi;

    if (paging_enabled) return;

    kernel_pml4 = boot_pml4;
    current_pml4 = kernel_pml4;

    /* Derive sub-table pointers from boot PML4 */
    kernel_pdpt = entry_subtable(kernel_pml4[0]);
    if (kernel_pdpt)
        kernel_pd0 = entry_subtable(kernel_pdpt[0]);

    paging_enabled = 1;
    paging_bytes_mapped = 4UL * 1024 * 1024 * 1024;  /* 4 GB identity map */
}

int paging_is_enabled(void) { return paging_enabled; }
uintptr_t paging_mapped_bytes(void) { return paging_bytes_mapped; }
uintptr_t paging_user_base(void) { return PAGING_USER_BASE; }
uintptr_t paging_user_size(void) { return PAGING_USER_SIZE; }

/* ── Address validation ─────────────────────────────────────────── */

int paging_address_is_user_accessible(uintptr_t address, uintptr_t size) {
    uintptr_t end;
    if (size == 0) return 0;
    end = address + size;
    if (end < address) return 0;
    return address >= PAGING_USER_BASE && end <= (PAGING_USER_BASE + PAGING_USER_SIZE);
}

/* Walk PML4→PDPT→PD→PT for one 4 KB page */
static int page_is_user_accessible(uint64_t* pml4, uintptr_t address) {
    uint64_t *pdpt, *pd, *pt;
    uint64_t e;

    if (pml4 == 0) return 0;
    if (address < PAGING_USER_BASE || address >= (PAGING_USER_BASE + PAGING_USER_SIZE))
        return 0;

    e = pml4[PML4_IDX(address)];
    if ((e & PG_PRESENT) == 0 || (e & PG_USER) == 0) return 0;
    pdpt = phys_to_virt(e);

    e = pdpt[PDPT_IDX(address)];
    if ((e & PG_PRESENT) == 0 || (e & PG_USER) == 0) return 0;
    pd = phys_to_virt(e);

    e = pd[PD_IDX(address)];
    if ((e & PG_PRESENT) == 0 || (e & PG_USER) == 0) return 0;
    if (e & PG_PS) return 1;  /* 2 MB page */
    pt = phys_to_virt(e);

    e = pt[PT_IDX(address)];
    return ((e & PG_PRESENT) != 0 && (e & PG_USER) != 0) ? 1 : 0;
}

int paging_directory_user_buffer_is_accessible(uint64_t* pml4, uintptr_t address, uintptr_t size) {
    uintptr_t cursor, end;
    if (!paging_address_is_user_accessible(address, size)) return 0;
    end = address + size;
    cursor = align_down(address, PAGING_PAGE_SIZE);
    while (cursor < end) {
        if (!page_is_user_accessible(pml4, cursor)) return 0;
        cursor += PAGING_PAGE_SIZE;
    }
    return 1;
}

int paging_user_buffer_is_accessible(const void* buffer, uintptr_t size) {
    if (buffer == 0) return 0;
    return paging_directory_user_buffer_is_accessible(current_pml4,
        (uintptr_t)buffer, size);
}

int paging_user_string_is_accessible(const char* text, uintptr_t max_length) {
    uintptr_t address;
    if (text == 0 || max_length == 0) return 0;
    address = (uintptr_t)text;
    if (!paging_address_is_user_accessible(address, 1)) return 0;
    for (uintptr_t i = 0; i < max_length; i++) {
        if (!paging_address_is_user_accessible(address + i, 1)) return 0;
        if (text[i] == '\0') return 1;
    }
    return 0;
}

/* ── Kernel directory ───────────────────────────────────────────── */

uint64_t* paging_kernel_directory(void) { return kernel_pml4; }

/* ── User page-table navigation helpers ─────────────────────────── *
 * Walk the per-process PML4→PDPT→PD to locate the PT(s) for user space.
 * Returns the PT covering the 2 MB region containing `va`, or 0.
 */
static uint64_t* user_pt_for(uint64_t* pml4, uintptr_t va) {
    uint64_t *pdpt, *pd;
    uint64_t e;

    if (pml4 == 0) return 0;
    e = pml4[PML4_IDX(va)];
    if ((e & PG_PRESENT) == 0) return 0;
    pdpt = phys_to_virt(e);

    e = pdpt[PDPT_IDX(va)];
    if ((e & PG_PRESENT) == 0) return 0;
    pd = phys_to_virt(e);

    e = pd[PD_IDX(va)];
    if ((e & PG_PRESENT) == 0 || (e & PG_PS) != 0) return 0;
    return phys_to_virt(e);
}

/* ── Create user-space page tables ──────────────────────────────── */

uint64_t* paging_create_user_directory(uintptr_t user_physical_base) {
    uint32_t pml4_phys, pdpt_phys, pd_phys, pt0_phys, pt1_phys;
    uint64_t *new_pml4, *new_pdpt, *new_pd, *pt0, *pt1;

    if (kernel_pml4 == 0 || kernel_pdpt == 0 || kernel_pd0 == 0)
        return 0;

    /* Allocate 5 frames: PML4, PDPT, PD, PT0, PT1 */
    pml4_phys = physmem_alloc_frame();
    pdpt_phys = physmem_alloc_frame();
    pd_phys   = physmem_alloc_frame();
    pt0_phys  = physmem_alloc_frame();
    pt1_phys  = physmem_alloc_frame();

    if (pml4_phys == 0 || pdpt_phys == 0 || pd_phys == 0 ||
        pt0_phys == 0 || pt1_phys == 0) {
        if (pml4_phys) physmem_free_frame(pml4_phys);
        if (pdpt_phys) physmem_free_frame(pdpt_phys);
        if (pd_phys)   physmem_free_frame(pd_phys);
        if (pt0_phys)  physmem_free_frame(pt0_phys);
        if (pt1_phys)  physmem_free_frame(pt1_phys);
        return 0;
    }

    new_pml4 = (uint64_t*)(uintptr_t)pml4_phys;
    new_pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
    new_pd   = (uint64_t*)(uintptr_t)pd_phys;
    pt0      = (uint64_t*)(uintptr_t)pt0_phys;
    pt1      = (uint64_t*)(uintptr_t)pt1_phys;

    /* 1. PML4: copy kernel entries, override [0] → new PDPT */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++)
        new_pml4[i] = kernel_pml4[i];
    new_pml4[0] = (uint64_t)pdpt_phys | PG_PRESENT | PG_WRITABLE | PG_USER;

    /* 2. PDPT: copy kernel entries, override [0] → new PD */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++)
        new_pdpt[i] = kernel_pdpt[i];
    new_pdpt[0] = (uint64_t)pd_phys | PG_PRESENT | PG_WRITABLE | PG_USER;

    /* 3. PD: copy kernel 2 MB pages, override user entries → PTs */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++)
        new_pd[i] = kernel_pd0[i];
    new_pd[USER_PD_START]     = (uint64_t)pt0_phys | PG_PRESENT | PG_WRITABLE | PG_USER;
    new_pd[USER_PD_START + 1] = (uint64_t)pt1_phys | PG_PRESENT | PG_WRITABLE | PG_USER;

    /* 4. Fill PTs with user physical pages */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++) pt0[i] = 0;
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++) pt1[i] = 0;

    for (unsigned page = 0; page < (PAGING_USER_SIZE / PAGING_PAGE_SIZE); page++) {
        uintptr_t va = PAGING_USER_BASE + (page * PAGING_PAGE_SIZE);
        uintptr_t pa = user_physical_base + (page * PAGING_PAGE_SIZE);

        if (va == PAGING_USER_STACK_GUARD_BASE) continue;

        unsigned pt_page = page;
        uint64_t* pt = pt0;
        if (pt_page >= ENTRIES_PER_TABLE) {
            pt = pt1;
            pt_page -= ENTRIES_PER_TABLE;
        }
        pt[pt_page] = (uint64_t)pa | PG_PRESENT | PG_WRITABLE | PG_USER;
    }

    return new_pml4;
}

/* ── Map / unmap / lookup single user pages ─────────────────────── */

int paging_map_user_page(uint64_t* pml4, uintptr_t virtual_address,
                         uintptr_t physical_address, int writable) {
    uint64_t* pt;
    uint64_t flags;

    if (pml4 == 0 ||
        virtual_address < PAGING_USER_BASE ||
        virtual_address >= PAGING_USER_BASE + PAGING_USER_SIZE ||
        (virtual_address % PAGING_PAGE_SIZE) != 0 ||
        physical_address == 0 ||
        (physical_address % PAGING_PAGE_SIZE) != 0)
        return 0;

    pt = user_pt_for(pml4, virtual_address);
    if (pt == 0) return 0;

    flags = PG_PRESENT | PG_USER | (writable ? PG_WRITABLE : 0UL);
    pt[PT_IDX(virtual_address)] = (uint64_t)physical_address | flags;

    if (current_pml4 == pml4)
        paging_invalidate_page(virtual_address);
    return 1;
}

int paging_unmap_user_page(uint64_t* pml4, uintptr_t virtual_address) {
    uint64_t* pt;

    if (pml4 == 0 ||
        virtual_address < PAGING_USER_BASE ||
        virtual_address >= PAGING_USER_BASE + PAGING_USER_SIZE ||
        (virtual_address % PAGING_PAGE_SIZE) != 0)
        return 0;

    pt = user_pt_for(pml4, virtual_address);
    if (pt == 0) return 0;

    pt[PT_IDX(virtual_address)] = 0;
    if (current_pml4 == pml4)
        paging_invalidate_page(virtual_address);
    return 1;
}

uintptr_t paging_lookup_user_page(uint64_t* pml4, uintptr_t virtual_address) {
    uint64_t* pt;
    uint64_t pte;

    if (pml4 == 0 ||
        virtual_address < PAGING_USER_BASE ||
        virtual_address >= PAGING_USER_BASE + PAGING_USER_SIZE)
        return 0;

    pt = user_pt_for(pml4, virtual_address);
    if (pt == 0) return 0;

    pte = pt[PT_IDX(virtual_address)];
    if ((pte & PG_PRESENT) == 0) return 0;
    return (uintptr_t)(pte & PG_ADDR_MASK);
}

/* ── Destroy / switch ───────────────────────────────────────────── */

static void free_user_pts(uint64_t* pml4) {
    /* Free the per-process PTs, PD, PDPT (not the kernel's) */
    uint64_t *pdpt, *pd;
    uint64_t e;

    e = pml4[0];
    if ((e & PG_PRESENT) == 0) return;
    pdpt = phys_to_virt(e);

    e = pdpt[0];
    if ((e & PG_PRESENT) == 0) goto free_pdpt;
    pd = phys_to_virt(e);

    /* Free user PTs */
    for (int i = 0; i < USER_PD_COUNT; i++) {
        uint64_t pde = pd[USER_PD_START + i];
        if ((pde & PG_PRESENT) != 0 && (pde & PG_PS) == 0)
            physmem_free_frame((uint32_t)(pde & PG_ADDR_MASK));
    }
    physmem_free_frame((uint32_t)(pdpt[0] & PG_ADDR_MASK));  /* PD */
free_pdpt:
    physmem_free_frame((uint32_t)(pml4[0] & PG_ADDR_MASK));  /* PDPT */
}

void paging_destroy_user_directory(uint64_t* pml4) {
    if (pml4 == 0 || pml4 == kernel_pml4) return;
    free_user_pts(pml4);
    physmem_free_frame((uint32_t)(uintptr_t)pml4);
}

void paging_switch_directory(uint64_t* pml4) {
    if (pml4 == 0) pml4 = kernel_pml4;
    if (current_pml4 == pml4) return;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
    current_pml4 = pml4;
}

/* ── MMIO mapping ───────────────────────────────────────────────── */

int paging_map_mmio(uintptr_t physical_address) {
    /* In our 4 GB identity map, all MMIO below 4 GB is already mapped */
    if (physical_address < paging_bytes_mapped)
        return 1;
    /* TODO: for addresses > 4 GB, build new page table entries */
    return 0;
}

/* ── COW support ────────────────────────────────────────────────── */

uint64_t* paging_cow_clone_user_directory(uint64_t* parent_pml4) {
    uint32_t pml4_phys, pdpt_phys, pd_phys;
    uint64_t *new_pml4, *new_pdpt, *new_pd;
    uint64_t *parent_pdpt, *parent_pd;

    if (parent_pml4 == 0 || kernel_pml4 == 0)
        return 0;

    /* Get parent's sub-tables */
    parent_pdpt = entry_subtable(parent_pml4[0]);
    if (parent_pdpt == 0) return 0;
    parent_pd = entry_subtable(parent_pdpt[0]);
    if (parent_pd == 0) return 0;

    /* Allocate PML4 + PDPT + PD (PTs are shared via COW) */
    pml4_phys = physmem_alloc_frame();
    pdpt_phys = physmem_alloc_frame();
    pd_phys   = physmem_alloc_frame();
    if (pml4_phys == 0 || pdpt_phys == 0 || pd_phys == 0) {
        if (pml4_phys) physmem_free_frame(pml4_phys);
        if (pdpt_phys) physmem_free_frame(pdpt_phys);
        if (pd_phys)   physmem_free_frame(pd_phys);
        return 0;
    }

    new_pml4 = (uint64_t*)(uintptr_t)pml4_phys;
    new_pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
    new_pd   = (uint64_t*)(uintptr_t)pd_phys;

    /* Clone PML4 */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++)
        new_pml4[i] = parent_pml4[i];
    new_pml4[0] = (uint64_t)pdpt_phys | PG_PRESENT | PG_WRITABLE | PG_USER;

    /* Clone PDPT */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++)
        new_pdpt[i] = parent_pdpt[i];
    new_pdpt[0] = (uint64_t)pd_phys | PG_PRESENT | PG_WRITABLE | PG_USER;

    /* Clone PD, allocate new PTs for user region with COW entries */
    for (unsigned i = 0; i < ENTRIES_PER_TABLE; i++)
        new_pd[i] = parent_pd[i];

    for (int ti = 0; ti < USER_PD_COUNT; ti++) {
        uint64_t parent_pde = parent_pd[USER_PD_START + ti];
        uint64_t* parent_pt;
        uint64_t* child_pt;
        uint32_t child_pt_phys;

        if ((parent_pde & PG_PRESENT) == 0 || (parent_pde & PG_PS) != 0) {
            new_pd[USER_PD_START + ti] = 0;
            continue;
        }

        parent_pt = phys_to_virt(parent_pde);
        child_pt_phys = physmem_alloc_frame();
        if (child_pt_phys == 0) {
            /* Cleanup on failure: free everything allocated so far */
            for (int j = 0; j < ti; j++) {
                uint64_t e = new_pd[USER_PD_START + j];
                if (e & PG_PRESENT) physmem_free_frame((uint32_t)(e & PG_ADDR_MASK));
            }
            physmem_free_frame(pd_phys);
            physmem_free_frame(pdpt_phys);
            physmem_free_frame(pml4_phys);
            return 0;
        }
        child_pt = (uint64_t*)(uintptr_t)child_pt_phys;

        for (unsigned p = 0; p < ENTRIES_PER_TABLE; p++) {
            uint64_t pte = parent_pt[p];
            if ((pte & PG_PRESENT) == 0) {
                child_pt[p] = 0;
                continue;
            }

            /* Skip SHM pages (don't COW) */
            uintptr_t va = PAGING_USER_BASE + ((unsigned)(ti * ENTRIES_PER_TABLE) + p) * PAGING_PAGE_SIZE;
            if (va >= PAGING_USER_SHM_BASE && va < PAGING_USER_STACK_GUARD_BASE) {
                child_pt[p] = 0;
                continue;
            }

            uint64_t phys = pte & PG_ADDR_MASK;
            uint64_t cow_flags = phys | PG_PRESENT | PG_USER | PG_COW;
            parent_pt[p] = cow_flags;
            child_pt[p]  = cow_flags;
            physmem_ref_frame((uint32_t)phys);

            if (current_pml4 == parent_pml4)
                paging_invalidate_page(va);
        }

        new_pd[USER_PD_START + ti] = (uint64_t)child_pt_phys | PG_PRESENT | PG_WRITABLE | PG_USER;
    }

    return new_pml4;
}

void paging_release_user_pages(uint64_t* pml4) {
    if (pml4 == 0) return;

    for (int ti = 0; ti < USER_PD_COUNT; ti++) {
        uint64_t* pt = user_pt_for(pml4, PAGING_USER_BASE + (uintptr_t)ti * ENTRIES_PER_TABLE * PAGING_PAGE_SIZE);
        if (pt == 0) continue;

        for (unsigned p = 0; p < ENTRIES_PER_TABLE; p++) {
            uint64_t pte = pt[p];
            if ((pte & PG_PRESENT) == 0) continue;
            physmem_unref_frame((uint32_t)(pte & PG_ADDR_MASK));
            pt[p] = 0;
        }
    }
}

int paging_cow_resolve(uint64_t* pml4, uintptr_t fault_address) {
    uint64_t* pt;
    uintptr_t page_addr;
    uint64_t pte;
    uint32_t old_phys, new_phys;

    if (pml4 == 0) return 0;

    page_addr = fault_address & ~(uintptr_t)(PAGING_PAGE_SIZE - 1);
    if (page_addr < PAGING_USER_BASE ||
        page_addr >= PAGING_USER_BASE + PAGING_USER_SIZE)
        return 0;

    pt = user_pt_for(pml4, page_addr);
    if (pt == 0) return 0;

    pte = pt[PT_IDX(page_addr)];
    if ((pte & PG_PRESENT) == 0 || (pte & PG_COW) == 0)
        return 0;

    old_phys = (uint32_t)(pte & PG_ADDR_MASK);

    /* If we're the sole owner, just flip to writable */
    if (physmem_frame_refcount(old_phys) == 1) {
        pt[PT_IDX(page_addr)] = (uint64_t)old_phys | PG_PRESENT | PG_WRITABLE | PG_USER;
        paging_invalidate_page(page_addr);
        return 1;
    }

    /* Allocate new frame and copy */
    new_phys = physmem_alloc_frame();
    if (new_phys == 0) return 0;

    uint64_t* src = (uint64_t*)(uintptr_t)old_phys;
    uint64_t* dst = (uint64_t*)(uintptr_t)new_phys;
    for (unsigned i = 0; i < PAGING_PAGE_SIZE / sizeof(uint64_t); i++)
        dst[i] = src[i];

    physmem_unref_frame(old_phys);
    pt[PT_IDX(page_addr)] = (uint64_t)new_phys | PG_PRESENT | PG_WRITABLE | PG_USER;
    paging_invalidate_page(page_addr);
    return 1;
}
