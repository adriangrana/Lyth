/* ================================================================
 * Lyth OS — xHCI (USB 3.x) Host Controller Driver
 *
 * Discovers the xHCI controller via PCI, takes ownership from BIOS,
 * initialises rings and DCBAA, then enumerates connected ports.
 * ================================================================ */

#include "xhci.h"
#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "physmem.h"
#include "heap.h"
#include "serial.h"
#include "klog.h"
#include "string.h"
#include "apic.h"
#include "terminal.h"

/* ── I/O helpers ────────────────────────────────────────────────── */

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static inline void mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

/* ── Singleton controller state ─────────────────────────────────── */

static xhci_controller_t hc;

xhci_controller_t* xhci_get_controller(void) {
    return &hc;
}

/* ── MMIO access helpers ────────────────────────────────────────── */

static uint32_t op_read32(uint32_t offset) {
    return hc.op_base[offset / 4];
}

static void op_write32(uint32_t offset, uint32_t val) {
    hc.op_base[offset / 4] = val;
}

static uint64_t op_read64(uint32_t offset) {
    uint32_t lo = hc.op_base[offset / 4];
    uint32_t hi = hc.op_base[offset / 4 + 1];
    return ((uint64_t)hi << 32) | lo;
}

static void op_write64(uint32_t offset, uint64_t val) {
    hc.op_base[offset / 4]     = (uint32_t)(val & 0xFFFFFFFF);
    hc.op_base[offset / 4 + 1] = (uint32_t)(val >> 32);
}

static uint32_t port_read32(int port_index) {
    return hc.port_base[(port_index * 4)]; /* 0x10 bytes = 4 dwords per port */
}

static void port_write32(int port_index, uint32_t val) {
    hc.port_base[(port_index * 4)] = val;
}

static uint32_t rt_read32(uint32_t offset) {
    return hc.rt_base[offset / 4];
}

static void rt_write32(uint32_t offset, uint32_t val) {
    hc.rt_base[offset / 4] = val;
}

static void rt_write64(uint32_t offset, uint64_t val) {
    hc.rt_base[offset / 4]     = (uint32_t)(val & 0xFFFFFFFF);
    hc.rt_base[offset / 4 + 1] = (uint32_t)(val >> 32);
}

static void db_write(int slot, uint32_t val) {
    hc.db_base[slot] = val;
    /* Read-back to flush PCIe posted writes (same pattern as Linux xHCI) */
    (void)hc.db_base[slot];
}

/* ── Delay ──────────────────────────────────────────────────────── */

static void delay_ms(int ms) {
    for (int i = 0; i < ms * 1000; i++)
        io_wait();
}

/* ── Small helpers ──────────────────────────────────────────────── */

