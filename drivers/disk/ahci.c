/* ============================================================
 *  AHCI (Advanced Host Controller Interface) SATA driver
 *
 *  Detects AHCI controllers via PCI (class 01h, subclass 06h),
 *  enumerates ports, issues IDENTIFY DEVICE, and registers each
 *  found SATA drive with the blkdev layer as "sd0", "sd1", etc.
 *
 *  Supports LBA48 read/write via DMA (READ/WRITE DMA EXT).
 * ============================================================ */

#include "ahci.h"
#include "blkdev.h"
#include "pci.h"
#include "paging.h"
#include "physmem.h"
#include "heap.h"
#include "string.h"
#include "klog.h"

/* ── Port CMD bits ──────────────────────────────────────────────── */
#define HBA_PxCMD_ST   0x0001U
#define HBA_PxCMD_FRE  0x0010U
#define HBA_PxCMD_FR   0x4000U
#define HBA_PxCMD_CR   0x8000U

/* ── GHC bits ───────────────────────────────────────────────────── */
#define HBA_GHC_AE     0x80000000U   /* AHCI Enable */

/* ── Port TFD bits ──────────────────────────────────────────────── */
#define HBA_PxTFD_BSY  0x80U
#define HBA_PxTFD_DRQ  0x08U
#define HBA_PxTFD_ERR  0x01U

/* ── Port SSTS (SStatus) ───────────────────────────────────────── */
#define HBA_PxSSTS_DET_MASK 0x0FU
#define HBA_PxSSTS_DET_OK   0x03U    /* device present, Phy comms established */
#define HBA_PxSSTS_IPM_MASK 0xF00U
#define HBA_PxSSTS_IPM_ACT  0x100U   /* interface active */

/* ── Port IS error bits (subset) ────────────────────────────────── */
#define HBA_PxIS_TFES  (1U << 30)    /* Task File Error Status */

/* ── Timeouts ───────────────────────────────────────────────────── */
#define AHCI_SPIN_TIMEOUT   1000000U  /* iterations for busy-wait loops */
#define AHCI_CMD_TIMEOUT   10000000U  /* iterations for command completion */

/* ── Module state ───────────────────────────────────────────────── */
static ahci_hba_mem_t* hba = 0;
static int             ahci_available = 0;
static ahci_drive_info_t  drives[AHCI_MAX_DRIVES];
static int                drive_count = 0;

/*
 * Per-port DMA memory:  command list (1 KiB), FIS receive (256 B),
 * and one command table (256 B header + PRDTs).
 *
 * We allocate a single 4 KiB page per active port.  Layout inside
 * the page (offsets):
 *   0x000 – 0x3FF  Command list   (32 × 32 B = 1024 B)
 *   0x400 – 0x4FF  FIS receive    (256 B)
 *   0x500 – 0x6FF  Command table  (128 B CFIS+ACMD+rsv + 8×16 B PRDT = 256 B)
 *   0x700 – 0xFFF  scratch / unused
 */
#define PORT_MEM_CLB_OFF  0x000U
#define PORT_MEM_FB_OFF   0x400U
#define PORT_MEM_CTBA_OFF 0x500U

static uint32_t port_dma_phys[AHCI_MAX_PORTS];  /* physical of each page */

/* ── Helpers ────────────────────────────────────────────────────── */

static void io_delay(void) {
    volatile int x = 0;
    (void)x;
}

static int port_is_sata(ahci_port_regs_t* port) {
    uint32_t ssts = port->ssts;
    if ((ssts & HBA_PxSSTS_DET_MASK) != HBA_PxSSTS_DET_OK)  return 0;
    if ((ssts & HBA_PxSSTS_IPM_MASK) != HBA_PxSSTS_IPM_ACT) return 0;
    return 1;
}

/* Stop command engine on a port. */
static void port_stop(ahci_port_regs_t* port) {
    uint32_t timeout;

    port->cmd &= ~HBA_PxCMD_ST;

    timeout = AHCI_SPIN_TIMEOUT;
    while ((port->cmd & HBA_PxCMD_CR) && --timeout)
        io_delay();

    port->cmd &= ~HBA_PxCMD_FRE;

    timeout = AHCI_SPIN_TIMEOUT;
    while ((port->cmd & HBA_PxCMD_FR) && --timeout)
        io_delay();
}

/* Start command engine on a port. */
static void port_start(ahci_port_regs_t* port) {
    uint32_t timeout = AHCI_SPIN_TIMEOUT;
    while ((port->tfd & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)) && --timeout)
        io_delay();

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

