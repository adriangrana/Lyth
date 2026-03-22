#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "multiboot.h"

#define PAGING_PAGE_SIZE 0x1000U
#define PAGING_USER_BASE 0x01000000U
#define PAGING_USER_SIZE 0x00400000U
#define PAGING_USER_STACK_SIZE 0x1000U
#define PAGING_USER_STACK_GUARD_SIZE PAGING_PAGE_SIZE
#define PAGING_USER_STACK_GUARD_BASE \
	(PAGING_USER_BASE + PAGING_USER_SIZE - PAGING_USER_STACK_SIZE - PAGING_USER_STACK_GUARD_SIZE)
#define PAGING_USER_STACK_BOTTOM (PAGING_USER_BASE + PAGING_USER_SIZE - PAGING_USER_STACK_SIZE)
#define PAGING_USER_STACK_TOP (PAGING_USER_BASE + PAGING_USER_SIZE)
#define PAGING_USER_SIGNAL_TRAMPOLINE_SIZE 16U

void paging_init(multiboot_info_t* mbi);
int paging_is_enabled(void);
uint32_t paging_mapped_bytes(void);
uint32_t paging_user_base(void);
uint32_t paging_user_size(void);
int paging_address_is_user_accessible(uint32_t address, uint32_t size);
int paging_user_buffer_is_accessible(const void* buffer, uint32_t size);
int paging_user_string_is_accessible(const char* text, uint32_t max_length);
int paging_directory_user_buffer_is_accessible(uint32_t* directory, uint32_t address, uint32_t size);
uint32_t* paging_kernel_directory(void);
uint32_t* paging_create_user_directory(uint32_t user_physical_base);
void paging_destroy_user_directory(uint32_t* directory);
void paging_switch_directory(uint32_t* directory);

#endif