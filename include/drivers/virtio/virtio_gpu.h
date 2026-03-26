#ifndef DRIVERS_VIRTIO_GPU_H
#define DRIVERS_VIRTIO_GPU_H

#include <stdint.h>
#include "virtio.h"

/* ====================================================================
 *  Virtio-GPU  (2D only)
 *
 *  Provides resource creation/destruction, backing store management,
 *  scanout setup, and host-side transfer + flush for display updates.
 * ==================================================================== */

/* ---- Command types ---- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107

/* ---- Response types ---- */
#define VIRTIO_GPU_RESP_OK_NODATA         0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO   0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC        0x1200

/* ---- Pixel formats ---- */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2

/* ---- Structures ---- */

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_hdr_t;

typedef struct {
    uint32_t x, y, width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

/* GET_DISPLAY_INFO response */
typedef struct {
    virtio_gpu_hdr_t hdr;
    struct {
        virtio_gpu_rect_t r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[16];
} __attribute__((packed)) virtio_gpu_display_info_t;

/* RESOURCE_CREATE_2D */
typedef struct {
    virtio_gpu_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_res_create_2d_t;

/* RESOURCE_ATTACH_BACKING */
typedef struct {
    virtio_gpu_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed)) virtio_gpu_attach_backing_t;

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

/* SET_SCANOUT */
typedef struct {
    virtio_gpu_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

/* TRANSFER_TO_HOST_2D */
typedef struct {
    virtio_gpu_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_2d_t;

/* RESOURCE_FLUSH */
typedef struct {
    virtio_gpu_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

/* RESOURCE_UNREF */
typedef struct {
    virtio_gpu_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

/* ---- Virtio-GPU device state ---- */
typedef struct {
    virtio_device_t   vdev;
    uint32_t          display_w;
    uint32_t          display_h;
    uint32_t          next_resource_id;
    uint32_t          scanout_res_id;   /* active scanout resource */
    uint32_t          res_w;            /* scanout resource width  */
} virtio_gpu_dev_t;

/* ---- API ---- */
int  virtio_gpu_init(virtio_gpu_dev_t *gpu);
int  virtio_gpu_get_display_info(virtio_gpu_dev_t *gpu);
uint32_t virtio_gpu_create_resource(virtio_gpu_dev_t *gpu,
                                     uint32_t w, uint32_t h, uint32_t fmt);
int  virtio_gpu_attach_backing(virtio_gpu_dev_t *gpu, uint32_t res_id,
                                uint32_t phys_addr, uint32_t size);
int  virtio_gpu_set_scanout(virtio_gpu_dev_t *gpu, uint32_t res_id,
                             uint32_t w, uint32_t h);
int  virtio_gpu_transfer(virtio_gpu_dev_t *gpu, uint32_t res_id,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  virtio_gpu_flush(virtio_gpu_dev_t *gpu, uint32_t res_id,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  virtio_gpu_present(virtio_gpu_dev_t *gpu, uint32_t res_id,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  virtio_gpu_unref_resource(virtio_gpu_dev_t *gpu, uint32_t res_id);

#endif /* DRIVERS_VIRTIO_GPU_H */
