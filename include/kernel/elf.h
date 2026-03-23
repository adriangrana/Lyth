#ifndef ELF_H
#define ELF_H

#include <stdint.h>

typedef struct {
    uintptr_t entry;
    uint32_t program_header_offset;
    uint16_t program_header_count;
    uint16_t program_header_entry_size;
    uint16_t type;
    uint16_t machine;
    uint32_t loadable_segments;
} elf_image_info_t;

int elf_parse_image(const void* image, uint32_t size, elf_image_info_t* info);

#endif