static uint64_t trb_ptr64(uint32_t lo, uint32_t hi) {
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint32_t ring_trb_phys(const xhci_ring_t* ring, uint32_t index) {
    return ring->trbs_phys + index * sizeof(xhci_trb_t);
}

/* ── Ring management ────────────────────────────────────────────── */

static int ring_alloc(xhci_ring_t* ring, uint32_t num_trbs) {
    uint32_t size = num_trbs * sizeof(xhci_trb_t);
    uint32_t phys = physmem_alloc_region(size, 64);
    if (!phys) return -1;
    paging_map_mmio(phys);

    ring->trbs      = (xhci_trb_t*)(uintptr_t)phys;
    ring->trbs_phys = phys;
    ring->enqueue   = 0;
    ring->dequeue   = 0;
    ring->cycle     = 1;
    ring->size      = num_trbs;
    memset(ring->trbs, 0, size);

    /* Set up Link TRB at the end — points back to start, toggles cycle */
    xhci_trb_t* link = &ring->trbs[num_trbs - 1];
    link->param_lo = phys;
    link->param_hi = 0;
    link->status   = 0;
    link->control  = XHCI_TRB_TYPE(XHCI_TRB_LINK) | (1U << 1); /* Toggle Cycle */

    return 0;
}

static void ring_enqueue_trb(xhci_ring_t* ring, uint32_t param_lo, uint32_t param_hi,
                              uint32_t status, uint32_t control) {
    xhci_trb_t* trb = &ring->trbs[ring->enqueue];

    trb->param_lo = param_lo;
    trb->param_hi = param_hi;
    trb->status   = status;
    /* Set cycle bit according to producer cycle state */
    trb->control  = control | (ring->cycle & 1);

    mb();

    ring->enqueue++;
    if (ring->enqueue >= ring->size - 1) {
        /* Wrap: update Link TRB cycle bit then flip */
        xhci_trb_t* link = &ring->trbs[ring->size - 1];
        link->control = (link->control & ~1U) | (ring->cycle & 1);
        mb();
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
}

/* ── BIOS/OS handoff ────────────────────────────────────────────── */

static void xhci_bios_handoff(const pci_device_t* dev) {
    uint32_t hccparams1 = hc.cap_base[0x10 / 4];
    uint32_t xecp_off = XHCI_HCC1_XECP(hccparams1);
    if (xecp_off == 0) return;

    volatile uint32_t* ecap = (volatile uint32_t*)((uintptr_t)hc.cap_base + (xecp_off * 4));

    /* Walk Extended Capability list */
    for (int i = 0; i < 100; i++) {
        uint32_t val = *ecap;
        uint8_t cap_id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;

        if (cap_id == XHCI_EXT_CAP_LEGACY) {
            serial_print("[xhci] BIOS handoff: legacy cap found\n");

            /* Set OS Owned semaphore */
            volatile uint32_t* legsup = ecap;
            *legsup = val | XHCI_USBLEGSUP_OS_OWNED;
            mb();

            /* Wait for BIOS to release (up to 1 second) */
            for (int j = 0; j < 100; j++) {
                if (!(*legsup & XHCI_USBLEGSUP_BIOS_OWNED))
                    break;
                delay_ms(10);
            }

            if (*legsup & XHCI_USBLEGSUP_BIOS_OWNED) {
                serial_print("[xhci] WARN: BIOS did not release ownership\n");
                /* Force clear BIOS ownership */
                *legsup = (*legsup & ~XHCI_USBLEGSUP_BIOS_OWNED) | XHCI_USBLEGSUP_OS_OWNED;
                mb();
            }

            /* Disable BIOS SMI generation */
            volatile uint32_t* legctlsts = ecap + 1;
            *legctlsts = 0;
            mb();

            serial_print("[xhci] BIOS handoff complete\n");
            return;
        }

        if (next == 0) break;
        ecap = (volatile uint32_t*)((uintptr_t)ecap + (next * 4));
    }
}

/* ── Controller reset ───────────────────────────────────────────── */

static int xhci_reset(void) {
    /* Stop the controller */
    uint32_t cmd = op_read32(XHCI_OP_USBCMD);
    op_write32(XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);
    mb();

    /* Wait for halt (HCH=1) */
    for (int i = 0; i < 100; i++) {
        if (op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH)
            break;
        delay_ms(1);
    }
    if (!(op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        serial_print("[xhci] WARN: controller did not halt\n");
    }

    /* Issue reset */
    op_write32(XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    mb();

    /* Wait for reset to complete (HCRST=0 and CNR=0) */
    for (int i = 0; i < 200; i++) {
        uint32_t cmd2 = op_read32(XHCI_OP_USBCMD);
        uint32_t sts = op_read32(XHCI_OP_USBSTS);
        if (!(cmd2 & XHCI_CMD_HCRST) && !(sts & XHCI_STS_CNR))
            return 0;
        delay_ms(1);
    }

    serial_print("[xhci] ERROR: controller reset timeout\n");
    return -1;
}

/* ── Scratchpad allocation ──────────────────────────────────────── */

static int xhci_alloc_scratchpad(void) {
    if (hc.scratchpad_count == 0) return 0;

    uint32_t array_size = hc.scratchpad_count * sizeof(uint64_t);
    /* Align to 64 bytes */
    uint32_t aligned_size = (array_size + 63) & ~63U;
    uint32_t phys = physmem_alloc_region(aligned_size, 64);
    if (!phys) return -1;
    paging_map_mmio(phys);

    hc.scratchpad_array = (uint64_t*)(uintptr_t)phys;
    hc.scratchpad_array_phys = phys;
    memset(hc.scratchpad_array, 0, aligned_size);

    /* Allocate individual scratchpad pages */
    for (uint32_t i = 0; i < hc.scratchpad_count; i++) {
        uint32_t page_phys = physmem_alloc_frame();
        if (!page_phys) return -1;
        paging_map_mmio(page_phys);
        memset((void*)(uintptr_t)page_phys, 0, PHYSMEM_FRAME_SIZE);
        hc.scratchpad_array[i] = (uint64_t)page_phys;
    }

    /* DCBAA[0] points to the scratchpad buffer array */
    hc.dcbaa[0] = (uint64_t)hc.scratchpad_array_phys;

    serial_print("[xhci] Allocated ");
    serial_print_uint(hc.scratchpad_count);
    serial_print(" scratchpad buffers\n");
    return 0;
}

/* ── DCBAA allocation ───────────────────────────────────────────── */

static int xhci_alloc_dcbaa(void) {
    uint32_t size = (hc.max_slots + 1) * sizeof(uint64_t);
    uint32_t aligned = (size + 63) & ~63U;
    uint32_t phys = physmem_alloc_region(aligned, 64);
    if (!phys) return -1;
    paging_map_mmio(phys);

    hc.dcbaa = (uint64_t*)(uintptr_t)phys;
    hc.dcbaa_phys = phys;
    memset(hc.dcbaa, 0, aligned);

    return 0;
}

/* ── Event Ring setup ───────────────────────────────────────────── */

static int xhci_setup_event_ring(void) {
    /* Allocate event ring TRB segment */
    if (ring_alloc(&hc.evt_ring, XHCI_RING_SIZE) != 0)
        return -1;
    /* Event ring doesn't use Link TRBs normally — just a flat segment.
       Clear the Link TRB we wrote and rely on ERSTSZ wrapping. */
    memset(&hc.evt_ring.trbs[XHCI_RING_SIZE - 1], 0, sizeof(xhci_trb_t));

    /* Allocate ERST (1 segment) */
    uint32_t erst_size = sizeof(xhci_erst_entry_t);
    uint32_t erst_aligned = (erst_size + 63) & ~63U;
    uint32_t erst_phys = physmem_alloc_region(erst_aligned, 64);
    if (!erst_phys) return -1;
    paging_map_mmio(erst_phys);

    hc.erst = (xhci_erst_entry_t*)(uintptr_t)erst_phys;
    hc.erst_phys = erst_phys;
    memset(hc.erst, 0, erst_aligned);

    hc.erst[0].ring_base = (uint64_t)hc.evt_ring.trbs_phys;
    hc.erst[0].ring_size = XHCI_RING_SIZE;
    hc.erst[0].reserved  = 0;

    /* Configure interrupter 0 */
    uint32_t ir0 = 0x20;  /* Interrupter 0 offset from runtime base */
    /* ERSTSZ = 1 segment */
    rt_write32(ir0 + XHCI_IR_ERSTSZ, 1);
    /* ERDP = start of event ring, bit 3 = EHB (clear) */
    rt_write64(ir0 + XHCI_IR_ERDP, (uint64_t)hc.evt_ring.trbs_phys);
    /* ERSTBA = address of ERST (must be written after ERSTSZ) */
    rt_write64(ir0 + XHCI_IR_ERSTBA, (uint64_t)hc.erst_phys);
    /* Enable interrupter */
    rt_write32(ir0 + XHCI_IR_IMAN, XHCI_IMAN_IE);
    /* Moderate: ~4000 interrupts/sec */
    rt_write32(ir0 + XHCI_IR_IMOD, 250);

    return 0;
}

/* ── Wait for command completion event ──────────────────────────── */

/* ── Event ring primitives: peek / consume / wait ───────────────── */

/* Peek at the next event on the ring WITHOUT consuming it.
   Returns 0 if an event is available, -1 if the ring is empty. */
static int xhci_peek_event(xhci_trb_t* out) {
    xhci_trb_t* evt = &hc.evt_ring.trbs[hc.evt_ring.dequeue];
    mb();
    uint32_t cycle_bit = evt->control & XHCI_TRB_CYCLE;
    if (cycle_bit != (hc.evt_ring.cycle & 1))
        return -1;  /* No event pending */
    if (out) *out = *evt;
    return 0;
}

/* Consume the current event: advance dequeue and update ERDP.
   MUST only be called after a successful xhci_peek_event(). */
static void xhci_consume_event(void) {
    hc.evt_ring.dequeue++;
    if (hc.evt_ring.dequeue >= hc.evt_ring.size) {
        hc.evt_ring.dequeue = 0;
        hc.evt_ring.cycle ^= 1;
    }
    uint64_t erdp = (uint64_t)hc.evt_ring.trbs_phys +
                    hc.evt_ring.dequeue * sizeof(xhci_trb_t);
    erdp |= (1ULL << 3); /* EHB = 1 to clear Event Handler Busy */
    rt_write64(0x20 + XHCI_IR_ERDP, erdp);
}

/* Wait up to timeout_ms for any event to appear, then peek it.
   Does NOT consume the event. Returns 0 on success, -1 on timeout. */
static int xhci_wait_any_event(xhci_trb_t* out, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 1000; i++) {
        if (xhci_peek_event(out) == 0)
            return 0;
        io_wait();
    }
    return -1;
}

/* Non-blocking single-check, consume if present. */
int xhci_check_event(xhci_trb_t* out) {
    if (xhci_peek_event(out) != 0)
        return -1;
    xhci_consume_event();
    return 0;
}

/* ── Filtered event waits ───────────────────────────────────────── */

static int xhci_wait_cmd_completion(uint64_t cmd_trb_ptr, xhci_trb_t* out, int timeout_ms) {
    for (int attempt = 0; attempt < 64; attempt++) {
        xhci_trb_t evt;
        if (xhci_wait_any_event(&evt, timeout_ms) != 0)
            return -1;

        uint32_t evt_type = XHCI_TRB_GET_TYPE(evt.control);
        uint64_t ptr = trb_ptr64(evt.param_lo, evt.param_hi);

        /* Always consume — in a single-threaded OS nobody else will drain it */
        xhci_consume_event();

        if (evt_type == XHCI_TRB_CMD_COMPLETION && ptr == cmd_trb_ptr) {
            if (out) *out = evt;
            return 0;
        }
    }
    return -1;
}

static int xhci_wait_transfer_event(int slot, int ep_dci, uint64_t trb_ptr,
                                    xhci_trb_t* out, int timeout_ms) {
    for (int attempt = 0; attempt < 128; attempt++) {
        xhci_trb_t evt;
        if (xhci_wait_any_event(&evt, timeout_ms) != 0)
            return -1;

        uint32_t evt_type = XHCI_TRB_GET_TYPE(evt.control);
        uint64_t ptr = trb_ptr64(evt.param_lo, evt.param_hi);

        /* Always consume — in a single-threaded OS nobody else will drain it */
        xhci_consume_event();

        if (evt_type == XHCI_TRB_TRANSFER_EVENT &&
            XHCI_TRB_SLOT_ID(evt.control) == (uint8_t)slot &&
            XHCI_TRB_EP_ID(evt.control)   == (uint8_t)ep_dci &&
            ptr == trb_ptr) {
            if (out) *out = evt;
            return 0;
        }
    }
    return -1;
}

/* ── Send command and wait for completion ───────────────────────── */

static int xhci_send_command(uint32_t param_lo, uint32_t param_hi,
                              uint32_t status, uint32_t control,
                              xhci_trb_t* result) {
    uint64_t cmd_trb_phys = (uint64_t)ring_trb_phys(&hc.cmd_ring, hc.cmd_ring.enqueue);

    ring_enqueue_trb(&hc.cmd_ring, param_lo, param_hi, status, control);
    mb();
    db_write(0, 0);
    mb();

    xhci_trb_t evt;
    if (xhci_wait_cmd_completion(cmd_trb_phys, &evt, 500) != 0) {
        serial_print("[xhci] Command timeout/no matching completion\n");
        return -1;
    }

    uint8_t cc = XHCI_TRB_COMP_CODE(evt.status);
    if (cc != XHCI_CC_SUCCESS) {
        serial_print("[xhci] Command failed, cc=");
        serial_print_uint(cc);
        serial_print("\n");
        return -1;
    }

    if (result) *result = evt;
    return 0;
}

/* ── Enable Slot ────────────────────────────────────────────────── */

static int xhci_enable_slot(void) {
    xhci_trb_t result;
    if (xhci_send_command(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &result) != 0)
        return -1;

    int slot_id = XHCI_TRB_SLOT_ID(result.control);
    serial_print("[xhci] Enabled slot ");
    serial_print_uint(slot_id);
    serial_print("\n");
    return slot_id;
}

/* ── Allocate device contexts ───────────────────────────────────── */

static int xhci_alloc_device_ctx(xhci_device_t* dev) {
    /* Each context is context_size * 32 entries for output,
       and context_size * 33 entries for input (extra Input Control Context) */
    uint32_t out_size = hc.context_size * 32;
    uint32_t in_size  = hc.context_size * 33;
    uint32_t page_size = PHYSMEM_FRAME_SIZE;

    /* Round up to page boundary for clean allocation */
    uint32_t out_aligned = (out_size + page_size - 1) & ~(page_size - 1);
    uint32_t in_aligned  = (in_size + page_size - 1) & ~(page_size - 1);

    uint32_t out_phys = physmem_alloc_region(out_aligned, 64);
    if (!out_phys) return -1;
    paging_map_mmio(out_phys);
    dev->out_ctx_phys = out_phys;
    dev->out_ctx = (void*)(uintptr_t)out_phys;
    memset(dev->out_ctx, 0, out_aligned);

    uint32_t in_phys = physmem_alloc_region(in_aligned, 64);
    if (!in_phys) return -1;
    paging_map_mmio(in_phys);
    dev->in_ctx_phys = in_phys;
    dev->in_ctx = (void*)(uintptr_t)in_phys;
    memset(dev->in_ctx, 0, in_aligned);

    return 0;
}

/* ── Compute max packet size for EP0 based on device speed ──────── */

static uint16_t ep0_max_packet(int speed) {
    switch (speed) {
        case XHCI_SPEED_LOW:   return 8;
        case XHCI_SPEED_FULL:  return 8;   /* Could be 8, 16, 32, or 64; start with 8 */
        case XHCI_SPEED_HIGH:  return 64;
        case XHCI_SPEED_SUPER: return 512;
        default:               return 8;
    }
}

/* ── Address a device on a port ─────────────────────────────────── */

/* Address a device — supports both root-hub and behind-hub devices.
   parent_slot = 0 for root-hub direct connections.
   parent_slot > 0 for devices behind a hub (slot ID of the hub).
   parent_port = 1-based port on the parent hub.
   route_string = xHCI route string (0 for root-hub). */
static int xhci_address_device_ex(int root_port, int speed,
                                   int parent_slot, int parent_port,
                                   uint32_t route_string) {
    if (hc.device_count >= XHCI_MAX_DEVICES) {
        serial_print("[xhci] Max devices reached\n");
        return -1;
    }

    int slot_id = xhci_enable_slot();
    if (slot_id <= 0) return -1;

    xhci_device_t* dev = &hc.devices[hc.device_count];
    memset(dev, 0, sizeof(xhci_device_t));
    dev->slot_id      = slot_id;
    dev->port         = root_port;
    dev->speed        = speed;
    dev->parent_slot  = parent_slot;
    dev->parent_port  = parent_port;
    dev->route_string = route_string;

    if (xhci_alloc_device_ctx(dev) != 0) return -1;

    /* Allocate EP0 transfer ring */
    if (ring_alloc(&dev->ep_ring[1], XHCI_RING_SIZE) != 0) return -1;

    /* Set DCBAA entry for this slot to point to output context */
    hc.dcbaa[slot_id] = (uint64_t)dev->out_ctx_phys;

    /* Build Input Context for Address Device command */
    uint8_t* in_ptr = (uint8_t*)dev->in_ctx;

    /* Input Control Context */
    xhci_input_ctrl_ctx_t* icc = (xhci_input_ctrl_ctx_t*)in_ptr;
    icc->add_flags  = (1U << 0) | (1U << 1); /* A0 (slot) + A1 (EP0) */
    icc->drop_flags = 0;

    /* Slot Context */
    xhci_slot_ctx_t* slot = (xhci_slot_ctx_t*)(in_ptr + hc.context_size);
    slot->route_speed_entries = (route_string & 0xFFFFF)
                              | ((uint32_t)speed << 20)
                              | (1U << 27);
    /* Root hub port number (1-based) — always the root port, even for hub devices */
    slot->latency_hub_port = ((uint32_t)(root_port + 1) << 16);

    /* If behind a hub, set parent info.
       For FS/LS devices behind a HS hub, the TT fields are needed. */
    if (parent_slot > 0) {
        slot->parent_slot_port = (uint32_t)parent_slot
                               | ((uint32_t)parent_port << 8);
    }

    /* EP0 Context */
    xhci_ep_ctx_t* ep0 = (xhci_ep_ctx_t*)(in_ptr + 2 * hc.context_size);
    uint16_t mps = ep0_max_packet(speed);
    ep0->ep_info2 = (3U << 1) | (XHCI_EP_TYPE_CONTROL << 3) | ((uint32_t)mps << 16);
    ep0->dequeue_ptr = (uint64_t)dev->ep_ring[1].trbs_phys | 1;
    ep0->avg_trb_len = 8;

    mb();

    /* Send Address Device command */
    xhci_trb_t result;
    uint32_t control = XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE)
                     | ((uint32_t)slot_id << 24);
    if (xhci_send_command(dev->in_ctx_phys, 0, 0, control, &result) != 0) {
        serial_print("[xhci] Address Device failed for root_port=");
        serial_print_uint(root_port);
        if (parent_slot) {
            serial_print(" hub_slot=");
            serial_print_uint(parent_slot);
            serial_print(" hub_port=");
            serial_print_uint(parent_port);
        }
        serial_print("\n");
        return -1;
    }

    hc.device_count++;
    serial_print("[xhci] Addressed device slot=");
    serial_print_uint(slot_id);
    serial_print(" speed=");
    serial_print_uint(speed);
    serial_print(" route=");
    serial_print_hex(route_string);
    serial_print("\n");

    return hc.device_count - 1;
}

/* Convenience wrapper for root-hub direct connections */
static int xhci_address_device(int port_index, int speed) {
    return xhci_address_device_ex(port_index, speed, 0, 0, 0);
}

/* ── Control transfer (for enumeration) ─────────────────────────── */

int xhci_control_transfer(int slot, uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                           void* data) {
    /* Find device by slot id */
    xhci_device_t* dev = 0;
    for (int i = 0; i < hc.device_count; i++) {
        if (hc.devices[i].slot_id == slot) {
            dev = &hc.devices[i];
            break;
        }
    }
    if (!dev) return -1;

    xhci_ring_t* ring = &dev->ep_ring[1]; /* EP0 = DCI 1 */

    /* Setup Stage TRB */
    uint32_t setup_lo = (uint32_t)bmRequestType | ((uint32_t)bRequest << 8)
                      | ((uint32_t)wValue << 16);
    uint32_t setup_hi = (uint32_t)wIndex | ((uint32_t)wLength << 16);
    uint32_t setup_status = 8; /* TRB Transfer Length = 8 (setup packet) */
    uint32_t setup_ctrl = XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT;
    /* TRT (Transfer Type): 0 = No Data, 2 = OUT Data, 3 = IN Data */
    if (wLength > 0) {
        if (bmRequestType & USB_REQ_DIR_IN)
            setup_ctrl |= (3U << 16);  /* IN Data Stage */
        else
            setup_ctrl |= (2U << 16);  /* OUT Data Stage */
    }
    ring_enqueue_trb(ring, setup_lo, setup_hi, setup_status, setup_ctrl);

    /* Data Stage TRB (if wLength > 0) */
    if (wLength > 0 && data) {
        uint32_t data_phys = (uint32_t)(uintptr_t)data;
        uint32_t data_ctrl = XHCI_TRB_TYPE(XHCI_TRB_DATA);
        if (bmRequestType & USB_REQ_DIR_IN)
            data_ctrl |= XHCI_TRB_DIR_IN;
        ring_enqueue_trb(ring, data_phys, 0, wLength, data_ctrl);
    }

    /* Status Stage TRB — remember its physical address for event matching. */
    uint64_t status_trb_phys = (uint64_t)ring_trb_phys(ring, ring->enqueue);
    uint32_t status_ctrl = XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC;
    if (wLength > 0 && (bmRequestType & USB_REQ_DIR_IN))
        status_ctrl &= ~XHCI_TRB_DIR_IN;  /* Status OUT (data was IN) */
    else
        status_ctrl |= XHCI_TRB_DIR_IN;   /* Status IN (no data, or data was OUT) */
    ring_enqueue_trb(ring, 0, 0, 0, status_ctrl);

    mb();
    db_write(slot, 1);
    mb();

    xhci_trb_t evt;
    if (xhci_wait_transfer_event(slot, 1, status_trb_phys, &evt, 1000) != 0) {
        serial_print("[xhci] Control transfer timeout/no matching event\n");
        return -1;
    }

    uint8_t cc = XHCI_TRB_COMP_CODE(evt.status);
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
        serial_print("[xhci] Control transfer error cc=");
        serial_print_uint(cc);
        serial_print("\n");

        /* Auto-recover EP0 from STALL — the xHCI spec requires a Reset
           Endpoint + Set TR Dequeue Pointer to clear the halt condition.
           Without this, ALL future control transfers on this slot fail. */
        if (cc == XHCI_CC_STALL) {
            serial_print("[xhci] EP0 stalled, resetting...\n");
            xhci_reset_endpoint(slot, 1);
        }

        return -(int)cc;
    }

    return 0;
}

/* ── Interrupt IN transfer ──────────────────────────────────────── */

int xhci_interrupt_transfer(int slot, int ep_index, void* buf, int len) {
    xhci_device_t* dev = 0;
    for (int i = 0; i < hc.device_count; i++) {
        if (hc.devices[i].slot_id == slot) {
            dev = &hc.devices[i];
            break;
        }
    }
    if (!dev) return -1;

    xhci_ring_t* ring = &dev->ep_ring[ep_index];
    if (!ring->trbs) return -1;

    uint64_t trb_phys = (uint64_t)ring_trb_phys(ring, ring->enqueue);

    uint32_t buf_phys = (uint32_t)(uintptr_t)buf;
    uint32_t control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP;
    ring_enqueue_trb(ring, buf_phys, 0, (uint32_t)len, control);
    mb();

    db_write(slot, (uint32_t)ep_index);
    mb();

    xhci_trb_t evt;
    if (xhci_wait_transfer_event(slot, ep_index, trb_phys, &evt, 500) != 0)
        return -1;

    uint8_t cc = XHCI_TRB_COMP_CODE(evt.status);
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET)
        return -1;

    int residual = evt.status & 0xFFFFFF;
    return len - residual;
}

