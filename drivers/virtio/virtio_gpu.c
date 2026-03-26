/*
 * Virtio-GPU driver  (2D only)
 *
 * Manages virtio-gpu resources and issues 2D commands:
 *   - GET_DISPLAY_INFO
 *   - RESOURCE_CREATE_2D  / RESOURCE_UNREF
 *   - RESOURCE_ATTACH_BACKING
 *   - SET_SCANOUT
 *   - TRANSFER_TO_HOST_2D
 *   - RESOURCE_FLUSH
 *
 * All commands are synchronous (one at a time on controlq 0).
 */

#include "virtio_gpu.h"
#include "serial.h"
#include "string.h"

/* ---- Static command/response buffers (page-aligned for DMA) ---- */
static virtio_gpu_hdr_t       cmd_hdr  __attribute__((aligned(64)));
static virtio_gpu_hdr_t       resp_hdr __attribute__((aligned(64)));

/* Large responses */
static virtio_gpu_display_info_t disp_info __attribute__((aligned(64)));

/* Attach backing: header + 1 mem entry packed together */
static struct {
    virtio_gpu_attach_backing_t  cmd;
    virtio_gpu_mem_entry_t       entry;
} __attribute__((packed, aligned(64))) attach_buf;

/* Fill header with a command type and zero the rest */
static void hdr_init(virtio_gpu_hdr_t *h, uint32_t type)
{
    memset(h, 0, sizeof(*h));
    h->type = type;
}

/* ================================================================== */
/*  Init: probe PCI, init transport, allocate controlq                 */
/* ================================================================== */

int virtio_gpu_init(virtio_gpu_dev_t *gpu)
{
    memset(gpu, 0, sizeof(*gpu));
    gpu->next_resource_id = 1;

    /* Probe PCI for virtio-gpu device */
    if (virtio_pci_probe(&gpu->vdev, VIRTIO_PCI_DEVICE_GPU) != 0)
        return -1;

    /* Enable bus mastering for DMA */
    pci_enable_bus_mastering(gpu->vdev.pci);

    /* Init virtio transport (reset, features, etc.) */
    if (virtio_init_device(&gpu->vdev) != 0)
        return -1;

    /* Allocate controlq (queue 0) */
    if (virtio_alloc_queue(&gpu->vdev, 0) != 0)
        return -1;

    /* Set DRIVER_OK */
    virtio_driver_ok(&gpu->vdev);

    /* Query display info */
    if (virtio_gpu_get_display_info(gpu) != 0) {
        serial_print("[virtio-gpu] get_display_info failed\n");
        return -1;
    }

    serial_print("[virtio-gpu] display: ");
    serial_print_int((int)gpu->display_w);
    serial_print("x");
    serial_print_int((int)gpu->display_h);
    serial_print("\n");
    return 0;
}

/* ================================================================== */
/*  GET_DISPLAY_INFO                                                   */
/* ================================================================== */

int virtio_gpu_get_display_info(virtio_gpu_dev_t *gpu)
{
    hdr_init(&cmd_hdr, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    memset(&disp_info, 0, sizeof(disp_info));

    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &cmd_hdr, sizeof(cmd_hdr),
                        &disp_info, sizeof(disp_info)) != 0)
        return -1;

    if (disp_info.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        serial_print("[virtio-gpu] display_info bad resp\n");
        return -1;
    }

    /* Use the first enabled scanout */
    int i;
    for (i = 0; i < 16; i++) {
        if (disp_info.pmodes[i].enabled &&
            disp_info.pmodes[i].r.width > 0) {
            gpu->display_w = disp_info.pmodes[i].r.width;
            gpu->display_h = disp_info.pmodes[i].r.height;
            return 0;
        }
    }

    serial_print("[virtio-gpu] no enabled scanout\n");
    return -1;
}

/* ================================================================== */
/*  RESOURCE_CREATE_2D                                                 */
/* ================================================================== */

uint32_t virtio_gpu_create_resource(virtio_gpu_dev_t *gpu,
                                     uint32_t w, uint32_t h, uint32_t fmt)
{
    static virtio_gpu_res_create_2d_t cr __attribute__((aligned(64)));
    hdr_init(&cr.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    cr.resource_id = gpu->next_resource_id++;
    cr.format      = fmt;
    cr.width       = w;
    cr.height      = h;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &cr, sizeof(cr),
                        &resp_hdr, sizeof(resp_hdr)) != 0)
        return 0;

    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_print("[virtio-gpu] create_resource failed\n");
        return 0;
    }

    serial_print("[virtio-gpu] resource created\n");
    return cr.resource_id;
}

/* ================================================================== */
/*  RESOURCE_ATTACH_BACKING                                            */
/* ================================================================== */

