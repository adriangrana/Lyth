#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>

#define MMAP_MAX_REGIONS  16
#define MMAP_PAGE_SIZE    4096U

/* Flags (subset of POSIX) */
#define MMAP_PROT_READ    0x1U
#define MMAP_PROT_WRITE   0x2U
#define MMAP_PROT_EXEC    0x4U
#define MMAP_PROT_NONE    0x0U

#define MMAP_MAP_PRIVATE   0x02U
#define MMAP_MAP_ANONYMOUS 0x20U
#define MMAP_MAP_FIXED     0x10U

#define MMAP_FAILED ((uint32_t)0)

typedef struct {
    uintptr_t base;     /* page-aligned virtual address */
    uintptr_t length;   /* length in bytes (page-aligned) */
    int      used;
} mmap_region_t;

/* Initialise per-task VMA table */
void mmap_init_regions(mmap_region_t* regions);

/*
 * Allocate an anonymous mapping.
 *   regions   – the task's VMA array (MMAP_MAX_REGIONS entries)
 *   mmap_top  – highest usable VA for mmap (exclusive, page-aligned)
 *   mmap_low  – lowest usable VA for mmap (inclusive, page-aligned)
 *   length    – requested size (will be rounded up to page boundary)
 *
 * Returns the base VA of the new mapping, or MMAP_FAILED.
 */
uintptr_t mmap_anonymous(mmap_region_t* regions,
                        uintptr_t mmap_top, uintptr_t mmap_low,
                        uintptr_t length);

/*
 * Release a mapping.  Returns 0 on success, -1 if not found.
 */
int mmap_unmap(mmap_region_t* regions, uintptr_t addr, uintptr_t length);

/*
 * Copy parent VMA table to child (used by fork).
 */
void mmap_clone_regions(const mmap_region_t* src, mmap_region_t* dst);

#endif
