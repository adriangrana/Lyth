#include "fs.h"
#include "string.h"

static const char readme_text[] =
    "Lyth FS\n"
    "-------\n"
    "Este es un filesystem en memoria de solo lectura.\n"
    "Sirve para probar syscalls y cargar recursos simples.\n";

static const char motd_text[] =
    "Bienvenido a Lyth OS.\n"
    "Ahora hay timer, jobs, heap, syscalls y un FS simple.\n";

static const char version_text[] =
    "Lyth OS v0.4\n"
    "Kernel hobby con scheduler cooperativo y PIT.\n";

typedef struct {
    const char* name;
    const char* contents;
} fs_entry_t;

static fs_entry_t files[] = {
    {"README.TXT", readme_text},
    {"MOTD.TXT", motd_text},
    {"VERSION.TXT", version_text},
};

static const int file_count = sizeof(files) / sizeof(files[0]);

static unsigned int text_length(const char* text) {
    unsigned int length = 0;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

void fs_init(void) {
}

int fs_count(void) {
    return file_count;
}

const char* fs_name_at(int index) {
    if (index < 0 || index >= file_count) {
        return 0;
    }

    return files[index].name;
}

unsigned int fs_size(const char* name) {
    for (int i = 0; i < file_count; i++) {
        if (str_equals(files[i].name, name)) {
            return text_length(files[i].contents);
        }
    }

    return 0;
}

int fs_exists(const char* name) {
    return fs_size(name) > 0;
}

int fs_read(const char* name, char* buffer, unsigned int buffer_size) {
    unsigned int size;
    unsigned int i;
    const char* contents = 0;

    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }

    for (int index = 0; index < file_count; index++) {
        if (str_equals(files[index].name, name)) {
            contents = files[index].contents;
            break;
        }
    }

    if (contents == 0) {
        return -1;
    }

    size = text_length(contents);
    if (buffer_size <= size) {
        size = buffer_size - 1;
    }

    for (i = 0; i < size; i++) {
        buffer[i] = contents[i];
    }

    buffer[size] = '\0';
    return (int)size;
}