/* ── Queue interrupt IN transfer (non-blocking, fire-and-forget) ── */

void xhci_queue_interrupt_in(int slot, int ep_dci, uint32_t buf_phys, int len) {
    xhci_device_t* dev = 0;
    for (int i = 0; i < hc.device_count; i++) {
        if (hc.devices[i].slot_id == slot) {
            dev = &hc.devices[i];
            break;
        }
    }
    if (!dev) return;

    xhci_ring_t* ring = &dev->ep_ring[ep_dci];
    if (!ring->trbs) return;

    uint32_t control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP;
    ring_enqueue_trb(ring, buf_phys, 0, (uint32_t)len, control);
    mb();

    db_write(slot, (uint32_t)ep_dci);
    mb();
}

/* ── Reset a halted endpoint ────────────────────────────────────── */

int xhci_reset_endpoint(int slot, int ep_dci) {
    xhci_device_t* dev = 0;
    for (int i = 0; i < hc.device_count; i++) {
        if (hc.devices[i].slot_id == slot) {
            dev = &hc.devices[i];
            break;
        }
    }
    if (!dev) return -1;

    /* Send Reset Endpoint command */
    xhci_trb_t result;
    uint32_t ctrl = XHCI_TRB_TYPE(XHCI_TRB_RESET_EP)
                  | ((uint32_t)slot << 24)
                  | ((uint32_t)ep_dci << 16);
    if (xhci_send_command(0, 0, 0, ctrl, &result) != 0) {
        serial_print("[xhci] Reset EP failed slot=");
        serial_print_uint(slot);
        serial_print(" ep=");
        serial_print_uint(ep_dci);
        serial_print("\n");
        return -1;
    }

    /* Set TR Dequeue Pointer to re-sync the ring */
    xhci_ring_t* ring = &dev->ep_ring[ep_dci];
    uint64_t deq = (uint64_t)ring->trbs_phys
                 + (uint64_t)ring->enqueue * sizeof(xhci_trb_t);
    deq |= (ring->cycle & 1); /* DCS = current producer cycle */
    ctrl = XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQUEUE)
         | ((uint32_t)slot << 24)
         | ((uint32_t)ep_dci << 16);
    if (xhci_send_command((uint32_t)(deq & 0xFFFFFFFF),
                          (uint32_t)(deq >> 32),
                          0, ctrl, &result) != 0) {
        serial_print("[xhci] Set TR Dequeue failed\n");
        return -1;
    }

    serial_print("[xhci] Endpoint reset OK slot=");
    serial_print_uint(slot);
    serial_print(" ep=");
    serial_print_uint(ep_dci);
    serial_print("\n");
    return 0;
}

