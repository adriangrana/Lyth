#include "mmap.h"
#include "string.h"

/*
 * Anonymous mmap for user processes.
 *
 * The user address space (4 MB, fully backed by physical memory) is shared
 * between the programme image, the user heap (grows up from programme break)
 * and mmap regions (allocated from the top of the free area, growing down).
 *
 * Pages are already physically mapped at process creation, so mmap only
 * needs to zero-fill and bookkeep.  munmap marks the region as free for
 * reuse by future mmap calls.
 */

static uintptr_t align_up_page(uintptr_t v) {
    return (v + MMAP_PAGE_SIZE - 1U) & ~(MMAP_PAGE_SIZE - 1U);
}

static uintptr_t align_down_page(uintptr_t v) {
    return v & ~(MMAP_PAGE_SIZE - 1U);
}

/* Check whether [base, base+len) overlaps any existing region */
static int overlaps_any(const mmap_region_t* regions, uintptr_t base, uintptr_t len) {
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        uintptr_t rend, nend;
        if (!regions[i].used)
            continue;
        rend = regions[i].base + regions[i].length;
        nend = base + len;
        if (base < rend && nend > regions[i].base)
            return 1;
    }
    return 0;
}

void mmap_init_regions(mmap_region_t* regions) {
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        regions[i].base   = 0;
        regions[i].length = 0;
        regions[i].used   = 0;
    }
}

uintptr_t mmap_anonymous(mmap_region_t* regions,
                        uintptr_t mmap_top, uintptr_t mmap_low,
                        uintptr_t length)
{
    int slot = -1;
    uintptr_t aligned_len;
    uintptr_t candidate;

    if (!regions || length == 0)
        return MMAP_FAILED;

    aligned_len = align_up_page(length);
    if (aligned_len == 0 || aligned_len > mmap_top - mmap_low)
        return MMAP_FAILED;

    /* Find a free slot */
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (!regions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return MMAP_FAILED;

    /*
     * Scan downward from mmap_top in page-size steps looking for a
     * contiguous block of aligned_len bytes that doesn't overlap any
     * existing mmap region.
     */
    candidate = align_down_page(mmap_top - aligned_len);
    while (candidate >= mmap_low) {
        if (!overlaps_any(regions, candidate, aligned_len)) {
            /* Found a gap – record it */
            regions[slot].base   = candidate;
            regions[slot].length = aligned_len;
            regions[slot].used   = 1;

            /* Zero the pages (memory is already physically mapped) */
            memset((void*)(uintptr_t)candidate, 0, aligned_len);
            return candidate;
        }
        if (candidate < MMAP_PAGE_SIZE)
            break;
        candidate -= MMAP_PAGE_SIZE;
    }

    return MMAP_FAILED;
}

int mmap_unmap(mmap_region_t* regions, uintptr_t addr, uintptr_t length) {
    uintptr_t aligned_len;
    (void)length; /* currently: match by base address only */

    if (!regions || addr == 0)
        return -1;

    aligned_len = align_up_page(length);

    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (regions[i].used && regions[i].base == addr) {
            /* Allow partial unmap only if length matches */
            if (aligned_len != 0 && aligned_len != regions[i].length)
                continue;
            regions[i].used = 0;
            return 0;
        }
    }
    return -1;
}

void mmap_clone_regions(const mmap_region_t* src, mmap_region_t* dst) {
    for (int i = 0; i < MMAP_MAX_REGIONS; i++)
        dst[i] = src[i];
}
