#include "shm.h"
#include "paging.h"
#include "physmem.h"

#define SHM_MAX_PAGES (PAGING_USER_SHM_SIZE / PAGING_PAGE_SIZE)

typedef struct {
    int used;
    int id;
    unsigned int size;
    unsigned int page_count;
    unsigned int ref_count;
    int marked_for_delete;
    uint32_t frames[SHM_MAX_PAGES];
} shm_segment_t;

static shm_segment_t shm_segments[SHM_MAX_SEGMENTS];
static int shm_next_id = 1;

static unsigned int shm_align_up(unsigned int value, unsigned int alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int shm_page_count_for_size(unsigned int size) {
    unsigned int aligned_size;

    if (size == 0 || size > PAGING_USER_SHM_SIZE) {
        return -1;
    }

    aligned_size = shm_align_up(size, PAGING_PAGE_SIZE);
    if (aligned_size == 0U || aligned_size > PAGING_USER_SHM_SIZE) {
        return -1;
    }

    return (int)(aligned_size / PAGING_PAGE_SIZE);
}

static shm_segment_t* shm_find_segment(int segment_id) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].used && shm_segments[i].id == segment_id) {
            return &shm_segments[i];
        }
    }

    return 0;
}

static void shm_release_segment(shm_segment_t* segment) {
    if (segment == 0 || !segment->used || segment->ref_count != 0U) {
        return;
    }

    for (unsigned int page = 0; page < segment->page_count; page++) {
        if (segment->frames[page] != 0U) {
            physmem_free_frame(segment->frames[page]);
            segment->frames[page] = 0U;
        }
    }

    segment->used = 0;
    segment->id = 0;
    segment->size = 0U;
    segment->page_count = 0U;
    segment->ref_count = 0U;
    segment->marked_for_delete = 0;
}

static int shm_slot_find_free(shm_mapping_t* slots, int slot_count) {
    for (int i = 0; i < slot_count; i++) {
        if (!slots[i].used) {
            return i;
        }
    }

    return -1;
}

static uintptr_t shm_find_free_base(const shm_mapping_t* slots,
                                   int slot_count,
                                   unsigned int page_count) {
    unsigned int used_pages[SHM_MAX_PAGES];

    if (page_count == 0U || page_count > SHM_MAX_PAGES) {
        return 0U;
    }

    for (unsigned int i = 0; i < SHM_MAX_PAGES; i++) {
        used_pages[i] = 0U;
    }

    for (int slot = 0; slot < slot_count; slot++) {
        unsigned int first_page;
        unsigned int count;

        if (!slots[slot].used || slots[slot].size == 0U) {
            continue;
        }

        if (slots[slot].base < PAGING_USER_SHM_BASE ||
            slots[slot].base >= (PAGING_USER_SHM_BASE + PAGING_USER_SHM_SIZE)) {
            continue;
        }

        first_page = (slots[slot].base - PAGING_USER_SHM_BASE) / PAGING_PAGE_SIZE;
        count = shm_align_up(slots[slot].size, PAGING_PAGE_SIZE) / PAGING_PAGE_SIZE;
        for (unsigned int page = 0; page < count && (first_page + page) < SHM_MAX_PAGES; page++) {
            used_pages[first_page + page] = 1U;
        }
    }

    for (unsigned int first_page = 0; first_page + page_count <= SHM_MAX_PAGES; first_page++) {
        unsigned int free = 1U;

        for (unsigned int page = 0; page < page_count; page++) {
            if (used_pages[first_page + page] != 0U) {
                free = 0U;
                first_page += page;
                break;
            }
        }

        if (free != 0U) {
            return PAGING_USER_SHM_BASE + (first_page * PAGING_PAGE_SIZE);
        }
    }

    return 0U;
}

static int shm_attach_at(uint64_t* directory,
                         shm_mapping_t* slots,
                         int slot_count,
                         shm_segment_t* segment,
                         uintptr_t base) {
    int slot_index;

    if (directory == 0 || slots == 0 || segment == 0 || !segment->used) {
        return 0;
    }

    slot_index = shm_slot_find_free(slots, slot_count);
    if (slot_index < 0) {
        return 0;
    }

    for (unsigned int page = 0; page < segment->page_count; page++) {
        uintptr_t va = base + (page * PAGING_PAGE_SIZE);
        if (!paging_map_user_page(directory, va, segment->frames[page], 1)) {
            while (page > 0U) {
                page--;
                paging_unmap_user_page(directory, base + (page * PAGING_PAGE_SIZE));
            }
            return 0;
        }
    }

    slots[slot_index].used = 1;
    slots[slot_index].segment_id = segment->id;
    slots[slot_index].base = base;
    slots[slot_index].size = segment->size;
    segment->ref_count++;
    return 1;
}

