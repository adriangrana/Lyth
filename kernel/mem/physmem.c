#include "physmem.h"

#define PHYSMEM_MAX_FRAMES 1048576U
#define PHYSMEM_BITMAP_SIZE (PHYSMEM_MAX_FRAMES / 8U)

extern char __kernel_start;
extern char __kernel_end;

static uint8_t frame_bitmap[PHYSMEM_BITMAP_SIZE];
static uint8_t frame_refcount[PHYSMEM_MAX_FRAMES];
static uint32_t total_frames = 0;
static uint32_t free_frames = 0;
static uint32_t highest_address = 0;

static uint32_t align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1U);
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static void bitmap_fill(uint8_t value) {
    for (uint32_t i = 0; i < PHYSMEM_BITMAP_SIZE; i++) {
        frame_bitmap[i] = value;
    }
}

static int frame_is_marked(uint32_t frame) {
    return (frame_bitmap[frame / 8U] & (uint8_t)(1U << (frame % 8U))) != 0;
}

static void mark_frame(uint32_t frame, int used) {
    uint8_t mask;

    if (frame >= PHYSMEM_MAX_FRAMES || frame >= total_frames) {
        return;
    }

    mask = (uint8_t)(1U << (frame % 8U));

    if (used) {
        if (!frame_is_marked(frame)) {
            frame_bitmap[frame / 8U] |= mask;
            if (free_frames > 0) {
                free_frames--;
            }
        }
    } else {
        if (frame_is_marked(frame)) {
            frame_bitmap[frame / 8U] &= (uint8_t)~mask;
            free_frames++;
        }
    }
}

static void mark_range(uint32_t start, uint32_t length, int used) {
    uint32_t frame_start;
    uint32_t frame_end;

    if (length == 0) {
        return;
    }

    frame_start = align_down(start, PHYSMEM_FRAME_SIZE) / PHYSMEM_FRAME_SIZE;
    frame_end = align_up(start + length, PHYSMEM_FRAME_SIZE) / PHYSMEM_FRAME_SIZE;

    if (frame_end > total_frames) {
        frame_end = total_frames;
    }

    for (uint32_t frame = frame_start; frame < frame_end; frame++) {
        mark_frame(frame, used);
    }
}

static uint32_t region_end_32(uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;

    if (end > 0xFFFFFFFFULL) {
        end = 0xFFFFFFFFULL;
    }

    return (uint32_t)end;
}

static void register_highest(uint32_t end) {
    if (end > highest_address) {
        highest_address = end;
    }
}

static void add_available_range(uint32_t start, uint32_t end) {
    uint32_t length;

    if (end <= start) {
        return;
    }

    length = end - start;
    register_highest(end);
    mark_range(start, length, 0);
}

static void init_from_multiboot_map(multiboot_info_t* mbi) {
    if ((mbi->flags & (1U << 6)) != 0 && mbi->mmap_length != 0 && mbi->mmap_addr != 0) {
        uint32_t cursor = mbi->mmap_addr;
        uint32_t end = mbi->mmap_addr + mbi->mmap_length;

        while (cursor < end) {
            multiboot_mmap_entry_t* entry = (multiboot_mmap_entry_t*)(uintptr_t)cursor;

            if (entry->type == 1 && entry->len != 0) {
                uint64_t region_start = entry->addr;
                uint32_t region_end = region_end_32(entry->addr, entry->len);

                if (region_start < 0x100000000ULL) {
                    add_available_range((uint32_t)region_start, region_end);
                }
            }

            cursor += entry->size + sizeof(entry->size);
        }

        return;
    }

    add_available_range(0x00100000U, 0x00100000U + (mbi->mem_upper * 1024U));
}

void physmem_init(multiboot_info_t* mbi) {
    uint32_t framebuffer_bytes = 0;
    uint32_t framebuffer_start = 0;

    bitmap_fill(0xFF);
    for (uint32_t i = 0; i < PHYSMEM_MAX_FRAMES; i++) {
        frame_refcount[i] = 0;
    }
    total_frames = PHYSMEM_MAX_FRAMES;
    free_frames = 0;
    highest_address = 0;

    if (mbi != 0) {
        init_from_multiboot_map(mbi);
    }

    if (highest_address == 0) {
        highest_address = 0x01000000U;
        add_available_range(0x00100000U, highest_address);
    }

    total_frames = align_up(highest_address, PHYSMEM_FRAME_SIZE) / PHYSMEM_FRAME_SIZE;
    if (total_frames > PHYSMEM_MAX_FRAMES) {
        total_frames = PHYSMEM_MAX_FRAMES;
        highest_address = PHYSMEM_MAX_FRAMES * PHYSMEM_FRAME_SIZE;
    }

    mark_range(0x00000000U, 0x00100000U, 1);
    mark_range((uint32_t)(uintptr_t)&__kernel_start,
               (uint32_t)((uintptr_t)&__kernel_end - (uintptr_t)&__kernel_start),
               1);

    if (mbi != 0) {
        mark_range((uint32_t)(uintptr_t)mbi, sizeof(multiboot_info_t), 1);

        if ((mbi->flags & (1U << 6)) != 0 && mbi->mmap_length != 0 && mbi->mmap_addr != 0) {
            mark_range(mbi->mmap_addr, mbi->mmap_length, 1);
        }

        if ((mbi->flags & (1U << 12)) != 0 && mbi->framebuffer_addr != 0 &&
            mbi->framebuffer_pitch != 0 && mbi->framebuffer_height != 0) {
            framebuffer_start = (uint32_t)mbi->framebuffer_addr;
            framebuffer_bytes = mbi->framebuffer_pitch * mbi->framebuffer_height;
            mark_range(framebuffer_start, framebuffer_bytes, 1);
            register_highest(framebuffer_start + framebuffer_bytes);
        }
    }
}

