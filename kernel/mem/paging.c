#include "paging.h"
#include "physmem.h"

#define PAGE_DIRECTORY_ENTRIES 1024U
#define PAGE_SIZE_4MB 0x400000U
#define PAGE_PRESENT 0x001U
#define PAGE_WRITABLE 0x002U
#define PAGE_PAGE_SIZE 0x080U

extern char __kernel_end;

static uint32_t page_directory[PAGE_DIRECTORY_ENTRIES] __attribute__((aligned(4096)));
static int paging_enabled = 0;
static uint32_t paging_bytes_mapped = 0;

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t max_uint32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static uint32_t detect_framebuffer_end(multiboot_info_t* mbi) {
    if (mbi == 0) {
        return 0;
    }

    if ((mbi->flags & (1U << 12)) == 0 || mbi->framebuffer_addr == 0 ||
        mbi->framebuffer_pitch == 0 || mbi->framebuffer_height == 0) {
        return 0;
    }

    return (uint32_t)mbi->framebuffer_addr + (mbi->framebuffer_pitch * mbi->framebuffer_height);
}

void paging_init(multiboot_info_t* mbi) {
    uint32_t highest_needed;
    uint32_t mapped_limit;
    uint32_t directory_count;

    if (paging_enabled) {
        return;
    }

    highest_needed = physmem_highest_address();
    highest_needed = max_uint32(highest_needed, (uint32_t)(uintptr_t)&__kernel_end);
    highest_needed = max_uint32(highest_needed, detect_framebuffer_end(mbi));

    if (highest_needed < PAGE_SIZE_4MB) {
        highest_needed = PAGE_SIZE_4MB;
    }

    mapped_limit = align_up(highest_needed, PAGE_SIZE_4MB);
    directory_count = mapped_limit / PAGE_SIZE_4MB;

    if (directory_count > PAGE_DIRECTORY_ENTRIES) {
        directory_count = PAGE_DIRECTORY_ENTRIES;
        mapped_limit = PAGE_DIRECTORY_ENTRIES * PAGE_SIZE_4MB;
    }

    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; i++) {
        page_directory[i] = 0;
    }

    for (uint32_t i = 0; i < directory_count; i++) {
        page_directory[i] = (i * PAGE_SIZE_4MB) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_PAGE_SIZE;
    }

    __asm__ volatile (
        "mov %%cr4, %%eax\n"
        "or $0x10, %%eax\n"
        "mov %%eax, %%cr4\n"
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        :
        : "r"(page_directory)
        : "eax", "memory"
    );

    paging_enabled = 1;
    paging_bytes_mapped = mapped_limit;
}

int paging_is_enabled(void) {
    return paging_enabled;
}

uint32_t paging_mapped_bytes(void) {
    return paging_bytes_mapped;
}