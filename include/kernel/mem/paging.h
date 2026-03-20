#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "multiboot.h"

void paging_init(multiboot_info_t* mbi);
int paging_is_enabled(void);
uint32_t paging_mapped_bytes(void);

#endif