int virtio_gpu_attach_backing(virtio_gpu_dev_t *gpu, uint32_t res_id,
                               uint32_t phys_addr, uint32_t size)
{
    hdr_init(&attach_buf.cmd.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    attach_buf.cmd.resource_id = res_id;
    attach_buf.cmd.nr_entries  = 1;
    attach_buf.entry.addr      = (uint64_t)phys_addr;
    attach_buf.entry.length    = size;
    attach_buf.entry.padding   = 0;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &attach_buf,
                        (uint32_t)sizeof(attach_buf.cmd) +
                        (uint32_t)sizeof(attach_buf.entry),
                        &resp_hdr, sizeof(resp_hdr)) != 0)
        return -1;

    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_print("[virtio-gpu] attach_backing failed\n");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  SET_SCANOUT                                                        */
/* ================================================================== */

int virtio_gpu_set_scanout(virtio_gpu_dev_t *gpu, uint32_t res_id,
                            uint32_t w, uint32_t h)
{
    static virtio_gpu_set_scanout_t sc __attribute__((aligned(64)));
    hdr_init(&sc.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    sc.r.x = 0;
    sc.r.y = 0;
    sc.r.width  = w;
    sc.r.height = h;
    sc.scanout_id  = 0;
    sc.resource_id = res_id;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &sc, sizeof(sc),
                        &resp_hdr, sizeof(resp_hdr)) != 0)
        return -1;

    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_print("[virtio-gpu] set_scanout failed\n");
        return -1;
    }

    gpu->scanout_res_id = res_id;
    serial_print("[virtio-gpu] scanout active\n");
    return 0;
}

/* ================================================================== */
/*  TRANSFER_TO_HOST_2D                                                */
/* ================================================================== */

int virtio_gpu_transfer(virtio_gpu_dev_t *gpu, uint32_t res_id,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    static virtio_gpu_transfer_2d_t tr __attribute__((aligned(64)));
    hdr_init(&tr.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    tr.r.x = x;
    tr.r.y = y;
    tr.r.width  = w;
    tr.r.height = h;
    tr.offset      = (uint64_t)(y * gpu->res_w + x) * 4;
    tr.resource_id = res_id;
    tr.padding     = 0;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &tr, sizeof(tr),
                        &resp_hdr, sizeof(resp_hdr)) != 0)
        return -1;

    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_print("[virtio-gpu] transfer failed\n");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  RESOURCE_FLUSH                                                     */
/* ================================================================== */

int virtio_gpu_flush(virtio_gpu_dev_t *gpu, uint32_t res_id,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    static virtio_gpu_resource_flush_t fl __attribute__((aligned(64)));
    hdr_init(&fl.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    fl.r.x = x;
    fl.r.y = y;
    fl.r.width  = w;
    fl.r.height = h;
    fl.resource_id = res_id;
    fl.padding     = 0;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &fl, sizeof(fl),
                        &resp_hdr, sizeof(resp_hdr)) != 0)
        return -1;

    if (resp_hdr.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_print("[virtio-gpu] flush failed\n");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  TRANSFER + FLUSH batched (single notify, halves round-trip cost)   */
/* ================================================================== */

int virtio_gpu_present(virtio_gpu_dev_t *gpu, uint32_t res_id,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    static virtio_gpu_transfer_2d_t tr2 __attribute__((aligned(64)));
    static virtio_gpu_resource_flush_t fl2 __attribute__((aligned(64)));
    static virtio_gpu_hdr_t resp1 __attribute__((aligned(64)));
    static virtio_gpu_hdr_t resp2 __attribute__((aligned(64)));

    hdr_init(&tr2.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    tr2.r.x = x;
    tr2.r.y = y;
    tr2.r.width  = w;
    tr2.r.height = h;
    tr2.offset      = (uint64_t)(y * gpu->res_w + x) * 4;
    tr2.resource_id = res_id;
    tr2.padding     = 0;

    hdr_init(&fl2.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    fl2.r.x = x;
    fl2.r.y = y;
    fl2.r.width  = w;
    fl2.r.height = h;
    fl2.resource_id = res_id;
    fl2.padding     = 0;

    memset(&resp1, 0, sizeof(resp1));
    memset(&resp2, 0, sizeof(resp2));

    return virtio_send_cmd_pair(&gpu->vdev.queues[0],
                                &tr2, sizeof(tr2), &resp1, sizeof(resp1),
                                &fl2, sizeof(fl2), &resp2, sizeof(resp2));
}

/* ================================================================== */
/*  RESOURCE_UNREF                                                     */
/* ================================================================== */

int virtio_gpu_unref_resource(virtio_gpu_dev_t *gpu, uint32_t res_id)
{
    static virtio_gpu_resource_unref_t ur __attribute__((aligned(64)));
    hdr_init(&ur.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    ur.resource_id = res_id;
    ur.padding     = 0;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    if (virtio_send_cmd(&gpu->vdev.queues[0],
                        &ur, sizeof(ur),
                        &resp_hdr, sizeof(resp_hdr)) != 0)
        return -1;

    return (resp_hdr.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}