static int shm_detach_slot(uint64_t* directory, shm_mapping_t* slot) {
    shm_segment_t* segment;
    unsigned int page_count;

    if (directory == 0 || slot == 0 || !slot->used) {
        return 0;
    }

    segment = shm_find_segment(slot->segment_id);
    page_count = shm_align_up(slot->size, PAGING_PAGE_SIZE) / PAGING_PAGE_SIZE;

    for (unsigned int page = 0; page < page_count; page++) {
        paging_unmap_user_page(directory, slot->base + (page * PAGING_PAGE_SIZE));
    }

    if (segment != 0 && segment->ref_count > 0U) {
        segment->ref_count--;
        if (segment->marked_for_delete && segment->ref_count == 0U) {
            shm_release_segment(segment);
        }
    }

    slot->used = 0;
    slot->segment_id = 0;
    slot->base = 0U;
    slot->size = 0U;
    return 1;
}

void shm_init(void) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_segments[i].used = 0;
        shm_segments[i].id = 0;
        shm_segments[i].size = 0U;
        shm_segments[i].page_count = 0U;
        shm_segments[i].ref_count = 0U;
        shm_segments[i].marked_for_delete = 0;
        for (int page = 0; page < SHM_MAX_PAGES; page++) {
            shm_segments[i].frames[page] = 0U;
        }
    }
    shm_next_id = 1;
}

int shm_create(unsigned int size) {
    int page_count;
    int slot = -1;

    page_count = shm_page_count_for_size(size);
    if (page_count <= 0) {
        return -1;
    }

    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_segments[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    shm_segments[slot].used = 1;
    shm_segments[slot].id = shm_next_id++;
    shm_segments[slot].size = size;
    shm_segments[slot].page_count = (unsigned int)page_count;
    shm_segments[slot].ref_count = 0U;
    shm_segments[slot].marked_for_delete = 0;

    for (int page = 0; page < page_count; page++) {
        uint32_t frame = physmem_alloc_frame();
        if (frame == 0U) {
            shm_segments[slot].marked_for_delete = 1;
            shm_release_segment(&shm_segments[slot]);
            return -1;
        }

        shm_segments[slot].frames[page] = frame;
        for (unsigned int byte = 0; byte < PAGING_PAGE_SIZE; byte++) {
            ((unsigned char*)(uintptr_t)frame)[byte] = 0;
        }
    }

    return shm_segments[slot].id;
}

uintptr_t shm_attach(uint64_t* directory,
                    shm_mapping_t* slots,
                    int slot_count,
                    int segment_id) {
    shm_segment_t* segment;
    uintptr_t base;

    segment = shm_find_segment(segment_id);
    if (segment == 0 || segment->marked_for_delete) {
        return 0U;
    }

    base = shm_find_free_base(slots, slot_count, segment->page_count);
    if (base == 0U) {
        return 0U;
    }

    if (!shm_attach_at(directory, slots, slot_count, segment, base)) {
        return 0U;
    }

    return base;
}

int shm_detach(uint64_t* directory,
               shm_mapping_t* slots,
               int slot_count,
               uintptr_t address) {
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].used &&
            address >= slots[i].base &&
            address < slots[i].base + slots[i].size) {
            return shm_detach_slot(directory, &slots[i]);
        }
    }

    return 0;
}

void shm_detach_all(uint64_t* directory,
                    shm_mapping_t* slots,
                    int slot_count) {
    if (directory == 0 || slots == 0) {
        return;
    }

    for (int i = 0; i < slot_count; i++) {
        if (slots[i].used) {
            shm_detach_slot(directory, &slots[i]);
        }
    }
}

int shm_clone_mappings(uint64_t* child_directory,
                       shm_mapping_t* child_slots,
                       int child_slot_count,
                       const shm_mapping_t* parent_slots,
                       int parent_slot_count) {
    if (child_directory == 0 || child_slots == 0 || parent_slots == 0) {
        return 0;
    }

    for (int i = 0; i < child_slot_count; i++) {
        child_slots[i].used = 0;
        child_slots[i].segment_id = 0;
        child_slots[i].base = 0U;
        child_slots[i].size = 0U;
    }

    for (int i = 0; i < parent_slot_count; i++) {
        shm_segment_t* segment;

        if (!parent_slots[i].used) {
            continue;
        }

        segment = shm_find_segment(parent_slots[i].segment_id);
        if (segment == 0 || segment->marked_for_delete) {
            shm_detach_all(child_directory, child_slots, child_slot_count);
            return 0;
        }

        if (!shm_attach_at(child_directory, child_slots, child_slot_count,
                           segment, parent_slots[i].base)) {
            shm_detach_all(child_directory, child_slots, child_slot_count);
            return 0;
        }
    }

    return 1;
}

int shm_unlink(int segment_id) {
    shm_segment_t* segment = shm_find_segment(segment_id);

    if (segment == 0) {
        return -1;
    }

    segment->marked_for_delete = 1;
    if (segment->ref_count == 0U) {
        shm_release_segment(segment);
    }

    return 0;
}

int shm_list(shm_segment_info_t* out, int max_segments) {
    int count = 0;

    if (out == 0 || max_segments <= 0) {
        return 0;
    }

    for (int i = 0; i < SHM_MAX_SEGMENTS && count < max_segments; i++) {
        if (!shm_segments[i].used) {
            continue;
        }

        out[count].used = 1;
        out[count].id = shm_segments[i].id;
        out[count].size = shm_segments[i].size;
        out[count].ref_count = shm_segments[i].ref_count;
        out[count].marked_for_delete = shm_segments[i].marked_for_delete;
        count++;
    }

    return count;
}