/* Set up DMA structures for one port. */
static int port_setup_dma(int port_no) {
    ahci_port_regs_t* port = &hba->ports[port_no];
    uint32_t phys;
    uint8_t* base;

    phys = physmem_alloc_frame();
    if (phys == 0) return -1;
    port_dma_phys[port_no] = phys;

    base = (uint8_t*)(uintptr_t)phys;
    memset(base, 0, PHYSMEM_FRAME_SIZE);

    /* Command list base */
    port->clb  = phys + PORT_MEM_CLB_OFF;
    port->clbu = 0;

    /* FIS receive base */
    port->fb   = phys + PORT_MEM_FB_OFF;
    port->fbu  = 0;

    /* Point command header 0 to the command table area */
    {
        ahci_cmd_header_t* hdr = (ahci_cmd_header_t*)(base + PORT_MEM_CLB_OFF);
        hdr[0].ctba  = phys + PORT_MEM_CTBA_OFF;
        hdr[0].ctbau = 0;
    }

    return 0;
}

/* Find a free command slot. We only use slot 0 (single-threaded). */
static int port_find_slot(ahci_port_regs_t* port) {
    (void)port;
    return 0;
}

/* Wait for a busy port to become ready. */
static int port_wait_ready(ahci_port_regs_t* port) {
    uint32_t timeout = AHCI_SPIN_TIMEOUT;
    while ((port->tfd & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)) && --timeout)
        io_delay();
    return timeout ? 0 : -1;
}

/* Issue a command on slot 0 and wait for completion. */
static int port_issue_cmd(ahci_port_regs_t* port) {
    uint32_t timeout;

    /* Clear any pending interrupt status */
    port->is = (uint32_t)-1;

    /* Issue command on slot 0 */
    port->ci = 1U;

    /* Poll for completion */
    timeout = AHCI_CMD_TIMEOUT;
    while (--timeout) {
        if ((port->ci & 1U) == 0) break;
        if (port->is & HBA_PxIS_TFES)  return -1;
        io_delay();
    }

    if (timeout == 0) return -1;
    if (port->is & HBA_PxIS_TFES) return -1;

    return 0;
}

/* ── IDENTIFY DEVICE ────────────────────────────────────────────── */

