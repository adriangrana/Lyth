#include "paging.h"
#include "physmem.h"

#define PAGE_DIRECTORY_ENTRIES 1024U
#define PAGE_SIZE_4MB 0x400000U
#define PAGE_PRESENT 0x001U
#define PAGE_WRITABLE 0x002U
#define PAGE_USER 0x004U
#define PAGE_PAGE_SIZE 0x080U

extern char __kernel_end;

static uint32_t page_directory[PAGE_DIRECTORY_ENTRIES] __attribute__((aligned(4096)));
static uint32_t* current_directory = page_directory;
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
        uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_PAGE_SIZE;

        page_directory[i] = (i * PAGE_SIZE_4MB) | flags;
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
    current_directory = page_directory;
}

int paging_is_enabled(void) {
    return paging_enabled;
}

uint32_t paging_mapped_bytes(void) {
    return paging_bytes_mapped;
}

uint32_t paging_user_base(void) {
    return PAGING_USER_BASE;
}

uint32_t paging_user_size(void) {
    return PAGING_USER_SIZE;
}

int paging_address_is_user_accessible(uint32_t address, uint32_t size) {
    uint32_t end;

    if (size == 0) {
        return 0;
    }

    end = address + size;
    if (end < address) {
        return 0;
    }

    return address >= PAGING_USER_BASE && end <= (PAGING_USER_BASE + PAGING_USER_SIZE);
}

int paging_user_buffer_is_accessible(const void* buffer, uint32_t size) {
    if (buffer == 0) {
        return 0;
    }

    return paging_address_is_user_accessible((uint32_t)(uintptr_t)buffer, size);
}

int paging_user_string_is_accessible(const char* text, uint32_t max_length) {
    uint32_t address;

    if (text == 0 || max_length == 0) {
        return 0;
    }

    address = (uint32_t)(uintptr_t)text;
    if (!paging_address_is_user_accessible(address, 1)) {
        return 0;
    }

    for (uint32_t i = 0; i < max_length; i++) {
        if (!paging_address_is_user_accessible(address + i, 1)) {
            return 0;
        }

        if (text[i] == '\0') {
            return 1;
        }
    }

    return 0;
}

uint32_t* paging_kernel_directory(void) {
    return page_directory;
}

uint32_t* paging_create_user_directory(uint32_t user_physical_base) {
    uint32_t directory_physical = physmem_alloc_frame();
    uint32_t* directory;

    if (directory_physical == 0) {
        return 0;
    }

    directory = (uint32_t*)(uintptr_t)directory_physical;

    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; i++) {
        directory[i] = page_directory[i];
    }

    directory[PAGING_USER_BASE / PAGE_SIZE_4MB] =
        user_physical_base | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_PAGE_SIZE;

    return directory;
}

void paging_destroy_user_directory(uint32_t* directory) {
    if (directory == 0 || directory == page_directory) {
        return;
    }

    physmem_free_frame((uint32_t)(uintptr_t)directory);
}

void paging_switch_directory(uint32_t* directory) {
    if (directory == 0) {
        directory = page_directory;
    }

    if (current_directory == directory) {
        return;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(directory) : "memory");
    current_directory = directory;
}