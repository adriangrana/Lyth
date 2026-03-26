/*
 * Virtio PCI Transport  (modern / v1.0+, split virtqueue)
 *
 * Handles PCI capability parsing, device init, virtqueue allocation,
 * and synchronous command submission via descriptor chains.
 */

#include "virtio.h"
#include "pci.h"
#include "paging.h"
#include "physmem.h"
#include "serial.h"
#include "string.h"

/* ================================================================== */
/*  PCI helpers (capability list traversal)                            */
/* ================================================================== */

static inline uint8_t pci_r8(const pci_device_t *d, uint8_t off)
{
    uint32_t val = pci_config_read32(d->bus, d->slot, d->func, off & 0xFC);
    return (uint8_t)(val >> ((off & 3) * 8));
}

/* Get BAR base address (handles 32/64-bit MMIO and I/O) */
static uint64_t bar_address(const pci_device_t *d, uint8_t idx)
{
    uint32_t bar = d->bar[idx];
    if (bar & 1)
        return bar & ~0x3U;                  /* I/O port */
    uint64_t addr = bar & ~0xFU;             /* 32-bit MMIO */
    if (((bar >> 1) & 3) == 2 && idx < 5)
        addr |= (uint64_t)d->bar[idx + 1] << 32;  /* 64-bit */
    return addr;
}

/* ================================================================== */
/*  PCI probe: find device + parse virtio PCI capabilities             */
/* ================================================================== */

int virtio_pci_probe(virtio_device_t *dev, uint16_t device_id)
{
    const pci_device_t *pci = pci_find_device(VIRTIO_PCI_VENDOR, device_id);
    if (!pci) {
        serial_print("[virtio] device not found\n");
        return -1;
    }

    memset(dev, 0, sizeof(*dev));
    dev->pci = pci;

    serial_print("[virtio] found device at PCI ");
    serial_print_int((int)pci->bus); serial_print(":");
    serial_print_int((int)pci->slot); serial_print(".");
    serial_print_int((int)pci->func); serial_print("\n");

    /* Walk PCI capability list looking for virtio caps (cap_id = 0x09) */
    uint16_t status = pci_config_read16(pci->bus, pci->slot, pci->func, 0x06);
    if (!(status & (1 << 4))) {
        serial_print("[virtio] no PCI capabilities\n");
        return -1;
    }

    uint8_t cap_ptr = pci_r8(pci, 0x34) & 0xFC;
    int limit = 48;

    while (cap_ptr && limit--) {
        uint8_t cap_id   = pci_r8(pci, cap_ptr);
        uint8_t cap_next = pci_r8(pci, (uint8_t)(cap_ptr + 1)) & 0xFC;

        if (cap_id == 0x09) {  /* vendor-specific = virtio */
            uint8_t cfg_type = pci_r8(pci, (uint8_t)(cap_ptr + 3));
            uint8_t bar_idx  = pci_r8(pci, (uint8_t)(cap_ptr + 4));
            uint32_t offset  = pci_config_read32(pci->bus, pci->slot,
                                                  pci->func,
                                                  (uint8_t)(cap_ptr + 8));
            uint32_t length  = pci_config_read32(pci->bus, pci->slot,
                                                  pci->func,
                                                  (uint8_t)(cap_ptr + 12));

            uint64_t base = bar_address(pci, bar_idx);
            paging_map_mmio((uintptr_t)base);
            volatile uint8_t *mmio = (volatile uint8_t *)(uintptr_t)(base + offset);

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                dev->common_cfg = mmio;
                serial_print("[virtio] common_cfg found\n");
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                dev->notify_base = mmio;
                /* notify_off_multiplier is at cap_ptr + 16 (4 bytes) */
                dev->notify_off_multiplier =
                    pci_config_read32(pci->bus, pci->slot, pci->func,
                                      (uint8_t)(cap_ptr + 16));
                serial_print("[virtio] notify found\n");
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                dev->isr_cfg = mmio;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                dev->device_cfg = mmio;
                serial_print("[virtio] device_cfg found\n");
                break;
            default:
                break;
            }
        }
        cap_ptr = cap_next;
    }

    if (!dev->common_cfg || !dev->notify_base) {
        serial_print("[virtio] missing required capabilities\n");
        return -1;
    }

    return 0;
}

/* ================================================================== */
/*  Device initialization (reset → negotiate → DRIVER_OK)              */
/* ================================================================== */