static int port_identify(int port_no, ahci_drive_info_t* info) {
    ahci_port_regs_t* port = &hba->ports[port_no];
    uint32_t phys = port_dma_phys[port_no];
    uint8_t* base = (uint8_t*)(uintptr_t)phys;
    ahci_cmd_header_t* hdr;
    ahci_cmd_table_t*  tbl;
    fis_reg_h2d_t*     fis;
    uint32_t           id_phys;
    uint16_t*          id;
    int i;

    if (port_wait_ready(port) < 0) return -1;

    /* Allocate a page for the IDENTIFY data (512 bytes) */
    id_phys = physmem_alloc_frame();
    if (id_phys == 0) return -1;
    memset((void*)(uintptr_t)id_phys, 0, 512);

    /* Set up command header */
    hdr = (ahci_cmd_header_t*)(base + PORT_MEM_CLB_OFF);
    memset(hdr, 0, sizeof(*hdr));
    hdr->cfis_length = sizeof(fis_reg_h2d_t) / 4;
    hdr->write = 0;
    hdr->prdtl = 1;
    hdr->ctba  = phys + PORT_MEM_CTBA_OFF;
    hdr->ctbau = 0;

    /* Set up command table */
    tbl = (ahci_cmd_table_t*)(base + PORT_MEM_CTBA_OFF);
    memset(tbl, 0, sizeof(*tbl));

    /* PRDT entry: point to the identify buffer */
    tbl->prdt[0].dba  = id_phys;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;
    tbl->prdt[0].i    = 0;

    /* Build FIS */
    fis = (fis_reg_h2d_t*)tbl->cfis;
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;

    if (port_issue_cmd(port) < 0) {
        physmem_free_frame(id_phys);
        return -1;
    }

    /* Parse IDENTIFY data */
    id = (uint16_t*)(uintptr_t)id_phys;

    info->port = port_no;

    /* Sector count: words 100-103 = LBA48 capacity.
       We cap to 32-bit for our blkdev layer. */
    {
        uint64_t lba48 = (uint64_t)id[100]
                       | ((uint64_t)id[101] << 16)
                       | ((uint64_t)id[102] << 32)
                       | ((uint64_t)id[103] << 48);
        if (lba48 == 0) {
            /* Fallback to LBA28: words 60-61 */
            lba48 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        }
        if (lba48 > 0xFFFFFFFFULL)
            info->sector_count = 0xFFFFFFFFU;
        else
            info->sector_count = (uint32_t)lba48;
    }

    /* Model string: words 27-46, byte-swapped */
    for (i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        info->model[i * 2]     = (char)(w >> 8);
        info->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    info->model[40] = '\0';

    /* Trim trailing spaces */
    for (i = 39; i >= 0 && info->model[i] == ' '; i--)
        info->model[i] = '\0';

    physmem_free_frame(id_phys);
    return 0;
}

/* ── DMA read/write core ────────────────────────────────────────── */

static int ahci_port_rw(int port_no, uint64_t lba, uint32_t count,
                         uint8_t* buf, int is_write)
{
    ahci_port_regs_t*  port = &hba->ports[port_no];
    uint32_t           phys = port_dma_phys[port_no];
    uint8_t*           base = (uint8_t*)(uintptr_t)phys;
    ahci_cmd_header_t* hdr;
    ahci_cmd_table_t*  tbl;
    fis_reg_h2d_t*     fis;
    uint32_t           buf_phys;
    uint32_t           bytes;

    if (port_wait_ready(port) < 0) return -1;

    /* We need the physical address of buf.  In our identity-map
       kernel, virtual == physical for kernel buffers. */
    buf_phys = (uint32_t)(uintptr_t)buf;
    bytes    = count * 512;

    /* Set up command header */
    hdr = (ahci_cmd_header_t*)(base + PORT_MEM_CLB_OFF);
    memset(hdr, 0, sizeof(*hdr));
    hdr->cfis_length = sizeof(fis_reg_h2d_t) / 4;
    hdr->write       = is_write ? 1 : 0;
    hdr->prdtl       = 1;
    hdr->ctba        = phys + PORT_MEM_CTBA_OFF;
    hdr->ctbau       = 0;

    /* Set up command table */
    tbl = (ahci_cmd_table_t*)(base + PORT_MEM_CTBA_OFF);
    memset(tbl, 0, sizeof(*tbl));

    /* PRDT: single entry (max 4 MB per PRDT entry, we limit to 128 sectors = 64 KB) */
    tbl->prdt[0].dba  = buf_phys;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = bytes - 1;
    tbl->prdt[0].i    = 0;

    /* Build FIS */
    fis = (fis_reg_h2d_t*)tbl->cfis;
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    fis->device   = 1 << 6;  /* LBA mode */

    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->count = (uint16_t)count;

    return port_issue_cmd(port);
}

/* ── blkdev ops callbacks ───────────────────────────────────────── */

/* Private data = drive index (stored as pointer-sized int). */

static int ahci_blkdev_read(void* priv, uint32_t lba, uint32_t count, uint8_t* buf) {
    int drive_idx = (int)(uintptr_t)priv;
    uint32_t done = 0;
    int port_no;

    if (drive_idx < 0 || drive_idx >= drive_count) return -1;
    port_no = drives[drive_idx].port;

    /* Process in chunks of up to 128 sectors (64 KB) to stay within
       a single PRDT entry's comfortable range. */
    while (count > 0) {
        uint32_t chunk = (count > 128) ? 128 : count;
        if (ahci_port_rw(port_no, (uint64_t)lba, chunk, buf, 0) < 0)
            return (done > 0) ? (int)done : -1;
        done  += chunk;
        lba   += chunk;
        buf   += chunk * 512;
        count -= chunk;
    }
    return (int)done;
}

static int ahci_blkdev_write(void* priv, uint32_t lba, uint32_t count, const uint8_t* buf) {
    int drive_idx = (int)(uintptr_t)priv;
    uint32_t done = 0;
    int port_no;

    if (drive_idx < 0 || drive_idx >= drive_count) return -1;
    port_no = drives[drive_idx].port;

    while (count > 0) {
        uint32_t chunk = (count > 128) ? 128 : count;
        /* Cast away const: AHCI DMA reads from buf, never modifies it. */
        if (ahci_port_rw(port_no, (uint64_t)lba, chunk, (uint8_t*)buf, 1) < 0)
            return (done > 0) ? (int)done : -1;
        done  += chunk;
        lba   += chunk;
        buf   += chunk * 512;
        count -= chunk;
    }
    return (int)done;
}

/* ── Initialization ─────────────────────────────────────────────── */

void ahci_init(void) {
    const pci_device_t* dev;
    uint32_t bar5;
    uint32_t pi;
    int      port_no;
    int      num_ports;

    drive_count    = 0;
    ahci_available = 0;
    memset(drives, 0, sizeof(drives));
    memset(port_dma_phys, 0, sizeof(port_dma_phys));

    /* Find AHCI controller: PCI class 01h (Mass Storage), subclass 06h (SATA) */
    dev = pci_find_class(0x01, 0x06);
    if (!dev) {
        klog_write(KLOG_LEVEL_INFO, "ahci", "No AHCI controller found");
        return;
    }

    klog_write(KLOG_LEVEL_INFO, "ahci", "AHCI controller detected via PCI");

    /* Enable bus mastering for DMA */
    pci_enable_bus_mastering(dev);

    /* BAR5 = AHCI Base Memory Register (ABAR) */
    bar5 = dev->bar[5];
    if (bar5 == 0) {
        klog_write(KLOG_LEVEL_ERROR, "ahci", "BAR5 is zero");
        return;
    }

    /* Mask off lower bits (it's an MMIO BAR) */
    bar5 &= 0xFFFFF000U;

    /* Ensure the MMIO region is mapped in our page tables */
    if (!paging_map_mmio((uintptr_t)bar5)) {
        klog_write(KLOG_LEVEL_ERROR, "ahci", "Failed to map AHCI MMIO");
        return;
    }

    hba = (ahci_hba_mem_t*)(uintptr_t)bar5;

    /* Enable AHCI mode */
    hba->ghc |= HBA_GHC_AE;

    /* Read port implemented bitmap */
    pi = hba->pi;
    num_ports = 0;

    for (port_no = 0; port_no < AHCI_MAX_PORTS && drive_count < AHCI_MAX_DRIVES; port_no++) {
        ahci_port_regs_t* port;
        ahci_drive_info_t info;
        char name[BLKDEV_NAME_MAX];
        blkdev_ops_t ops;
        int idx;

        if (!(pi & (1U << port_no))) continue;

        port = &hba->ports[port_no];

        if (!port_is_sata(port)) continue;

        /* Only handle SATA drives (signature 0x00000101) */
        if (port->sig != SATA_SIG_ATA) continue;

        num_ports++;

        /* Stop port before reconfiguring DMA buffers */
        port_stop(port);

        /* Allocate and assign DMA structures */
        if (port_setup_dma(port_no) < 0) {
            klog_write(KLOG_LEVEL_WARN, "ahci", "Failed to alloc DMA for port");
            continue;
        }

        /* Clear error register */
        port->serr = (uint32_t)-1;

        /* Start port */
        port_start(port);

        /* IDENTIFY the drive */
        memset(&info, 0, sizeof(info));
        if (port_identify(port_no, &info) < 0) {
            klog_write(KLOG_LEVEL_WARN, "ahci", "IDENTIFY failed on port");
            port_stop(port);
            continue;
        }

        if (info.sector_count == 0) {
            port_stop(port);
            continue;
        }

        /* Store drive info */
        drives[drive_count] = info;

        /* Register with blkdev as "sd0", "sd1", etc. */
        name[0] = 's';
        name[1] = 'd';
        name[2] = (char)('0' + drive_count);
        name[3] = '\0';

        ops.read  = ahci_blkdev_read;
        ops.write = ahci_blkdev_write;

        idx = blkdev_register(name, 512, info.sector_count,
                              0, -1, ops, (void*)(uintptr_t)drive_count);
        if (idx >= 0) {
            klog_write(KLOG_LEVEL_INFO, "ahci", "Drive registered");
            blkdev_probe_partitions(idx);
        }

        drive_count++;
    }

    if (drive_count > 0) {
        ahci_available = 1;
        klog_write(KLOG_LEVEL_INFO, "ahci", "AHCI init complete");
    } else {
        klog_write(KLOG_LEVEL_INFO, "ahci", "No SATA drives found");
    }
}

int ahci_is_available(void) {
    return ahci_available;
}

int ahci_drive_count(void) {
    return drive_count;
}

const ahci_drive_info_t* ahci_get_drive(int index) {
    if (index < 0 || index >= drive_count) return 0;
    return &drives[index];
}

int ahci_read(int drive_index, uint32_t lba, uint32_t count, uint8_t* buf) {
    return ahci_blkdev_read((void*)(uintptr_t)drive_index, lba, count, buf);
}

int ahci_write(int drive_index, uint32_t lba, uint32_t count, const uint8_t* buf) {
    return ahci_blkdev_write((void*)(uintptr_t)drive_index, lba, count, buf);
}