/* ── Read EP state from Output Context ──────────────────────────── */

int xhci_read_ep_state(int slot, int ep_dci) {
    xhci_device_t* dev = 0;
    for (int i = 0; i < hc.device_count; i++) {
        if (hc.devices[i].slot_id == slot) {
            dev = &hc.devices[i];
            break;
        }
    }
    if (!dev) return -1;
    if (ep_dci < 1 || ep_dci >= XHCI_MAX_EPS) return -1;

    xhci_ep_ctx_t* ep = (xhci_ep_ctx_t*)((uint8_t*)dev->out_ctx + ep_dci * hc.context_size);
    return (int)(ep->ep_state_info & 0x7);
}

/* ── Check USBSTS for errors ────────────────────────────────────── */

uint32_t xhci_read_usbsts(void) {
    if (!hc.enabled) return 0;
    return op_read32(XHCI_OP_USBSTS);
}

/* ── Deep diagnostic: dump event ring state and EP output context ── */

void xhci_dump_evt_ring_diag(void) {
    serial_print("[xhci-diag] evt dequeue=");
    serial_print_uint(hc.evt_ring.dequeue);
    serial_print(" cycle=");
    serial_print_uint(hc.evt_ring.cycle);
    serial_print("\n");

    /* Show raw TRBs around dequeue (3 entries) */
    for (int i = 0; i < 3; i++) {
        uint32_t idx = (hc.evt_ring.dequeue + i) % hc.evt_ring.size;
        xhci_trb_t* e = &hc.evt_ring.trbs[idx];
        serial_print("[xhci-diag] evt[");
        serial_print_uint(idx);
        serial_print("]: lo=");
        serial_print_hex(e->param_lo);
        serial_print(" hi=");
        serial_print_hex(e->param_hi);
        serial_print(" sts=");
        serial_print_hex(e->status);
        serial_print(" ctl=");
        serial_print_hex(e->control);
        serial_print(" cycle=");
        serial_print_uint(e->control & 1);
        serial_print("\n");
    }

    /* Show IMAN and ERDP */
    uint32_t iman = rt_read32(0x20 + XHCI_IR_IMAN);
    uint32_t erdp_lo = rt_read32(0x20 + XHCI_IR_ERDP);
    uint32_t erdp_hi = rt_read32(0x20 + XHCI_IR_ERDP + 4);
    serial_print("[xhci-diag] IMAN=");
    serial_print_hex(iman);
    serial_print(" ERDP=");
    serial_print_hex(erdp_hi);
    serial_print_hex(erdp_lo);
    serial_print(" evt_ring_phys=");
    serial_print_hex(hc.evt_ring.trbs_phys);
    serial_print("\n");

    /* Print USBCMD and USBSTS */
    uint32_t cmd = op_read32(XHCI_OP_USBCMD);
    uint32_t sts = op_read32(XHCI_OP_USBSTS);
    serial_print("[xhci-diag] USBCMD=");
    serial_print_hex(cmd);
    serial_print(" USBSTS=");
    serial_print_hex(sts);
    serial_print("\n");

    /* Terminal summary removed — serial-only diagnostics */
}

void xhci_dump_ep_diag(int slot, int ep_dci) {
    xhci_device_t* dev = 0;
    for (int i = 0; i < hc.device_count; i++) {
        if (hc.devices[i].slot_id == slot) {
            dev = &hc.devices[i];
            break;
        }
    }
    if (!dev) return;
    if (ep_dci < 1 || ep_dci >= XHCI_MAX_EPS) return;

    /* Read EP context from Output Context */
    xhci_ep_ctx_t* ep = (xhci_ep_ctx_t*)((uint8_t*)dev->out_ctx + ep_dci * hc.context_size);
    serial_print("[xhci-diag] EP");
    serial_print_uint(ep_dci);
    serial_print(" slot=");
    serial_print_uint(slot);
    serial_print(": state=");
    serial_print_uint(ep->ep_state_info & 0x7);
    serial_print(" info2=");
    serial_print_hex(ep->ep_info2);
    serial_print(" deq=");
    serial_print_hex((uint32_t)(ep->dequeue_ptr >> 32));
    serial_print_hex((uint32_t)ep->dequeue_ptr);
    serial_print("\n");

    /* Show ring state */
    xhci_ring_t* ring = &dev->ep_ring[ep_dci];
    serial_print("[xhci-diag] ring: enq=");
    serial_print_uint(ring->enqueue);
    serial_print(" deq=");
    serial_print_uint(ring->dequeue);
    serial_print(" cycle=");
    serial_print_uint(ring->cycle);
    serial_print(" phys=");
    serial_print_hex(ring->trbs_phys);
    serial_print("\n");

    /* Show first 2 TRBs on the transfer ring */
    for (int i = 0; i < 2 && ring->trbs; i++) {
        xhci_trb_t* t = &ring->trbs[i];
        serial_print("[xhci-diag] trb[");
        serial_print_uint(i);
        serial_print("]: lo=");
        serial_print_hex(t->param_lo);
        serial_print(" sts=");
        serial_print_hex(t->status);
        serial_print(" ctl=");
        serial_print_hex(t->control);
        serial_print("\n");
    }

    /* Terminal diagnostics removed — serial-only */
}

static int xhci_irq_count = 0;

/* ── USB Get Descriptor helper ──────────────────────────────────── */

static int usb_get_descriptor(int slot, uint8_t desc_type, uint8_t desc_index,
                               uint16_t lang, void* buf, uint16_t len) {
    return xhci_control_transfer(slot,
        USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR,
        (uint16_t)((desc_type << 8) | desc_index),
        lang, len, buf);
}

/* ── USB Set Configuration ──────────────────────────────────────── */

static int usb_set_configuration(int slot, uint8_t config_value) {
    return xhci_control_transfer(slot,
        USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION,
        config_value, 0, 0, 0);
}

/* Helper: detect Boot-interface keyboards/mice */
static int hid_is_boot_keyboard(const usb_interface_desc_t* iface) {
    if (!iface) return 0;
    return (iface->bInterfaceClass == USB_CLASS_HID) &&
           (iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT) &&
           (iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD);
}

static int hid_is_boot_mouse(const usb_interface_desc_t* iface) {
    if (!iface) return 0;
    return (iface->bInterfaceClass == USB_CLASS_HID) &&
           (iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT) &&
           (iface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE);
}

/* Parse a HID report descriptor and return 1 if it contains a Keyboard usage
   (Usage Page = 0x01 Generic Desktop, Usage = 0x06 Keyboard) */
static int hid_report_descriptor_has_keyboard(const uint8_t* rep, size_t len) {
    size_t i = 0;
    uint16_t usage_page = 0;

    while (i < len) {
        uint8_t b = rep[i++];

        if (b == 0xFE) {
            /* Long item */
            if (i + 1 >= len) break;
            uint8_t data_size = rep[i++];
            /* skip long item tag */
            i++; 
            if (i + data_size > len) break;
            i += data_size;
            continue;
        }

        uint8_t size_code = b & 0x03;
        uint8_t type      = (b >> 2) & 0x03;
        uint8_t tag       = (b >> 4) & 0x0F;

        size_t data_len = (size_code == 3) ? 4 : size_code;
        if (i + data_len > len) break;

        uint32_t value = 0;
        for (size_t j = 0; j < data_len; j++)
            value |= ((uint32_t)rep[i + j]) << (8 * j);

        /* Global item: Usage Page (type=1, tag=0) */
        if (type == 1 && tag == 0x0)
            usage_page = (uint16_t)value;

        /* Local item: Usage (type=2, tag=0) */
        if (type == 2 && tag == 0x0) {
            if (usage_page == 0x01 && value == 0x06) {
                /* Generic Desktop / Keyboard */
                return 1;
            }
        }

        i += data_len;
    }

    return 0;
}

/* ── Configure Endpoint for HID interrupt IN ────────────────────── */

