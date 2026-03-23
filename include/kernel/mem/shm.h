#ifndef KERNEL_MEM_SHM_H
#define KERNEL_MEM_SHM_H

#include <stdint.h>

#define SHM_MAX_SEGMENTS 16
#define SHM_MAX_MAPPINGS 8

typedef struct {
    int used;
    int segment_id;
    uintptr_t base;
    unsigned int size;
} shm_mapping_t;

typedef struct {
    int used;
    int id;
    unsigned int size;
    unsigned int ref_count;
    int marked_for_delete;
} shm_segment_info_t;

void shm_init(void);
int shm_create(unsigned int size);
uintptr_t shm_attach(uint64_t* directory,
                    shm_mapping_t* slots,
                    int slot_count,
                    int segment_id);
int shm_detach(uint64_t* directory,
               shm_mapping_t* slots,
               int slot_count,
               uintptr_t address);
void shm_detach_all(uint64_t* directory,
                    shm_mapping_t* slots,
                    int slot_count);
int shm_clone_mappings(uint64_t* child_directory,
                       shm_mapping_t* child_slots,
                       int child_slot_count,
                       const shm_mapping_t* parent_slots,
                       int parent_slot_count);
int shm_unlink(int segment_id);
int shm_list(shm_segment_info_t* out, int max_segments);

#endif