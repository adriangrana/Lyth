#ifndef KLOG_H
#define KLOG_H

typedef enum {
    KLOG_LEVEL_DEBUG = 0,
    KLOG_LEVEL_INFO,
    KLOG_LEVEL_WARN,
    KLOG_LEVEL_ERROR
} klog_level_t;

void klog_clear(void);
void klog_write(klog_level_t level, const char* component, const char* message);
void klog_dump_to_terminal(void);
int klog_count(void);

/* Read entry at logical index (0 = oldest). Returns 0 on success, -1 if out of range.
   level_out, comp_out (max 16), msg_out (max 80) are filled if non-NULL. */
int klog_read_entry(int index, klog_level_t* level_out,
                    char* comp_out, int comp_max,
                    char* msg_out, int msg_max);

#endif
