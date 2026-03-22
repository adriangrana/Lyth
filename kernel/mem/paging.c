#include "paging.h"
#include "physmem.h"

#define PAGE_DIRECTORY_ENTRIES 1024U
#define PAGE_SIZE_4MB 0x400000U
#define PAGE_TABLE_ENTRIES 1024U
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

static uint32_t align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1U);
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

static int paging_directory_page_is_user_accessible(uint32_t* directory, uint32_t address) {
    uint32_t pde_index;
    uint32_t pde;
    uint32_t* table;
    uint32_t pte_index;
    uint32_t pte;

    if (directory == 0) {
        return 0;
    }

    if (address < PAGING_USER_BASE || address >= (PAGING_USER_BASE + PAGING_USER_SIZE)) {
        return 0;
    }

    pde_index = address >> 22;
    pde = directory[pde_index];

    if ((pde & PAGE_PRESENT) == 0 || (pde & PAGE_USER) == 0) {
        return 0;
    }

    if ((pde & PAGE_PAGE_SIZE) != 0) {
        return 1;
    }

    table = (uint32_t*)(uintptr_t)(pde & 0xFFFFF000U);
    pte_index = (address >> 12) & 0x3FFU;
    pte = table[pte_index];

    return ((pte & PAGE_PRESENT) != 0 && (pte & PAGE_USER) != 0) ? 1 : 0;
}

int paging_directory_user_buffer_is_accessible(uint32_t* directory, uint32_t address, uint32_t size) {
    uint32_t cursor;
    uint32_t end;

    if (!paging_address_is_user_accessible(address, size)) {
        return 0;
    }

    end = address + size;
    cursor = align_down(address, PAGING_PAGE_SIZE);

    while (cursor < end) {
        if (!paging_directory_page_is_user_accessible(directory, cursor)) {
            return 0;
        }
        cursor += PAGING_PAGE_SIZE;
    }

    return 1;
}

int paging_user_buffer_is_accessible(const void* buffer, uint32_t size) {
    if (buffer == 0) {
        return 0;
    }

    return paging_directory_user_buffer_is_accessible(current_directory,
                                                      (uint32_t)(uintptr_t)buffer,
                                                      size);
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
    uint32_t table_physical = physmem_alloc_frame();
    uint32_t* directory;
    uint32_t* table;
    uint32_t page;

    if (directory_physical == 0 || table_physical == 0) {
        if (directory_physical != 0) {
            physmem_free_frame(directory_physical);
        }
        if (table_physical != 0) {
            physmem_free_frame(table_physical);
        }
        return 0;
    }

    directory = (uint32_t*)(uintptr_t)directory_physical;
    table = (uint32_t*)(uintptr_t)table_physical;

    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; i++) {
        directory[i] = page_directory[i];
    }

    for (uint32_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        table[i] = 0;
    }

    for (page = 0; page < PAGE_TABLE_ENTRIES; page++) {
        uint32_t va = PAGING_USER_BASE + (page * PAGING_PAGE_SIZE);
        uint32_t pa = user_physical_base + (page * PAGING_PAGE_SIZE);

        if (va == PAGING_USER_STACK_GUARD_BASE) {
            continue;
        }

        table[page] = pa | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    directory[PAGING_USER_BASE / PAGE_SIZE_4MB] =
        table_physical | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    return directory;
}

void paging_destroy_user_directory(uint32_t* directory) {
    uint32_t pde;

    if (directory == 0 || directory == page_directory) {
        return;
    }

    pde = directory[PAGING_USER_BASE / PAGE_SIZE_4MB];
    if ((pde & PAGE_PRESENT) != 0 && (pde & PAGE_PAGE_SIZE) == 0) {
        physmem_free_frame(pde & 0xFFFFF000U);
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