static int xhci_configure_endpoint(xhci_device_t* dev, int ep_num, int ep_dir_in,
                                     int ep_type, uint16_t max_packet, uint8_t interval) {
    /* DCI (Device Context Index):
       EP0 = 1
       EP1 OUT = 2, EP1 IN = 3
       EP2 OUT = 4, EP2 IN = 5
       ... EPn OUT = 2n, EPn IN = 2n+1 */
    int dci = ep_num * 2 + (ep_dir_in ? 1 : 0);
    if (dci < 2 || dci >= XHCI_MAX_EPS) return -1;

    /* Allocate transfer ring for this endpoint */
    if (ring_alloc(&dev->ep_ring[dci], XHCI_RING_SIZE) != 0) return -1;

    /* Build Input Context */
    uint8_t* in_ptr = (uint8_t*)dev->in_ctx;
    memset(in_ptr, 0, hc.context_size * 33);

    /* Input Control Context */
    xhci_input_ctrl_ctx_t* icc = (xhci_input_ctrl_ctx_t*)in_ptr;
    icc->add_flags = (1U << 0) | (1U << dci); /* A0 (slot) + endpoint */
    icc->drop_flags = 0;

    /* Slot Context — update context entries */
    xhci_slot_ctx_t* slot = (xhci_slot_ctx_t*)(in_ptr + hc.context_size);
    /* Copy existing slot context from output context */
    memcpy(slot, dev->out_ctx, sizeof(xhci_slot_ctx_t));
    /* Update Context Entries to include this endpoint */
    uint32_t entries = (slot->route_speed_entries >> 27) & 0x1F;
    if ((uint32_t)dci > entries) {
        slot->route_speed_entries = (slot->route_speed_entries & ~(0x1FU << 27))
                                  | ((uint32_t)dci << 27);
    }

    /* Populate existing EP contexts in the Input Context from the Output Context.
       Per xHCI spec, the controller should ignore entries where both Add and Drop
       are '0'. However, copying valid data here provides a safety net for xHCI
       implementations that read all entries regardless (known Intel quirk). */
    for (uint32_t j = 2; j <= entries && j < XHCI_MAX_EPS; j++) {
        if (j == (uint32_t)dci) continue; /* Skip new EP — set up below */
        xhci_ep_ctx_t* out_ep = (xhci_ep_ctx_t*)((uint8_t*)dev->out_ctx + j * hc.context_size);
        if ((out_ep->ep_state_info & 0x7) != 0) { /* Not Disabled */
            xhci_ep_ctx_t* in_ep = (xhci_ep_ctx_t*)(in_ptr + (1 + j) * hc.context_size);
            memcpy(in_ep, out_ep, sizeof(xhci_ep_ctx_t));
        }
    }

    /* Endpoint Context */
    xhci_ep_ctx_t* ep = (xhci_ep_ctx_t*)(in_ptr + (uint32_t)(1 + dci) * hc.context_size);
    ep->ep_info2 = (3U << 1) | ((uint32_t)ep_type << 3) | ((uint32_t)max_packet << 16);
    ep->dequeue_ptr = (uint64_t)dev->ep_ring[dci].trbs_phys | 1;
    /* Average TRB Length [15:0] | Max ESIT Payload Lo [31:16] */
    ep->avg_trb_len = (uint32_t)max_packet | ((uint32_t)max_packet << 16);
    /* Interval: convert USB bInterval to xHCI Endpoint Context Interval field.
       xHCI Interval = 2^(Interval) * 125 µs.
       - HS/SS: bInterval is already an exponent (1–16), so Interval = bInterval - 1.
       - FS/LS: bInterval is in ms (1–255), convert: Interval = floor(log2(bInterval * 8)).
         Approximation: find highest bit position of (bInterval * 8). */
    uint32_t xhci_interval;
    if (dev->speed >= XHCI_SPEED_HIGH) {
        xhci_interval = (interval > 0) ? (interval - 1) : 0;
    } else {
        /* FS/LS: bInterval in ms → 125µs frames = bInterval * 8 */
        uint32_t frames = (uint32_t)interval * 8;
        if (frames < 1) frames = 1;
        xhci_interval = 0;
        while (frames > 1) { frames >>= 1; xhci_interval++; }
        /* Minimum interval of 3 (~1ms) for FS/LS interrupt endpoints */
        if (xhci_interval < 3) xhci_interval = 3;
    }
    if (xhci_interval > 15) xhci_interval = 15;
    ep->ep_state_info = (xhci_interval << 16);

    serial_print("[xhci] EP ctx: interval=");
    serial_print_uint(xhci_interval);
    serial_print(" speed=");
    serial_print_uint(dev->speed);
    serial_print(" bInterval=");
    serial_print_uint(interval);
    serial_print(" ESIT=");
    serial_print_uint(max_packet);
    serial_print("\n");

    mb();

    /* Send Configure Endpoint command */
    xhci_trb_t result;
    uint32_t ctrl = XHCI_TRB_TYPE(XHCI_TRB_CONFIG_EP)
                  | ((uint32_t)dev->slot_id << 24);
    if (xhci_send_command(dev->in_ctx_phys, 0, 0, ctrl, &result) != 0) {
        serial_print("[xhci] Configure Endpoint failed for slot ");
        serial_print_uint(dev->slot_id);
        serial_print("\n");
        return -1;
    }

    dev->hid_max_packet = max_packet;

    /* Read back EP state from Output Context to verify activation */
    xhci_ep_ctx_t* out_ep = (xhci_ep_ctx_t*)((uint8_t*)dev->out_ctx + dci * hc.context_size);
    uint8_t ep_state = out_ep->ep_state_info & 0x7;

    serial_print("[xhci] Configured EP");
    serial_print_uint(ep_num);
    serial_print(ep_dir_in ? " IN" : " OUT");
    serial_print(" DCI=");
    serial_print_uint(dci);
    serial_print(" maxpkt=");
    serial_print_uint(max_packet);
    serial_print(" state=");
    serial_print_uint(ep_state);
    serial_print("\n");

    if (ep_state != 1) {
        serial_print("[xhci] WARN: EP not Running after configure (state=");
        serial_print_uint(ep_state);
        serial_print(")\n");
    }

    return 0;
}

/* ── Update EP0 Max Packet Size via Evaluate Context ────────────── */

static int xhci_update_ep0_mps(xhci_device_t* dev, uint16_t new_mps) {
    uint8_t* in_ptr = (uint8_t*)dev->in_ctx;
    memset(in_ptr, 0, hc.context_size * 33);

    /* Input Control Context: evaluate EP0 */
    xhci_input_ctrl_ctx_t* icc = (xhci_input_ctrl_ctx_t*)in_ptr;
    icc->add_flags = (1U << 1); /* A1 = EP0 (DCI 1) */
    icc->drop_flags = 0;

    /* EP0 Context — only Max Packet Size is evaluated */
    xhci_ep_ctx_t* ep0 = (xhci_ep_ctx_t*)(in_ptr + 2 * hc.context_size);
    ep0->ep_info2 = (3U << 1) | (XHCI_EP_TYPE_CONTROL << 3)
                  | ((uint32_t)new_mps << 16);

    mb();

    xhci_trb_t result;
    uint32_t ctrl = XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CTX)
                  | ((uint32_t)dev->slot_id << 24);
    return xhci_send_command(dev->in_ctx_phys, 0, 0, ctrl, &result);
}

/* ── Probe and enumerate a device ───────────────────────────────── */