int virtio_init_device(virtio_device_t *dev)
{
    volatile uint8_t *cfg = dev->common_cfg;

    /* 1. Reset */
    vio_w8(cfg, VIRTIO_CMN_STATUS, 0);
    /* Read back to ensure reset is complete */
    while (vio_r8(cfg, VIRTIO_CMN_STATUS) != 0)
        ;

    /* 2. Acknowledge */
    vio_w8(cfg, VIRTIO_CMN_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* 3. Driver */
    vio_w8(cfg, VIRTIO_CMN_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Feature negotiation: accept whatever features the device offers
     *    (for GPU 2D we don't need specific features) */
    vio_w32(cfg, VIRTIO_CMN_GFSELECT, 0);
    vio_w32(cfg, VIRTIO_CMN_GF, 0);   /* accept none for simplicity */
    vio_w32(cfg, VIRTIO_CMN_GFSELECT, 1);
    vio_w32(cfg, VIRTIO_CMN_GF, 0);

    /* 5. Features OK */
    vio_w8(cfg, VIRTIO_CMN_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK);

    uint8_t s = vio_r8(cfg, VIRTIO_CMN_STATUS);
    if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
        serial_print("[virtio] device rejected features\n");
        vio_w8(cfg, VIRTIO_CMN_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Read number of queues */
    dev->num_queues = (int)vio_r16(cfg, VIRTIO_CMN_NUM_QUEUES);
    if (dev->num_queues > VIRTIO_MAX_QUEUES)
        dev->num_queues = VIRTIO_MAX_QUEUES;

    serial_print("[virtio] queues: ");
    serial_print_int(dev->num_queues);
    serial_print("\n");
    return 0;
}

/* ================================================================== */
/*  Virtqueue allocation                                               */
/* ================================================================== */

int virtio_alloc_queue(virtio_device_t *dev, int qidx)
{
    volatile uint8_t *cfg = dev->common_cfg;
    virtqueue_t *vq = &dev->queues[qidx];

    /* Select queue */
    vio_w16(cfg, VIRTIO_CMN_Q_SELECT, (uint16_t)qidx);

    uint16_t max_size = vio_r16(cfg, VIRTIO_CMN_Q_SIZE);
    if (max_size == 0) {
        serial_print("[virtio] queue not available\n");
        return -1;
    }

    /* Cap at 64 for simplicity */
    uint16_t size = max_size;
    if (size > 64)
        size = 64;
    vq->size = size;

    /*
     * Layout in a single physically-contiguous allocation:
     *   [descriptors]      16 * size      @ offset 0
     *   [available ring]   6 + 2*size     @ offset desc_end (2-aligned)
     *   [used ring]        6 + 8*size     @ offset avail_end (4096-aligned)
     */
    uint32_t desc_bytes  = (uint32_t)size * 16;
    uint32_t avail_bytes = 6 + (uint32_t)size * 2;
    uint32_t used_bytes  = 6 + (uint32_t)size * 8;

    /* Align used ring to page boundary as required by spec */
    uint32_t avail_end = desc_bytes + avail_bytes;
    uint32_t used_off  = (avail_end + 4095) & ~4095U;
    uint32_t total     = used_off + used_bytes;

    uint32_t phys = physmem_alloc_region(total, 4096);
    if (!phys) {
        serial_print("[virtio] queue alloc failed\n");
        return -1;
    }
    paging_map_mmio(phys);

    uint8_t *base = (uint8_t *)(uintptr_t)phys;
    memset(base, 0, total);

    vq->desc  = (virtq_desc_t *)(base);
    vq->avail = (virtq_avail_t *)(base + desc_bytes);
    vq->used  = (virtq_used_t *)(base + used_off);
    vq->last_used_idx = 0;
    vq->phys_base  = phys;
    vq->alloc_size = total;

    /* Tell device about the queue */
    vio_w16(cfg, VIRTIO_CMN_Q_SIZE, size);
    vio_w32(cfg, VIRTIO_CMN_Q_DESC_LO,  phys);
    vio_w32(cfg, VIRTIO_CMN_Q_DESC_HI,  0);
    vio_w32(cfg, VIRTIO_CMN_Q_AVAIL_LO, phys + desc_bytes);
    vio_w32(cfg, VIRTIO_CMN_Q_AVAIL_HI, 0);
    vio_w32(cfg, VIRTIO_CMN_Q_USED_LO,  phys + used_off);
    vio_w32(cfg, VIRTIO_CMN_Q_USED_HI,  0);

    /* Disable MSI-X for this queue */
    vio_w16(cfg, VIRTIO_CMN_Q_MSIX_VEC, 0xFFFF);

    /* Compute notify address */
    uint16_t notify_off = vio_r16(cfg, VIRTIO_CMN_Q_NOTIFY_OFF);
    vq->notify_addr = (volatile uint16_t *)(
        dev->notify_base + (uint32_t)notify_off * dev->notify_off_multiplier);

    /* Enable the queue */
    vio_w16(cfg, VIRTIO_CMN_Q_ENABLE, 1);

    serial_print("[virtio] queue ready, size=");
    serial_print_int((int)size);
    serial_print("\n");
    return 0;
}

/* ================================================================== */
/*  Synchronous command: send request+response, poll for completion     */
/* ================================================================== */

int virtio_send_cmd(virtqueue_t *vq,
                    void *cmd, uint32_t cmd_len,
                    void *resp, uint32_t resp_len)
{
    /*
     * Two-descriptor chain:
     *   d0 (device-readable)  = command
     *   d1 (device-writable)  = response
     *
     * We always use descriptors 0,1 (synchronous, one command at a time).
     */
    vq->desc[0].addr  = (uint64_t)(uintptr_t)cmd;
    vq->desc[0].len   = cmd_len;
    vq->desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[0].next  = 1;

    vq->desc[1].addr  = (uint64_t)(uintptr_t)resp;
    vq->desc[1].len   = resp_len;
    vq->desc[1].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[1].next  = 0;

    /* Add chain head to available ring */
    vq->avail->ring[vq->avail->idx % vq->size] = 0;
    __asm__ volatile("mfence" ::: "memory");
    vq->avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    /* Notify device */
    *vq->notify_addr = 0;

    /* Poll until device processes the request */
    uint32_t timeout = 10000000;
    while (vq->used->idx == vq->last_used_idx) {
        __asm__ volatile("pause" ::: "memory");
        if (--timeout == 0) {
            serial_print("[virtio] command timeout\n");
            return -1;
        }
    }
    vq->last_used_idx = vq->used->idx;

    return 0;
}

/* ================================================================== */
/*  Send two commands in one batch (single notify + single poll)       */
/* ================================================================== */

int virtio_send_cmd_pair(virtqueue_t *vq,
                         void *cmd1, uint32_t cmd1_len,
                         void *resp1, uint32_t resp1_len,
                         void *cmd2, uint32_t cmd2_len,
                         void *resp2, uint32_t resp2_len)
{
    uint16_t avail_start;

    /* Chain 1: descriptors 0→1 */
    vq->desc[0].addr  = (uint64_t)(uintptr_t)cmd1;
    vq->desc[0].len   = cmd1_len;
    vq->desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[0].next  = 1;

    vq->desc[1].addr  = (uint64_t)(uintptr_t)resp1;
    vq->desc[1].len   = resp1_len;
    vq->desc[1].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[1].next  = 0;

    /* Chain 2: descriptors 2→3 */
    vq->desc[2].addr  = (uint64_t)(uintptr_t)cmd2;
    vq->desc[2].len   = cmd2_len;
    vq->desc[2].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[2].next  = 3;

    vq->desc[3].addr  = (uint64_t)(uintptr_t)resp2;
    vq->desc[3].len   = resp2_len;
    vq->desc[3].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[3].next  = 0;

    /* Add both chain heads to available ring */
    avail_start = vq->avail->idx;
    vq->avail->ring[avail_start % vq->size] = 0;       /* chain 1 head */
    vq->avail->ring[(avail_start + 1) % vq->size] = 2;  /* chain 2 head */
    __asm__ volatile("mfence" ::: "memory");
    vq->avail->idx = avail_start + 2;
    __asm__ volatile("mfence" ::: "memory");

    /* Single notify */
    *vq->notify_addr = 0;

    /* Wait for both completions */
    {
        uint16_t target = (uint16_t)(vq->last_used_idx + 2);
        uint32_t timeout = 10000000;
        while (vq->used->idx != target) {
            __asm__ volatile("pause" ::: "memory");
            if (--timeout == 0) {
                serial_print("[virtio] cmd_pair timeout\n");
                vq->last_used_idx = vq->used->idx;
                return -1;
            }
        }
        vq->last_used_idx = target;
    }

    return 0;
}

/* ================================================================== */
/*  Finalize: set DRIVER_OK after queues are configured                */
/* ================================================================== */

void virtio_driver_ok(virtio_device_t *dev)
{
    uint8_t s = vio_r8(dev->common_cfg, VIRTIO_CMN_STATUS);
    vio_w8(dev->common_cfg, VIRTIO_CMN_STATUS,
           s | VIRTIO_STATUS_DRIVER_OK);
    serial_print("[virtio] status = DRIVER_OK\n");
}
