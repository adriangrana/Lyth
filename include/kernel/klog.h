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

#endif
