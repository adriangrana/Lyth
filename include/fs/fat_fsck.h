#ifndef FAT_FSCK_H
#define FAT_FSCK_H

#include <stdint.h>

typedef struct {
    uint32_t errors;
    uint32_t warnings;
    uint32_t fat_out_of_range;
    uint32_t fat_bad_clusters;
    uint32_t fat_loops;
    uint32_t dir_corrupt_entries;
    uint32_t invalid_start_cluster;
    uint32_t files_checked;
    uint32_t dirs_checked;
} fat_fsck_report_t;

#endif /* FAT_FSCK_H */
