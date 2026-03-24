#include "klog.h"
#include "terminal.h"
#include "timer.h"
#include "string.h"

#define KLOG_MAX_ENTRIES 64
#define KLOG_COMPONENT_MAX 16
#define KLOG_MESSAGE_MAX 80

typedef struct {
    unsigned int ticks;
    klog_level_t level;
    char component[KLOG_COMPONENT_MAX];
    char message[KLOG_MESSAGE_MAX];
} klog_entry_t;

static klog_entry_t klog_entries[KLOG_MAX_ENTRIES];
static int klog_start = 0;
static int klog_size = 0;

static void klog_copy_text(char* destination, unsigned int destination_size, const char* source) {
    unsigned int index = 0;

    if (destination == 0 || destination_size == 0) {
        return;
    }

    if (source == 0) {
        destination[0] = '\0';
        return;
    }

    while (source[index] != '\0' && index < destination_size - 1) {
        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
}

static const char* klog_level_name(klog_level_t level) {
    switch (level) {
        case KLOG_LEVEL_DEBUG:
            return "DEBUG";
        case KLOG_LEVEL_INFO:
            return "INFO";
        case KLOG_LEVEL_WARN:
            return "WARN";
        case KLOG_LEVEL_ERROR:
            return "ERROR";
        default:
            return "UNK";
    }
}

void klog_clear(void) {
    klog_start = 0;
    klog_size = 0;
}

void klog_write(klog_level_t level, const char* component, const char* message) {
    int index;
    klog_entry_t* entry;

    if (klog_size < KLOG_MAX_ENTRIES) {
        index = (klog_start + klog_size) % KLOG_MAX_ENTRIES;
        klog_size++;
    } else {
        index = klog_start;
        klog_start = (klog_start + 1) % KLOG_MAX_ENTRIES;
    }

    entry = &klog_entries[index];
    entry->ticks = timer_get_ticks();
    entry->level = level;
    klog_copy_text(entry->component, sizeof(entry->component), component != 0 ? component : "kernel");
    klog_copy_text(entry->message, sizeof(entry->message), message != 0 ? message : "");
}

void klog_dump_to_terminal(void) {
    for (int i = 0; i < klog_size; i++) {
        klog_entry_t* entry = &klog_entries[(klog_start + i) % KLOG_MAX_ENTRIES];

        terminal_put_char('[');
        terminal_print_uint(entry->ticks);
        terminal_put_char(']');
        terminal_put_char(' ');
        terminal_print(klog_level_name(entry->level));
        terminal_put_char(' ');
        terminal_print(entry->component);
        terminal_print(": ");
        terminal_print_line(entry->message);
    }
}

int klog_count(void) {
    return klog_size;
}

int klog_read_entry(int index, klog_level_t* level_out,
                    char* comp_out, int comp_max,
                    char* msg_out, int msg_max) {
    klog_entry_t* entry;
    if (index < 0 || index >= klog_size) return -1;
    entry = &klog_entries[(klog_start + index) % KLOG_MAX_ENTRIES];
    if (level_out) *level_out = entry->level;
    if (comp_out) klog_copy_text(comp_out, (unsigned int)comp_max, entry->component);
    if (msg_out) klog_copy_text(msg_out, (unsigned int)msg_max, entry->message);
    return 0;
}
