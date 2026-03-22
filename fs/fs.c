#include "fs.h"
#include "string.h"
#include "kernel/mem/heap.h"

#define FS_WRITE_MAX 32
#define FS_WRITE_NAME_MAX 64

typedef struct {
    int used;
    char* name;
    unsigned char* data;
    unsigned int size;
} fs_writable_entry_t;

static fs_writable_entry_t writable[FS_WRITE_MAX];

static const unsigned char motd_text[] =
    "Bienvenido a Lyth OS v0.4.\n";

static const unsigned char version_text[] =
    "NAME=Lyth\n"
    "VERSION=0.4\n"
    "ID=lyth\n"
    "PRETTY_NAME=Lyth OS 0.4\n"
    "ARCH=i386\n";

static const unsigned char demo_script_text[] =
    "# demo.sh - script de ejemplo para la shell de Lyth\n"
    "echo Ejecutando script de ejemplo...\n"
    "env\n"
    "ls\n";

static const unsigned char demo_elf[] = {
    0x7F, 0x45, 0x4C, 0x46, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x34, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x34, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x54, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,
    0x3A, 0x00, 0x00, 0x00,
    0x3A, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00,
    0x66, 0xB8, 0x23, 0x00,
    0x8E, 0xD8,
    0x8E, 0xC0,
    0x8E, 0xE0,
    0x8E, 0xE8,
    0xBB, 0x23, 0x00, 0x00, 0x01,
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0x31, 0xDB,
    0xB8, 0x0B, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xEB, 0xFE,
    'H', 'o', 'l', 'a', ' ', 'd', 'e', 's', 'd', 'e', ' ', 'u',
    's', 'e', 'r', ' ', 'm', 'o', 'd', 'e', '!', '\n', 0x00
};

typedef struct {
    const char* name;
    const unsigned char* contents;
    unsigned int size;
} fs_entry_t;

static fs_entry_t files[] = {
    {"etc/motd",          motd_text,        sizeof(motd_text) - 1},
    {"etc/os-release",    version_text,     sizeof(version_text) - 1},
    {"home/user/demo.sh", demo_script_text, sizeof(demo_script_text) - 1},
    {"home/user/demo",    demo_elf,         sizeof(demo_elf)},
};

static const int file_count = sizeof(files) / sizeof(files[0]);

void fs_init(void) {
    for (int i = 0; i < FS_WRITE_MAX; i++) {
        writable[i].used = 0;
        writable[i].name = 0;
        writable[i].data = 0;
        writable[i].size = 0;
    }
}

int fs_count(void) {
    int count = file_count;
    for (int i = 0; i < FS_WRITE_MAX; i++) {
        if (writable[i].used) {
            count++;
        }
    }

    return count;
}

const char* fs_name_at(int index) {
    if (index < 0) {
        return 0;
    }

    if (index < file_count) {
        return files[index].name;
    }

    /* index into writable overlay */
    int target = index - file_count;
    for (int i = 0; i < FS_WRITE_MAX; i++) {
        if (writable[i].used) {
            if (target == 0) {
                return writable[i].name;
            }
            target--;
        }
    }

    return 0;
}

unsigned int fs_size(const char* name) {
    if (name == 0) {
        return 0;
    }

    /* writable entries override static ones */
    for (int i = 0; i < FS_WRITE_MAX; i++) {
        if (writable[i].used && str_equals(writable[i].name, name)) {
            return writable[i].size;
        }
    }
    for (int i = 0; i < file_count; i++) {
        if (str_equals(files[i].name, name)) {
            return files[i].size;
        }
    }

    return 0;
}

int fs_exists(const char* name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < FS_WRITE_MAX; i++) {
        if (writable[i].used && str_equals(writable[i].name, name))
            return 1;
    }
    for (i = 0; i < file_count; i++) {
        if (str_equals(files[i].name, name))
            return 1;
    }
    return 0;
}

int fs_read(const char* name, char* buffer, unsigned int buffer_size) {
    int read;

    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }

    read = fs_read_bytes(name, (unsigned char*)buffer, buffer_size - 1);
    if (read < 0) {
        return -1;
    }

    buffer[read] = '\0';
    return read;
}

