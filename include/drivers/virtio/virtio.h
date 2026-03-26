#ifndef DRIVERS_VIRTIO_H
#define DRIVERS_VIRTIO_H

#include <stdint.h>
#include "pci.h"

/* ====================================================================
 *  Virtio PCI Transport  (modern / v1.0+)
 *
 *  Implements the transport layer: PCI capability parsing, device
 *  initialization, split-virtqueue management, and synchronous
 *  command submission.
 * ==================================================================== */

/* ---- PCI vendor / device IDs ---- */
#define VIRTIO_PCI_VENDOR         0x1AF4
#define VIRTIO_PCI_DEVICE_GPU     0x1050   /* virtio-gpu (modern) */

/* ---- PCI capability types ---- */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/* ---- Device status bits ---- */
#define VIRTIO_STATUS_ACKNOWLEDGE     1
#define VIRTIO_STATUS_DRIVER          2
#define VIRTIO_STATUS_DRIVER_OK       4
#define VIRTIO_STATUS_FEATURES_OK     8
#define VIRTIO_STATUS_FAILED        128

/* ---- Descriptor flags ---- */
#define VIRTQ_DESC_F_NEXT    1   /* chained via 'next' field */
#define VIRTQ_DESC_F_WRITE   2   /* device writes (response) */

/* ---- Common configuration register offsets ---- */
#define VIRTIO_CMN_DFSELECT     0x00  /* u32: device feature select   */
#define VIRTIO_CMN_DF           0x04  /* u32: device feature bits     */
#define VIRTIO_CMN_GFSELECT     0x08  /* u32: guest feature select    */
#define VIRTIO_CMN_GF           0x0C  /* u32: guest feature bits      */
#define VIRTIO_CMN_MSIX_CFG     0x10  /* u16: MSI-X config vector     */
#define VIRTIO_CMN_NUM_QUEUES   0x12  /* u16: max queues              */
#define VIRTIO_CMN_STATUS       0x14  /* u8:  device status           */
#define VIRTIO_CMN_CFGGEN       0x15  /* u8:  config generation       */
#define VIRTIO_CMN_Q_SELECT     0x16  /* u16: queue select            */
#define VIRTIO_CMN_Q_SIZE       0x18  /* u16: queue size              */
#define VIRTIO_CMN_Q_MSIX_VEC   0x1A  /* u16: queue msix vector       */
#define VIRTIO_CMN_Q_ENABLE     0x1C  /* u16: queue enable            */
#define VIRTIO_CMN_Q_NOTIFY_OFF 0x1E  /* u16: queue notify offset     */
#define VIRTIO_CMN_Q_DESC_LO    0x20  /* u32: desc table addr lo      */
#define VIRTIO_CMN_Q_DESC_HI    0x24  /* u32: desc table addr hi      */
#define VIRTIO_CMN_Q_AVAIL_LO   0x28  /* u32: available ring addr lo  */
#define VIRTIO_CMN_Q_AVAIL_HI   0x2C  /* u32: available ring addr hi  */
#define VIRTIO_CMN_Q_USED_LO    0x30  /* u32: used ring addr lo       */
#define VIRTIO_CMN_Q_USED_HI    0x34  /* u32: used ring addr hi       */

/* ---- Split virtqueue descriptor ---- */
typedef struct {
    uint64_t addr;       /* physical address of buffer */
    uint32_t len;        /* buffer length              */
    uint16_t flags;      /* VIRTQ_DESC_F_*             */
    uint16_t next;       /* next descriptor if NEXT    */
} __attribute__((packed)) virtq_desc_t;

/* ---- Available ring ---- */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) virtq_avail_t;

/* ---- Used ring element ---- */
typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

/* ---- Virtqueue state ---- */
typedef struct {
    uint16_t       size;            /* number of descriptors       */
    uint16_t       last_used_idx;   /* last seen used->idx         */
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;
    volatile uint16_t *notify_addr; /* notify doorbell             */
    uint32_t       phys_base;       /* physical base of ring alloc */
    uint32_t       alloc_size;      /* total allocation size       */
} virtqueue_t;

/* ---- Virtio device ---- */
#define VIRTIO_MAX_QUEUES 4

typedef struct {
    const pci_device_t *pci;
    /* MMIO pointers from PCI capabilities */
    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    volatile uint8_t *isr_cfg;
    volatile uint8_t *device_cfg;
    uint32_t          notify_off_multiplier;
    /* Queues */
    virtqueue_t       queues[VIRTIO_MAX_QUEUES];
    int               num_queues;
} virtio_device_t;

/* ---- MMIO access helpers ---- */
static inline void vio_w8(volatile uint8_t *base, uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(base + off) = v;
}
static inline void vio_w16(volatile uint8_t *base, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(base + off) = v;
}
static inline void vio_w32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}
static inline uint8_t vio_r8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}
static inline uint16_t vio_r16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}
static inline uint32_t vio_r32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

/* ---- API ---- */
int  virtio_pci_probe(virtio_device_t *dev, uint16_t device_id);
int  virtio_init_device(virtio_device_t *dev);
int  virtio_alloc_queue(virtio_device_t *dev, int qidx);
int  virtio_send_cmd(virtqueue_t *vq,
                     void *cmd, uint32_t cmd_len,
                     void *resp, uint32_t resp_len);
int  virtio_send_cmd_pair(virtqueue_t *vq,
                          void *cmd1, uint32_t cmd1_len,
                          void *resp1, uint32_t resp1_len,
                          void *cmd2, uint32_t cmd2_len,
                          void *resp2, uint32_t resp2_len);
void virtio_driver_ok(virtio_device_t *dev);

#endif /* DRIVERS_VIRTIO_H */