static void xhci_enumerate_device(int dev_idx) {
    xhci_device_t* dev = &hc.devices[dev_idx];

    /* Allocate a DMA-safe buffer for descriptors */
    uint32_t buf_phys = physmem_alloc_frame();
    if (!buf_phys) return;
    paging_map_mmio(buf_phys);
    uint8_t* buf = (uint8_t*)(uintptr_t)buf_phys;
    memset(buf, 0, PHYSMEM_FRAME_SIZE);

    /* Stage 1: read first 8 bytes to learn bMaxPacketSize0 */
    int rc = usb_get_descriptor(dev->slot_id, USB_DESC_DEVICE, 0, 0, buf, 8);
    if (rc != 0) {
        /* Likely a phantom USB2 companion port with CCS=1 but no real device.
           This is normal on Intel chipsets — just skip silently. */
        serial_print("[usb] No response from slot ");
        serial_print_uint(dev->slot_id);
        serial_print(" (phantom port?)\n");
        physmem_free_frame(buf_phys);
        return;
    }

    usb_device_desc_t* ddesc = (usb_device_desc_t*)buf;
    uint8_t dev_mps = ddesc->bMaxPacketSize0;
    uint16_t current_mps = ep0_max_packet(dev->speed);

    /* Update EP0 max packet size if device reports a different one */
    if (dev_mps > 0 && dev_mps != (uint8_t)current_mps) {
        serial_print("[usb] Updating EP0 MPS from ");
        serial_print_uint(current_mps);
        serial_print(" to ");
        serial_print_uint(dev_mps);
        serial_print("\n");
        if (xhci_update_ep0_mps(dev, dev_mps) != 0) {
            serial_print("[usb] Evaluate Context failed for MPS update\n");
        }
    }

    /* Stage 2: read full 18-byte device descriptor (with retry) */
    int desc18_ok = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(buf, 0, 512);
        if (usb_get_descriptor(dev->slot_id, USB_DESC_DEVICE, 0, 0, buf, 18) == 0) {
            desc18_ok = 1;
            break;
        }
        delay_ms(10);
    }
    if (!desc18_ok) {
        serial_print("[usb] Failed to get full device descriptor slot=");
        serial_print_uint(dev->slot_id);
        serial_print("\n");
        physmem_free_frame(buf_phys);
        return;
    }

    ddesc = (usb_device_desc_t*)buf;
    serial_print("[usb] Device: VID=");
    serial_print_hex(ddesc->idVendor);
    serial_print(" PID=");
    serial_print_hex(ddesc->idProduct);
    serial_print(" class=");
    serial_print_uint(ddesc->bDeviceClass);
    serial_print("/");
    serial_print_uint(ddesc->bDeviceSubClass);
    serial_print("/");
    serial_print_uint(ddesc->bDeviceProtocol);
    serial_print("\n");

    uint8_t dev_class = ddesc->bDeviceClass;

    /* Hub device: just SET_CONFIGURATION and mark as hub. 
       Hub ports will be enumerated separately. */
    if (dev_class == USB_CLASS_HUB) {
        dev->is_hub = 1;
        memset(buf, 0, 512);
        if (usb_get_descriptor(dev->slot_id, USB_DESC_CONFIGURATION, 0, 0, buf, 9) == 0) {
            usb_config_desc_t* cdesc = (usb_config_desc_t*)buf;
            if (usb_set_configuration(dev->slot_id, cdesc->bConfigurationValue) == 0) {
                dev->configured = 1;
            }
        }
        physmem_free_frame(buf_phys);
        return;
    }

    /* Get Configuration Descriptor (first 9 bytes for total length) */
    memset(buf, 0, 512);
    if (usb_get_descriptor(dev->slot_id, USB_DESC_CONFIGURATION, 0, 0, buf, 9) != 0) {
        serial_print("[usb] Failed to get config descriptor\n");
        physmem_free_frame(buf_phys);
        return;
    }

    usb_config_desc_t* cdesc = (usb_config_desc_t*)buf;
    uint16_t total_len = cdesc->wTotalLength;
    if (total_len > 512) total_len = 512;

    /* Get full configuration descriptor */
    memset(buf, 0, 512);
    if (usb_get_descriptor(dev->slot_id, USB_DESC_CONFIGURATION, 0, 0, buf, total_len) != 0) {
        serial_print("[usb] Failed to get full config descriptor\n");
        physmem_free_frame(buf_phys);
        return;
    }

    /* Parse interfaces and endpoints */
    uint16_t offset = 0;
    while (offset + 2 <= total_len) {
        uint8_t len = buf[offset];
        uint8_t type = buf[offset + 1];
        if (len < 2 || offset + len > total_len) break;

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            usb_interface_desc_t* iface = (usb_interface_desc_t*)(buf + offset);

            if (iface->bInterfaceClass == USB_CLASS_HID) {

                serial_print("[usb] HID interface found: subclass=");
                serial_print_uint(iface->bInterfaceSubClass);
                serial_print(" proto=");
                serial_print_uint(iface->bInterfaceProtocol);
                serial_print("\n");

                int is_boot = (iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT);
                int is_kbd   = hid_is_boot_keyboard(iface);
                int is_mouse = hid_is_boot_mouse(iface);

                if (is_boot) {
                    if (is_kbd)
                        serial_print("[usb] HID BOOT KEYBOARD\n");
                    else if (is_mouse)
                        serial_print("[usb] HID BOOT MOUSE\n");
                } else {
                    serial_print("[usb] HID GENERIC\n");
                }

                /* Log interface class/subclass/protocol to serial */
                serial_print("[usb] IF(");
                serial_print_uint(iface->bInterfaceClass);
                serial_print("/");
                serial_print_uint(iface->bInterfaceSubClass);
                serial_print("/");
                serial_print_uint(iface->bInterfaceProtocol);
                serial_print(")\n");

                /* Try to find an HID descriptor (to get Report descriptor length) */
                uint16_t hid_report_len = 0;
                uint16_t scan = offset + len;
                while (scan + 2 <= total_len) {
                    uint8_t dlen = buf[scan];
                    uint8_t dtype = buf[scan + 1];
                    if (dlen < 2 || scan + dlen > total_len) break;
                    if (dtype == USB_DESC_HID && dlen >= 9) {
                        uint8_t numdesc = buf[scan + 5];
                        if (numdesc > 0 && buf[scan + 6] == USB_DESC_HID_REPORT) {
                            hid_report_len = (uint16_t)buf[scan + 7] | ((uint16_t)buf[scan + 8] << 8);
                        }
                        break;
                    }
                    if (dtype == USB_DESC_INTERFACE) break;
                    scan += dlen;
                }

                /* Find the Interrupt IN endpoint in subsequent descriptors */
                uint16_t ep_off = offset + len;
                int found_ep = 0;
                while (ep_off + 2 <= total_len && found_ep < iface->bNumEndpoints) {
                    uint8_t elen = buf[ep_off];
                    uint8_t etype = buf[ep_off + 1];
                    if (elen < 2 || ep_off + elen > total_len) break;
                    /* Stop if we hit the next interface descriptor */
                    if (etype == USB_DESC_INTERFACE) break;

                    if (etype == USB_DESC_ENDPOINT && elen >= sizeof(usb_endpoint_desc_t)) {
                        found_ep++;
                        usb_endpoint_desc_t* ep = (usb_endpoint_desc_t*)(buf + ep_off);
                        if ((ep->bmAttributes & 0x03) == USB_EP_TYPE_INTERRUPT &&
                            (ep->bEndpointAddress & USB_EP_DIR_MASK) == USB_EP_DIR_IN) {

                            int ep_num = ep->bEndpointAddress & USB_EP_NUM_MASK;
                            serial_print("[usb] Interrupt IN EP");
                            serial_print_uint(ep_num);
                            serial_print(" maxpkt=");
                            serial_print_uint(ep->wMaxPacketSize);
                            serial_print(" interval=");
                            serial_print_uint(ep->bInterval);
                            serial_print("\n");

                            /* Set Configuration first (once per device) */
                            if (!dev->configured) {
                                if (usb_set_configuration(dev->slot_id, cdesc->bConfigurationValue) == 0) {
                                    dev->configured = 1;
                                } else {
                                    break; /* Skip this interface entirely */
                                }
                            }

                            /* For non-boot HID interfaces, check report descriptor
                               BEFORE configuring the endpoint. Only proceed if the
                               report descriptor confirms this is a keyboard.
                               This avoids sending SET_PROTOCOL or configuring endpoints
                               for vendor-specific interfaces on composite devices
                               (e.g. Logitech receivers), which can confuse the device. */
                            int report_has_kbd = 0;
                            if (!is_boot) {
                                /* If we already have a boot keyboard or mouse for
                                   this device, skip generic HID interfaces entirely.
                                   Logitech HID++ (3/0/0) report descriptors contain
                                   Keyboard usage which would falsely match and
                                   OVERWRITE the correct boot endpoint assignment. */
                                if (dev->is_hid_keyboard || dev->is_hid_mouse) {
                                    serial_print("[usb] Skipping generic HID iface ");
                                    serial_print_uint(iface->bInterfaceNumber);
                                    serial_print(" (boot devs already found)\n");
                                    break;
                                }
                                if (hid_report_len > 0 && dev->configured) {
                                    uint32_t rep_phys = physmem_alloc_frame();
                                    if (rep_phys) {
                                        paging_map_mmio(rep_phys);
                                        uint8_t* repbuf = (uint8_t*)(uintptr_t)rep_phys;
                                        memset(repbuf, 0, PHYSMEM_FRAME_SIZE);
                                        uint16_t fetch_len = hid_report_len;
                                        if (fetch_len > PHYSMEM_FRAME_SIZE) fetch_len = PHYSMEM_FRAME_SIZE;
                                        if (usb_get_descriptor(dev->slot_id, USB_DESC_HID_REPORT, 0,
                                            iface->bInterfaceNumber, repbuf, fetch_len) == 0) {
                                            if (hid_report_descriptor_has_keyboard(repbuf, fetch_len))
                                                report_has_kbd = 1;
                                        }
                                        physmem_free_frame(rep_phys);
                                    }
                                }
                                if (!report_has_kbd) {
                                    serial_print("[usb] Skipping non-boot HID iface ");
                                    serial_print_uint(iface->bInterfaceNumber);
                                    serial_print(" (not a keyboard/mouse)\n");
                                    break; /* Skip — don't configure EP or send SET_PROTOCOL */
                                }
                            }

                            int dci = ep_num * 2 + 1; /* IN endpoint */
                            if (xhci_configure_endpoint(dev, ep_num, 1,
                                XHCI_EP_TYPE_INTERRUPT_IN,
                                ep->wMaxPacketSize, ep->bInterval) != 0) {
                                break;
                            }

                            /* Set boot protocol for boot subclass interfaces */
                            if (is_boot) {
                                xhci_control_transfer(dev->slot_id,
                                    USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
                                    USB_HID_REQ_SET_PROTOCOL,
                                    USB_HID_PROTOCOL_BOOT,
                                    iface->bInterfaceNumber, 0, 0);
                            }

                            /* Accept keyboard/mouse */
                            if (is_kbd || report_has_kbd) {
                                dev->is_hid_keyboard = 1;
                                dev->hid_kbd_ep_in = dci;
                                dev->hid_kbd_iface = iface->bInterfaceNumber;
                            } else if (is_mouse) {
                                dev->is_hid_mouse = 1;
                                dev->hid_mouse_ep_in = dci;
                                dev->hid_mouse_iface = iface->bInterfaceNumber;
                            }

                            /* Set Idle (rate=0 = only report on change)
                               Skip for keyboard — some Logitech receivers
                               go silent after SET_IDLE on kbd interface. */
                            if (!is_kbd) {
                                xhci_control_transfer(dev->slot_id,
                                    USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
                                    USB_HID_REQ_SET_IDLE,
                                    0, iface->bInterfaceNumber, 0, 0);
                            }
                        }
                    }
                    ep_off += elen;
                }
            }
        }
        offset += len;
    }

    physmem_free_frame(buf_phys);
}

/* ── USB Hub Enumeration ────────────────────────────────────────── */

/* Forward declaration */
static void xhci_enumerate_device(int dev_idx);