uint32_t physmem_alloc_frame(void) {
    for (uint32_t frame = 0; frame < total_frames; frame++) {
        if (!frame_is_marked(frame)) {
            mark_frame(frame, 1);
            frame_refcount[frame] = 1;
            return frame * PHYSMEM_FRAME_SIZE;
        }
    }

    return 0;
}

void physmem_free_frame(uint32_t physical_address) {
    uint32_t frame = physical_address / PHYSMEM_FRAME_SIZE;

    if (physical_address == 0 || (physical_address % PHYSMEM_FRAME_SIZE) != 0) {
        return;
    }

    if (frame < (0x00100000U / PHYSMEM_FRAME_SIZE)) {
        return;
    }

    frame_refcount[frame] = 0;
    mark_frame(frame, 0);
}

uint32_t physmem_alloc_region(uint32_t size, uint32_t alignment) {
    uint32_t aligned_size;
    uint32_t aligned_start;
    uint32_t end_frame;
    uint32_t start_frame;
    uint32_t frame_count;

    if (size == 0) {
        return 0;
    }

    if (alignment < PHYSMEM_FRAME_SIZE) {
        alignment = PHYSMEM_FRAME_SIZE;
    }

    aligned_size = align_up(size, PHYSMEM_FRAME_SIZE);
    frame_count = aligned_size / PHYSMEM_FRAME_SIZE;

    for (start_frame = 0; start_frame + frame_count <= total_frames; start_frame++) {
        uint32_t start = start_frame * PHYSMEM_FRAME_SIZE;
        int free = 1;

        aligned_start = align_up(start, alignment);
        if (aligned_start != start) {
            continue;
        }

        if (start < 0x00100000U) {
            continue;
        }

        end_frame = start_frame + frame_count;
        for (uint32_t frame = start_frame; frame < end_frame; frame++) {
            if (frame_is_marked(frame)) {
                free = 0;
                start_frame = frame;
                break;
            }
        }

        if (!free) {
            continue;
        }

        mark_range(start, aligned_size, 1);
        for (uint32_t frame = start_frame; frame < end_frame; frame++) {
            frame_refcount[frame] = 1;
        }
        return start;
    }

    return 0;
}

void physmem_free_region(uint32_t start, uint32_t length) {
    uint32_t frame_start;
    uint32_t frame_end;

    if (length == 0) {
        return;
    }

    frame_start = align_down(start, PHYSMEM_FRAME_SIZE) / PHYSMEM_FRAME_SIZE;
    frame_end = align_up(start + length, PHYSMEM_FRAME_SIZE) / PHYSMEM_FRAME_SIZE;

    if (frame_end > total_frames) {
        frame_end = total_frames;
    }

    for (uint32_t frame = frame_start; frame < frame_end; frame++) {
        physmem_unref_frame(frame * PHYSMEM_FRAME_SIZE);
    }
}

void physmem_reserve_region(uint32_t start, uint32_t length) {
    mark_range(start, length, 1);
}

void physmem_ref_frame(uint32_t physical_address) {
    uint32_t frame = physical_address / PHYSMEM_FRAME_SIZE;

    if (physical_address == 0 || (physical_address % PHYSMEM_FRAME_SIZE) != 0) {
        return;
    }

    if (frame >= total_frames) {
        return;
    }

    if (frame_refcount[frame] < 255) {
        frame_refcount[frame]++;
    }
}

void physmem_unref_frame(uint32_t physical_address) {
    uint32_t frame = physical_address / PHYSMEM_FRAME_SIZE;

    if (physical_address == 0 || (physical_address % PHYSMEM_FRAME_SIZE) != 0) {
        return;
    }

    if (frame >= total_frames || frame < (0x00100000U / PHYSMEM_FRAME_SIZE)) {
        return;
    }

    if (frame_refcount[frame] == 0) {
        return;
    }

    frame_refcount[frame]--;
    if (frame_refcount[frame] == 0) {
        mark_frame(frame, 0);
    }
}

unsigned int physmem_frame_refcount(uint32_t physical_address) {
    uint32_t frame = physical_address / PHYSMEM_FRAME_SIZE;

    if (physical_address == 0 || (physical_address % PHYSMEM_FRAME_SIZE) != 0) {
        return 0;
    }

    if (frame >= total_frames) {
        return 0;
    }

    return frame_refcount[frame];
}

uint32_t physmem_total_bytes(void) {
    return total_frames * PHYSMEM_FRAME_SIZE;
}

uint32_t physmem_free_bytes(void) {
    return free_frames * PHYSMEM_FRAME_SIZE;
}

uint32_t physmem_used_bytes(void) {
    return physmem_total_bytes() - physmem_free_bytes();
}

uint32_t physmem_frame_count(void) {
    return total_frames;
}

uint32_t physmem_highest_address(void) {
    return highest_address;
}