int fs_read_bytes(const char* name, unsigned char* buffer, unsigned int buffer_size) {
    unsigned int size;
    const unsigned char* contents = 0;

    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }

    /* check writable entries first */
    for (int w = 0; w < FS_WRITE_MAX; w++) {
        if (writable[w].used && str_equals(writable[w].name, name)) {
            contents = writable[w].data;
            size = writable[w].size;

            if (buffer_size < size) {
                size = buffer_size;
            }

            for (unsigned int i = 0; i < size; i++) {
                buffer[i] = contents[i];
            }

            return (int)size;
        }
    }

    for (int index = 0; index < file_count; index++) {
        if (str_equals(files[index].name, name)) {
            contents = files[index].contents;
            size = files[index].size;

            if (buffer_size < size) {
                size = buffer_size;
            }

            for (unsigned int i = 0; i < size; i++) {
                buffer[i] = contents[i];
            }

            return (int)size;
        }
    }

    return -1;
}

int fs_write(const char* name, const unsigned char* data, unsigned int size, int append) {
    if (name == 0 || name[0] == '\0') {
        return -1;
    }

    if (data == 0 && size > 0) {
        return -1;
    }

    /* find existing writable entry */
    int slot = -1;
    for (int i = 0; i < FS_WRITE_MAX; i++) {
        if (writable[i].used && str_equals(writable[i].name, name)) {
            slot = i;
            break;
        }
    }

    if (slot < 0 && !append) {
        /* create new slot */
        for (int i = 0; i < FS_WRITE_MAX; i++) {
            if (!writable[i].used) {
                slot = i;
                writable[i].used = 1;
                writable[i].name = (char*)kmalloc(FS_WRITE_NAME_MAX);
                if (writable[i].name) {
                    int j = 0;
                    while (name[j] != '\0' && j < FS_WRITE_NAME_MAX - 1) {
                        writable[i].name[j] = name[j];
                        j++;
                    }
                    writable[i].name[j] = '\0';
                }
                writable[i].data = 0;
                writable[i].size = 0;
                break;
            }
        }
    }

    if (slot < 0) {
        return -1;
    }

    if (append && writable[slot].used && writable[slot].data) {
        unsigned int new_size = writable[slot].size + size;
        unsigned char* new_data = (unsigned char*)kmalloc(new_size);
        if (!new_data) {
            return -1;
        }

        for (unsigned int i = 0; i < writable[slot].size; i++) {
            new_data[i] = writable[slot].data[i];
        }
        for (unsigned int i = 0; i < size; i++) {
            new_data[writable[slot].size + i] = data[i];
        }

        if (writable[slot].data) {
            kfree(writable[slot].data);
        }

        writable[slot].data = new_data;
        writable[slot].size = new_size;
        return (int)new_size;
    }

    /* overwrite or fresh write */
    if (writable[slot].data) {
        kfree(writable[slot].data);
        writable[slot].data = 0;
        writable[slot].size = 0;
    }

    if (size == 0) {
        /* create empty file */
        writable[slot].data = 0;
        writable[slot].size = 0;
        return 0;
    }

    unsigned char* copy = (unsigned char*)kmalloc(size);
    if (!copy) {
        return -1;
    }

    for (unsigned int i = 0; i < size; i++) {
        copy[i] = data[i];
    }

    writable[slot].data = copy;
    writable[slot].size = size;
    return (int)size;
}

int fs_delete(const char* name) {
    int i;
    if (!name || name[0] == '\0') return -1;
    for (i = 0; i < FS_WRITE_MAX; i++) {
        if (writable[i].used && str_equals(writable[i].name, name)) {
            if (writable[i].data) kfree(writable[i].data);
            if (writable[i].name) kfree(writable[i].name);
            writable[i].used = 0;
            writable[i].name = 0;
            writable[i].data = 0;
            writable[i].size = 0;
            return 0;
        }
    }
    /* Static (read-only) files cannot be deleted */
    return -1;
}