static void xhci_enumerate_hub(int hub_dev_idx) {
    xhci_device_t* hub = &hc.devices[hub_dev_idx];

    /* Get Hub Descriptor to find number of ports */
    uint32_t buf_phys = physmem_alloc_frame();
    if (!buf_phys) return;
    paging_map_mmio(buf_phys);
    uint8_t* buf = (uint8_t*)(uintptr_t)buf_phys;
    memset(buf, 0, PHYSMEM_FRAME_SIZE);

    if (xhci_control_transfer(hub->slot_id,
            USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_DEVICE,
            USB_REQ_GET_DESCRIPTOR,
            (USB_DESC_HUB << 8) | 0, 0, sizeof(usb_hub_desc_t), buf) != 0) {
        serial_print("[usb-hub] Failed to get hub descriptor\n");
        physmem_free_frame(buf_phys);
        return;
    }

    usb_hub_desc_t* hdesc = (usb_hub_desc_t*)buf;
    int nports = hdesc->bNbrPorts;
    int pwrdelay = hdesc->bPwrOn2PwrGood * 2; /* in ms */
    if (nports < 1 || nports > 15) nports = 0; /* sanity */

    hub->is_hub = 1;
    hub->hub_ports = nports;



    /* Tell xHCI this device is a hub via Evaluate Context.
       Set Hub bit [26] in slot ctx word 0, and Num Ports [31:24] in word 1. */
    {
        uint8_t* in_ptr = (uint8_t*)hub->in_ctx;
        memset(in_ptr, 0, hc.context_size * 33);
        xhci_input_ctrl_ctx_t* icc = (xhci_input_ctrl_ctx_t*)in_ptr;
        icc->add_flags = (1U << 0); /* Evaluate Slot Context */
        xhci_slot_ctx_t* sc = (xhci_slot_ctx_t*)(in_ptr + hc.context_size);
        memcpy(sc, hub->out_ctx, sizeof(xhci_slot_ctx_t));
        sc->route_speed_entries |= (1U << 26); /* Hub = 1 */
        sc->latency_hub_port = (sc->latency_hub_port & 0x00FFFFFF)
                             | ((uint32_t)nports << 24);
        /* For FS/LS hubs, set TTT (Think Time) if needed */
        mb();
        xhci_trb_t eval_result;
        uint32_t eval_ctrl = XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CTX)
                           | ((uint32_t)hub->slot_id << 24);
        if (xhci_send_command(hub->in_ctx_phys, 0, 0, eval_ctrl, &eval_result) != 0) {
            serial_print("[usb-hub] Evaluate Context failed\n");
        }
    }

    /* Power on all hub ports */
    for (int p = 1; p <= nports; p++) {
        xhci_control_transfer(hub->slot_id,
            USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER,
            USB_HUB_REQ_SET_FEATURE,
            USB_HUB_FEAT_PORT_POWER, p, 0, 0);
    }
    delay_ms(pwrdelay > 0 ? (uint32_t)pwrdelay : 100);

    /* Check each port for a connected device */
    for (int p = 1; p <= nports; p++) {
        memset(buf, 0, 4);
        if (xhci_control_transfer(hub->slot_id,
                USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER,
                USB_HUB_REQ_GET_STATUS,
                0, p, 4, buf) != 0) {
            continue;
        }
        usb_port_status_t* ps = (usb_port_status_t*)buf;
        if (!(ps->wPortStatus & USB_HUB_PORT_CONNECTION)) continue;

        serial_print("[usb-hub] Hub port ");
        serial_print_uint(p);
        serial_print(" connected, status=");
        serial_print_hex(ps->wPortStatus);
        serial_print("\n");

        /* Reset the port */
        xhci_control_transfer(hub->slot_id,
            USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER,
            USB_HUB_REQ_SET_FEATURE,
            USB_HUB_FEAT_PORT_RESET, p, 0, 0);
        delay_ms(60);

        /* Read status again after reset */
        memset(buf, 0, 4);
        if (xhci_control_transfer(hub->slot_id,
                USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER,
                USB_HUB_REQ_GET_STATUS,
                0, p, 4, buf) != 0) {
            continue;
        }
        ps = (usb_port_status_t*)buf;
        if (!(ps->wPortStatus & USB_HUB_PORT_ENABLE)) {
            continue;
        }

        /* Clear change bits */
        xhci_control_transfer(hub->slot_id,
            USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER,
            USB_HUB_REQ_CLEAR_FEATURE,
            USB_HUB_FEAT_C_PORT_RESET, p, 0, 0);
        xhci_control_transfer(hub->slot_id,
            USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_OTHER,
            USB_HUB_REQ_CLEAR_FEATURE,
            USB_HUB_FEAT_C_PORT_CONNECTION, p, 0, 0);

        /* Determine speed of the downstream device */
        int child_speed;
        if (ps->wPortStatus & USB_HUB_PORT_HIGH_SPEED)
            child_speed = XHCI_SPEED_HIGH;
        else if (ps->wPortStatus & USB_HUB_PORT_LOW_SPEED)
            child_speed = XHCI_SPEED_LOW;
        else
            child_speed = XHCI_SPEED_FULL;

        /* Build route string: parent's route + this port at the right nibble depth */
        uint32_t route = hub->route_string;
        int depth = 0;
        uint32_t tmp = route;
        while (tmp & 0xF) { tmp >>= 4; depth++; }
        if (depth < 5) {
            route |= ((uint32_t)p << (depth * 4));
        }

        /* Address the device behind the hub */
        int dev_idx = xhci_address_device_ex(hub->port, child_speed,
                                              hub->slot_id, p, route);
        if (dev_idx < 0) {
            continue;
        }

        delay_ms(50);
        xhci_enumerate_device(dev_idx);

        /* Recursive: if child is also a hub, enumerate it */
        if (hc.devices[dev_idx].is_hub) {
            xhci_enumerate_hub(dev_idx);
        }
    }

    physmem_free_frame(buf_phys);
}

/* ── Port reset and enable ──────────────────────────────────────── */

static int xhci_port_reset(int port_index) {
    uint32_t portsc = port_read32(port_index);

    /* Check if device is connected */
    if (!(portsc & XHCI_PORTSC_CCS)) return -1;

    int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

    /* SuperSpeed+ ports auto-train on connection; if already enabled, skip reset */
    if (speed >= XHCI_SPEED_SUPER && (portsc & XHCI_PORTSC_PED)) {
        serial_print("[xhci] Port ");
        serial_print_uint(port_index);
        serial_print(" SuperSpeed already enabled, skipping reset\n");
        return speed;
    }

    /* Issue port reset.
       PORTSC layout per xHCI spec §5.4.8:
         Bits [0]     CCS   — RO       (don't write)
         Bits [1]     PED   — RW1CS    (write 1 clears! NEVER set this)
         Bits [3]     OCA   — RO
         Bits [4]     PR    — RW1S     (write 1 to start reset)
         Bits [8:5]   PLS   — RWS      (requires LWS=1 to write)
         Bits [9]     PP    — RW       (must preserve power)
         Bits [13:10] Speed — RO
         Bits [16]    LWS   — RW       (Link Write Strobe)
         Bits [23:17] Change— RW1C     (write 1 to clear; MUST keep 0 to preserve)
       Strategy: build the write value from scratch, only setting what we want. */
    uint32_t write_val = XHCI_PORTSC_PR;  /* Assert port reset */
    if (portsc & XHCI_PORTSC_PP)
        write_val |= XHCI_PORTSC_PP;      /* Preserve port power */
    port_write32(port_index, write_val);
    mb();

    /* Wait for reset to complete (PRC=1 means reset finished) */
    for (int i = 0; i < 200; i++) {
        portsc = port_read32(port_index);
        if (portsc & XHCI_PORTSC_PRC) {
            /* Clear PRC by writing 1 to it. Build value from scratch:
               preserve PP, write 1 to PRC only, leave all other
               change bits as 0 so we don't accidentally clear them. */
            uint32_t ack = XHCI_PORTSC_PRC;  /* Clear PRC (RW1C) */
            if (portsc & XHCI_PORTSC_PP)
                ack |= XHCI_PORTSC_PP;       /* Preserve PP */
            port_write32(port_index, ack);
            break;
        }
        delay_ms(1);
    }

    /* Check port is now enabled */
    portsc = port_read32(port_index);
    if (!(portsc & XHCI_PORTSC_PED)) {
        serial_print("[xhci] Port ");
        serial_print_uint(port_index);
        serial_print(" not enabled after reset\n");
        return -1;
    }

    speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    return speed;
}

/* ── Port enumeration ───────────────────────────────────────────── */

static void xhci_enumerate_ports(void) {
    serial_print("[xhci] Enumerating ");
    serial_print_uint(hc.max_ports);
    serial_print(" ports\n");

    /* Dump raw PORTSC values for diagnostics; count connected ports */
    int ccs_count = 0;
    for (uint32_t i = 0; i < hc.max_ports; i++) {
        uint32_t ps = port_read32(i);
        serial_print("[xhci] PORTSC[");
        serial_print_uint(i);
        serial_print("]=");
        serial_print_hex(ps);
        serial_print("\n");
        if (ps & XHCI_PORTSC_CCS) ccs_count++;
    }

    for (uint32_t i = 0; i < hc.max_ports; i++) {
        uint32_t portsc = port_read32(i);
        if (!(portsc & XHCI_PORTSC_CCS)) continue;

        int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
        serial_print("[xhci] Port ");
        serial_print_uint(i);
        serial_print(" connected, speed=");
        serial_print_uint(speed);
        serial_print("\n");

        /* Reset port (USB2 ports need explicit reset) */
        int reset_speed = xhci_port_reset(i);
        if (reset_speed < 0) {
            serial_print("[xhci] Port ");
            serial_print_uint(i);
            serial_print(" reset failed\n");
            continue;
        }

        /* Drain port status change events for THIS port only.
           Use peek so we never consume events that aren't ours. */
        xhci_trb_t evt;
        for (int drain = 0; drain < 4; drain++) {
            if (xhci_wait_any_event(&evt, 50) != 0)
                break;  /* Timeout — nothing pending */
            uint32_t evt_type = XHCI_TRB_GET_TYPE(evt.control);
            if (evt_type != XHCI_TRB_PORT_STATUS_CHANGE)
                break;  /* Not a PSC — leave it for the real consumer */
            /* PSC event: param_lo bits[31:24] = port ID (1-based) */
            uint8_t psc_port = (uint8_t)((evt.param_lo >> 24) & 0xFF);
            if (psc_port != (uint8_t)(i + 1))
                break;  /* PSC for a different port — leave it */
            /* This PSC is ours — consume it */
            xhci_consume_event();
        }

        /* Address the device */
        int dev_idx = xhci_address_device(i, reset_speed);
        if (dev_idx < 0) {
            continue;
        }

        /* Enumerate (get descriptors, configure HID endpoints) */
        delay_ms(50);  /* Give device time after SET_ADDRESS (real HW needs more) */
        xhci_enumerate_device(dev_idx);

        /* If this device is a hub, enumerate its downstream ports */
        if (hc.devices[dev_idx].is_hub) {
            xhci_enumerate_hub(dev_idx);
        }
    }
}

