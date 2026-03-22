#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_USER_CODE_SELECTOR   0x18
#define GDT_USER_DATA_SELECTOR   0x20
#define GDT_TSS_SELECTOR         0x28

void gdt_init(void);
uint16_t gdt_kernel_code_selector(void);
uint16_t gdt_kernel_data_selector(void);
uint16_t gdt_user_code_selector(void);
uint16_t gdt_user_data_selector(void);

/* Per-AP GDT/TSS setup: installs a fresh GDT copy with a unique TSS
   pointing to the given kernel stack top.  Must be called on the AP itself. */
void gdt_init_ap(uint32_t kernel_stack_top);

#endif