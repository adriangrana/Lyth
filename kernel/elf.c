#include "elf.h"

#define ELF_MAGIC_0 0x7F
#define ELF_MAGIC_1 'E'
#define ELF_MAGIC_2 'L'
#define ELF_MAGIC_3 'F'
#define ELF_CLASS_32 1
#define ELF_DATA_LSB 1
#define ELF_VERSION_CURRENT 1
#define ELF_MACHINE_I386 3
#define ELF_TYPE_EXEC 2
#define ELF_PROGRAM_TYPE_LOAD 1

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

int elf_parse_image(const void* image, uint32_t size, elf_image_info_t* info) {
    const elf32_header_t* header;
    uint32_t loadable_segments = 0;

    if (image == 0 || info == 0 || size < sizeof(elf32_header_t)) {
        return 0;
    }

    header = (const elf32_header_t*)image;

    if (header->magic[0] != ELF_MAGIC_0 ||
        header->magic[1] != ELF_MAGIC_1 ||
        header->magic[2] != ELF_MAGIC_2 ||
        header->magic[3] != ELF_MAGIC_3) {
        return 0;
    }

    if (header->elf_class != ELF_CLASS_32 ||
        header->data != ELF_DATA_LSB ||
        header->version != ELF_VERSION_CURRENT ||
        header->version2 != ELF_VERSION_CURRENT ||
        header->machine != ELF_MACHINE_I386 ||
        header->type != ELF_TYPE_EXEC) {
        return 0;
    }

    if (header->ehsize != sizeof(elf32_header_t)) {
        return 0;
    }

    if (header->phnum > 0) {
        uint32_t ph_end;

        if (header->phentsize != sizeof(elf32_program_header_t)) {
            return 0;
        }

        ph_end = header->phoff + (header->phnum * header->phentsize);
        if (ph_end > size || ph_end < header->phoff) {
            return 0;
        }

        for (uint16_t index = 0; index < header->phnum; index++) {
            const elf32_program_header_t* program_header =
                (const elf32_program_header_t*)((const uint8_t*)image +
                                                header->phoff +
                                                (index * header->phentsize));

            if (program_header->type == ELF_PROGRAM_TYPE_LOAD) {
                uint32_t segment_end = program_header->offset + program_header->filesz;

                if (segment_end > size || segment_end < program_header->offset) {
                    return 0;
                }

                if (program_header->memsz < program_header->filesz) {
                    return 0;
                }

                loadable_segments++;
            }
        }
    }

    info->entry = header->entry;
    info->program_header_offset = header->phoff;
    info->program_header_count = header->phnum;
    info->program_header_entry_size = header->phentsize;
    info->type = header->type;
    info->machine = header->machine;
    info->loadable_segments = loadable_segments;
    return 1;
}