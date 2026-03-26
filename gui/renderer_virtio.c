/*
 * Virtio-GPU Renderer Backend
 *
 * Thin layer that uses the SW renderer for all drawing operations
 * (fill, blit, blur, text, etc.) but overrides the present path
 * to use virtio-gpu TRANSFER_TO_HOST_2D + RESOURCE_FLUSH instead
 * of memcpy to the MMIO framebuffer.
 *
 * Benefits:
 *   - No slow uncacheable framebuffer writes
 *   - Host-side compositing (QEMU uses host GPU)
 *   - Same pixel-perfect output as SW backend
 */

#include "renderer.h"
#include "virtio_gpu.h"
#include "serial.h"
#include "string.h"

/* ---- Static state ---- */
static virtio_gpu_dev_t vgpu;
static uint32_t         vgpu_res_id;   /* scanout resource */
static gpu_ops_t        virtio_ops;    /* our vtable (copy of sw_ops + overrides) */
static int              vgpu_active;   /* 1 if virtio-gpu is driving display */

/* ================================================================== */
/*  Present: transfer dirty region + flush                             */
/* ================================================================== */

static void vgpu_present(int x, int y, int w, int h)
{
    if (!vgpu_active) return;
    /* Batched transfer+flush in single virtio notify (halves latency) */
    virtio_gpu_present(&vgpu, vgpu_res_id,
                       (uint32_t)x, (uint32_t)y,
                       (uint32_t)w, (uint32_t)h);
}

static void vgpu_present_full(void)
{
    vgpu_present(0, 0, g_gpu.width, g_gpu.height);
}

/* virtio-gpu doesn't have a vblank — the transfer+flush is
 * already synchronous, so vsync_wait is a no-op */
static void vgpu_vsync_wait(void)
{
    /* no-op */
}

static void vgpu_shutdown(void)
{
    if (vgpu_active) {
        virtio_gpu_unref_resource(&vgpu, vgpu_res_id);
        vgpu_active = 0;
    }
}

/* ================================================================== */
/*  Init: probe device, set up scanout, override present ops           */
/* ================================================================== */

int gpu_init_virtio(int w, int h, int bpp)
{
    /* Always initialize the SW engine first (allocates backbuffer,
     * sets up all drawing functions).  This is our fallback. */
    int rc = gpu_init_sw(w, h, bpp);
    if (rc != 0)
        return rc;

    /* Try to find and init virtio-gpu device */
    if (virtio_gpu_init(&vgpu) != 0) {
        serial_print("[renderer] no virtio-gpu, using software backend\n");
        return 0;   /* SW is active — that's fine */
    }

    /* Create a 2D resource matching the backbuffer */
    vgpu_res_id = virtio_gpu_create_resource(
        &vgpu, (uint32_t)w, (uint32_t)h,
        VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
    vgpu.res_w = (uint32_t)w;
    if (vgpu_res_id == 0) {
        serial_print("[renderer] resource create failed, using SW\n");
        return 0;
    }

    /* Attach the SW backbuffer's physical memory as backing store.
     * Since the kernel identity-maps < 4 GB, the physmem allocation
     * done by sw_init is directly DMA-accessible. */
    if (virtio_gpu_attach_backing(&vgpu, vgpu_res_id,
                                   g_gpu.backbuffer->alloc_phys,
                                   g_gpu.backbuffer->alloc_size) != 0) {
        serial_print("[renderer] attach_backing failed, using SW\n");
        return 0;
    }

    /* Set this resource as the display scanout */
    if (virtio_gpu_set_scanout(&vgpu, vgpu_res_id,
                                (uint32_t)w, (uint32_t)h) != 0) {
        serial_print("[renderer] set_scanout failed, using SW\n");
        return 0;
    }

    /* Flush the (empty) backbuffer to the scanout so the resource is
     * in a known state before the first real present. */
    virtio_gpu_transfer(&vgpu, vgpu_res_id, 0, 0, (uint32_t)w, (uint32_t)h);
    virtio_gpu_flush(&vgpu, vgpu_res_id, 0, 0, (uint32_t)w, (uint32_t)h);

    /* Build the virtio ops table: start with all SW ops, then
     * override only the present/lifecycle functions */
    virtio_ops = sw_ops;
    virtio_ops.name         = "virtio-gpu";
    virtio_ops.present      = vgpu_present;
    virtio_ops.present_full = vgpu_present_full;
    virtio_ops.vsync_wait   = vgpu_vsync_wait;
    virtio_ops.shutdown     = vgpu_shutdown;

    /* Switch the global renderer to our backend */
    g_gpu.ops = &virtio_ops;
    vgpu_active = 1;

    serial_print("[renderer] using virtio-gpu backend\n");
    return 0;
}
