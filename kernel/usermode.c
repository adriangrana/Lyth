#include "usermode.h"
#include "elf.h"
#include "fs.h"
#include "heap.h"
#include "paging.h"
#include "physmem.h"
#include "task.h"
#include <stdint.h>

#define ELF_PROGRAM_TYPE_LOAD 1U

typedef struct {
    uint8_t magic[4];
    uint8_t elf_class;
    uint8_t data;
    uint8_t version;
    uint8_t osabi;
    uint8_t abi_version;
    uint8_t pad[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf32_header_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed)) elf32_program_header_t;

static void zero_memory(uint8_t* buffer, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = 0;
    }
}

static void copy_memory(uint8_t* dst, const uint8_t* src, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

int usermode_spawn_elf_task(const char* fs_name, int foreground) {
    unsigned int image_size;
    unsigned char* image;
    int read;
    elf_image_info_t info;
    const elf32_header_t* header;
    uint32_t user_physical_base = 0;
    uint32_t highest_user_end = paging_user_base();
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    uint32_t* page_directory = 0;
    int task_id;

    if (fs_name == 0) {
        return -1;
    }

    image_size = fs_size(fs_name);
    if (image_size == 0) {
        return -1;
    }

    image = (unsigned char*)kmalloc(image_size);
    if (image == 0) {
        return -1;
    }

    read = fs_read_bytes(fs_name, image, image_size);
    if (read < 0 || (unsigned int)read != image_size) {
        kfree(image);
        return -1;
    }

    if (!elf_parse_image(image, image_size, &info)) {
        kfree(image);
        return -1;
    }

    user_physical_base = physmem_alloc_region(paging_user_size(), paging_user_size());
    if (user_physical_base == 0) {
        kfree(image);
        return -1;
    }

    page_directory = paging_create_user_directory(user_physical_base);
    if (page_directory == 0) {
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    zero_memory((uint8_t*)(uintptr_t)user_physical_base, paging_user_size());

    header = (const elf32_header_t*)image;

    for (uint16_t index = 0; index < header->phnum; index++) {
        const elf32_program_header_t* program_header =
            (const elf32_program_header_t*)(image + header->phoff + (index * header->phentsize));

        if (program_header->type != ELF_PROGRAM_TYPE_LOAD) {
            continue;
        }

        if (!paging_address_is_user_accessible(program_header->vaddr, program_header->memsz) ||
            program_header->offset + program_header->filesz > image_size ||
            program_header->memsz < program_header->filesz) {
            paging_destroy_user_directory(page_directory);
            physmem_free_region(user_physical_base, paging_user_size());
            kfree(image);
            return -1;
        }

        if (program_header->vaddr + program_header->memsz > highest_user_end) {
            highest_user_end = program_header->vaddr + program_header->memsz;
        }

        zero_memory((uint8_t*)(uintptr_t)(user_physical_base + (program_header->vaddr - paging_user_base())),
                    program_header->memsz);
        copy_memory((uint8_t*)(uintptr_t)(user_physical_base + (program_header->vaddr - paging_user_base())),
                    image + program_header->offset,
                    program_header->filesz);
    }

    user_heap_base = align_up(highest_user_end, 16U);
    if (user_heap_base >= PAGING_USER_STACK_TOP) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
        kfree(image);
        return -1;
    }

    user_heap_size = PAGING_USER_STACK_TOP - user_heap_base;

    task_id = task_spawn_user(fs_name,
                              info.entry,
                              user_physical_base,
                              user_heap_base,
                              user_heap_size,
                              page_directory,
                              foreground);
    if (task_id < 0) {
        paging_destroy_user_directory(page_directory);
        physmem_free_region(user_physical_base, paging_user_size());
    }

    kfree(image);
    return task_id;
}