/* ── IRQ Handler ────────────────────────────────────────────────── */

void xhci_irq_handler(void) {
    if (!hc.enabled) return;

    uint32_t sts = op_read32(XHCI_OP_USBSTS);
    if (!(sts & XHCI_STS_EINT)) return;

    /* Ack status */
    op_write32(XHCI_OP_USBSTS, XHCI_STS_EINT);

    /* Ack interrupter */
    uint32_t iman = rt_read32(0x20 + XHCI_IR_IMAN);
    rt_write32(0x20 + XHCI_IR_IMAN, iman | XHCI_IMAN_IP);

    xhci_irq_count++;
    /* Events are consumed by usb_hid_poll() in the main loop */
}

int xhci_get_irq_count(void) {
    return xhci_irq_count;
}

/* ── Initialization ─────────────────────────────────────────────── */

int xhci_is_enabled(void) {
    return hc.enabled;
}

void xhci_init(void) {
    memset(&hc, 0, sizeof(hc));

    /* Find xHCI controller: class 0x0C, subclass 0x03, prog_if 0x30 */
    const pci_device_t* dev = 0;
    int xhci_ctrl_count = 0;
    int pci_count = pci_device_count();
    for (int i = 0; i < pci_count; i++) {
        const pci_device_t* d = pci_get_device(i);
        if (d && d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
            xhci_ctrl_count++;
            serial_print("[xhci] Found controller #");
            serial_print_uint(xhci_ctrl_count);
            serial_print(" at PCI ");
            serial_print_uint(d->bus);
            serial_print(":");
            serial_print_uint(d->slot);
            serial_print(".");
            serial_print_uint(d->func);
            serial_print(" VID=");
            serial_print_hex(d->vendor_id);
            serial_print(" DID=");
            serial_print_hex(d->device_id);
            serial_print("\n");
            if (!dev) dev = d;
        }
    }
    if (!dev) {
        serial_print("[xhci] No xHCI controller found (need prog_if=0x30)\n");
        klog_write(KLOG_LEVEL_WARN, "xhci", "No xHCI controller found");
        return;
    }
    if (xhci_ctrl_count > 1) {
        serial_print("[xhci] WARNING: multiple controllers found, using first only\n");
    }

    serial_print("[xhci] Found xHCI at PCI ");
    serial_print_uint(dev->bus);
    serial_print(":");
    serial_print_uint(dev->slot);
    serial_print(".");
    serial_print_uint(dev->func);
    serial_print(" VID=");
    serial_print_hex(dev->vendor_id);
    serial_print(" DID=");
    serial_print_hex(dev->device_id);
    serial_print(" progif=");
    serial_print_hex(dev->prog_if);
    serial_print("\n");


    hc.irq_line = dev->irq_line;

    /* Enable bus mastering and memory space */
    pci_enable_bus_mastering(dev);
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1U << 1); /* Memory Space Enable */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    /* Map BAR0 (MMIO) — read directly from config space for 64-bit support */
    uint32_t bar0_lo = pci_config_read32(dev->bus, dev->slot, dev->func, 0x10);
    uint64_t bar0_addr = (uint64_t)(bar0_lo & ~0xFU);

    /* Check if BAR0 is 64-bit (bits 2:1 of BAR = 10b) */
    if ((bar0_lo & 0x06) == 0x04) {
        uint32_t bar0_hi = pci_config_read32(dev->bus, dev->slot, dev->func, 0x14);
        bar0_addr |= ((uint64_t)bar0_hi << 32);
    }

    serial_print("[xhci] BAR0 = ");
    serial_print_hex((uint32_t)(bar0_addr >> 32));
    serial_print_hex((uint32_t)(bar0_addr & 0xFFFFFFFF));
    serial_print("\n");

    if (bar0_addr == 0) {
        serial_print("[xhci] BAR0 is zero\n");
        return;
    }
    paging_map_mmio((uintptr_t)bar0_addr);
    hc.mmio_phys = bar0_addr;

    /* Parse Capability Registers */
    hc.cap_base = (volatile uint32_t*)(uintptr_t)bar0_addr;
    xhci_cap_regs_t* cap = (xhci_cap_regs_t*)hc.cap_base;

    uint8_t  cap_length = cap->caplength;
    uint16_t hci_ver    = cap->hciversion;
    uint32_t hcsparams1 = cap->hcsparams1;
    uint32_t hcsparams2 = cap->hcsparams2;
    uint32_t hccparams1 = cap->hccparams1;
    uint32_t dboff      = cap->dboff;
    uint32_t rtsoff     = cap->rtsoff;

    hc.max_slots  = XHCI_HCS1_MAX_SLOTS(hcsparams1);
    hc.max_ports  = XHCI_HCS1_MAX_PORTS(hcsparams1);
    hc.max_intrs  = XHCI_HCS1_MAX_INTRS(hcsparams1);
    hc.context_size = XHCI_HCC1_CSZ(hccparams1) ? 64 : 32;
    hc.scratchpad_count = XHCI_HCS2_MAX_SCRATCHPAD(hcsparams2);

    if (hc.max_ports > XHCI_MAX_PORTS) hc.max_ports = XHCI_MAX_PORTS;
    if (hc.max_slots > 64) hc.max_slots = 64;

    serial_print("[xhci] Version ");
    serial_print_hex(hci_ver);
    serial_print(" slots=");
    serial_print_uint(hc.max_slots);
    serial_print(" ports=");
    serial_print_uint(hc.max_ports);
    serial_print(" ctxsz=");
    serial_print_uint(hc.context_size);
    serial_print(" scratch=");
    serial_print_uint(hc.scratchpad_count);
    serial_print("\n");

    /* Calculate register base addresses */
    hc.op_base   = (volatile uint32_t*)(uintptr_t)(bar0_addr + cap_length);
    hc.rt_base   = (volatile uint32_t*)(uintptr_t)(bar0_addr + rtsoff);
    hc.db_base   = (volatile uint32_t*)(uintptr_t)(bar0_addr + dboff);
    hc.port_base = (volatile uint32_t*)(uintptr_t)(bar0_addr + cap_length + 0x400);

    serial_print("[xhci] caplength=");
    serial_print_uint(cap_length);
    serial_print(" dboff=");
    serial_print_hex(dboff);
    serial_print(" rtsoff=");
    serial_print_hex(rtsoff);
    serial_print("\n");
    serial_print("[xhci] op_base=");
    serial_print_hex((uint32_t)(uintptr_t)hc.op_base);
    serial_print(" port_base=");
    serial_print_hex((uint32_t)(uintptr_t)hc.port_base);
    serial_print(" rt_base=");
    serial_print_hex((uint32_t)(uintptr_t)hc.rt_base);
    serial_print(" db_base=");
    serial_print_hex((uint32_t)(uintptr_t)hc.db_base);
    serial_print("\n");

    /* Perform BIOS/OS handoff */
    xhci_bios_handoff(dev);

    /* Reset controller */
    if (xhci_reset() != 0) {
        serial_print("[xhci] Reset failed, aborting\n");
        return;
    }

    /* Wait for CNR to clear */
    for (int i = 0; i < 100; i++) {
        if (!(op_read32(XHCI_OP_USBSTS) & XHCI_STS_CNR))
            break;
        delay_ms(1);
    }

    /* Set Max Device Slots Enabled */
    op_write32(XHCI_OP_CONFIG, hc.max_slots);

    /* Allocate DCBAA */
    if (xhci_alloc_dcbaa() != 0) {
        serial_print("[xhci] DCBAA alloc failed\n");
        return;
    }
    op_write64(XHCI_OP_DCBAAP, (uint64_t)hc.dcbaa_phys);

    /* Allocate Scratchpad Buffers */
    if (xhci_alloc_scratchpad() != 0) {
        serial_print("[xhci] Scratchpad alloc failed\n");
        return;
    }

    /* Allocate Command Ring */
    if (ring_alloc(&hc.cmd_ring, XHCI_RING_SIZE) != 0) {
        serial_print("[xhci] Command ring alloc failed\n");
        return;
    }
    /* Write CRCR — physical address of command ring, RCS=1 */
    op_write64(XHCI_OP_CRCR, (uint64_t)hc.cmd_ring.trbs_phys | 1);

    /* Set up Event Ring */
    if (xhci_setup_event_ring() != 0) {
        serial_print("[xhci] Event ring setup failed\n");
        return;
    }

    /* Start controller */
    uint32_t usbcmd = op_read32(XHCI_OP_USBCMD);
    usbcmd |= XHCI_CMD_RUN | XHCI_CMD_INTE;
    op_write32(XHCI_OP_USBCMD, usbcmd);
    mb();

    /* Wait for HCH to clear */
    for (int i = 0; i < 100; i++) {
        if (!(op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH))
            break;
        delay_ms(1);
    }
    if (op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        serial_print("[xhci] Controller did not start\n");
        return;
    }

    hc.enabled = 1;
    serial_print("[xhci] Controller running\n");
    klog_write(KLOG_LEVEL_INFO, "xhci", "xHCI controller initialized");
    /* Route xHCI PCI interrupt via IOAPIC to vector 48 */
    if (apic_is_enabled() && hc.irq_line != 0 && hc.irq_line != 0xFF) {
        ioapic_route_irq_level(hc.irq_line, 48, 0);
        serial_print("[xhci] IRQ ");
        serial_print_uint(hc.irq_line);
        serial_print(" routed to vector 48\n");
    }

    /* Send NOOP to verify command ring works */
    xhci_trb_t noop_result;
    if (xhci_send_command(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_NOOP_CMD), &noop_result) == 0) {
        serial_print("[xhci] Command ring verified (NOOP OK)\n");
    } else {
        serial_print("[xhci] WARN: NOOP command failed\n");
    }

    /* Enumerate connected ports */
    xhci_enumerate_ports();
}
