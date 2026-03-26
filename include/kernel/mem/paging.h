#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "multiboot.h"

#define PAGING_PAGE_SIZE 0x1000U
#define PAGING_USER_BASE 0x01000000UL
#define PAGING_USER_SIZE 0x00400000UL
#define PAGING_USER_STACK_SIZE 0x1000UL
#define PAGING_USER_STACK_GUARD_SIZE PAGING_PAGE_SIZE
#define PAGING_USER_SHM_SIZE (16UL * PAGING_PAGE_SIZE)
#define PAGING_USER_STACK_GUARD_BASE \
	(PAGING_USER_BASE + PAGING_USER_SIZE - PAGING_USER_STACK_SIZE - PAGING_USER_STACK_GUARD_SIZE)
#define PAGING_USER_SHM_BASE (PAGING_USER_STACK_GUARD_BASE - PAGING_USER_SHM_SIZE)
#define PAGING_USER_STACK_BOTTOM (PAGING_USER_BASE + PAGING_USER_SIZE - PAGING_USER_STACK_SIZE)
#define PAGING_USER_STACK_TOP (PAGING_USER_BASE + PAGING_USER_SIZE)
#define PAGING_USER_SIGNAL_TRAMPOLINE_SIZE 16U
#define PAGING_PAGE_COW 0x200UL

void paging_init(multiboot_info_t* mbi);
int paging_is_enabled(void);

/* Map a physical region into the boot identity-map page tables using 2 MB
 * pages.  Safe to call before paging_init() / physmem_init() because it
 * uses a static pool of pre-allocated PDs reserved in boot64.s.
 * Intended for the framebuffer which may reside above the initial 4 GB map. */
void paging_map_region_early(uint64_t phys_start, uint64_t size);

/* Same as paging_map_region_early but maps with Write-Combining (WC) caching
 * via PAT entry 1.  Use for the framebuffer to get ~5-10x faster writes. */
void paging_map_region_early_wc(uint64_t phys_start, uint64_t size);

uintptr_t paging_mapped_bytes(void);
uintptr_t paging_user_base(void);
uintptr_t paging_user_size(void);
int paging_address_is_user_accessible(uintptr_t address, uintptr_t size);
int paging_user_buffer_is_accessible(const void* buffer, uintptr_t size);
int paging_user_string_is_accessible(const char* text, uintptr_t max_length);
int paging_directory_user_buffer_is_accessible(uint64_t* pml4, uintptr_t address, uintptr_t size);
uint64_t* paging_kernel_directory(void);
uint64_t* paging_create_user_directory(uintptr_t user_physical_base);
int paging_map_user_page(uint64_t* pml4, uintptr_t virtual_address,
						 uintptr_t physical_address, int writable);
int paging_unmap_user_page(uint64_t* pml4, uintptr_t virtual_address);
uintptr_t paging_lookup_user_page(uint64_t* pml4, uintptr_t virtual_address);
void paging_destroy_user_directory(uint64_t* pml4);
void paging_switch_directory(uint64_t* pml4);

/* MMIO mapping */
int paging_map_mmio(uintptr_t physical_address);

/* COW support */
uint64_t* paging_cow_clone_user_directory(uint64_t* parent_pml4);
void paging_release_user_pages(uint64_t* pml4);
int paging_cow_resolve(uint64_t* pml4, uintptr_t fault_address);

#endif