#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

/* ── xHCI Capability Registers (offsets from BAR0) ──────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  caplength;       /* 0x00: Capacity Registers Length */
    uint8_t  reserved;        /* 0x01 */
    uint16_t hciversion;      /* 0x02: Interface Version Number */
    uint32_t hcsparams1;      /* 0x04: Structural Parameters 1 */
    uint32_t hcsparams2;      /* 0x08: Structural Parameters 2 */
    uint32_t hcsparams3;      /* 0x0C: Structural Parameters 3 */
    uint32_t hccparams1;      /* 0x10: Capability Parameters 1 */
    uint32_t dboff;           /* 0x14: Doorbell Offset */
    uint32_t rtsoff;          /* 0x18: Runtime Register Space Offset */
    uint32_t hccparams2;      /* 0x1C: Capability Parameters 2 */
} xhci_cap_regs_t;

/* HCSPARAMS1 field extraction */
#define XHCI_HCS1_MAX_SLOTS(p)   ((p) & 0xFF)
#define XHCI_HCS1_MAX_INTRS(p)   (((p) >> 8) & 0x7FF)
#define XHCI_HCS1_MAX_PORTS(p)   (((p) >> 24) & 0xFF)

/* HCSPARAMS2 field extraction */
#define XHCI_HCS2_MAX_SCRATCHPAD_HI(p) (((p) >> 21) & 0x1F)
#define XHCI_HCS2_MAX_SCRATCHPAD_LO(p) (((p) >> 27) & 0x1F)
#define XHCI_HCS2_MAX_SCRATCHPAD(p) \
    ((XHCI_HCS2_MAX_SCRATCHPAD_HI(p) << 5) | XHCI_HCS2_MAX_SCRATCHPAD_LO(p))

/* HCCPARAMS1 field extraction */
#define XHCI_HCC1_AC64(p)        ((p) & 1)            /* 64-bit addressing */
#define XHCI_HCC1_CSZ(p)         (((p) >> 2) & 1)     /* Context Size (0=32B, 1=64B) */
#define XHCI_HCC1_XECP(p)       (((p) >> 16) & 0xFFFF) /* xHCI Extended Capabilities Pointer (dwords) */

/* ── xHCI Operational Registers (offsets from op_base) ──────────── */

#define XHCI_OP_USBCMD     0x00
#define XHCI_OP_USBSTS     0x04
#define XHCI_OP_PAGESIZE    0x08
#define XHCI_OP_DNCTRL     0x14
#define XHCI_OP_CRCR       0x18  /* Command Ring Control Register (64-bit) */
#define XHCI_OP_DCBAAP     0x30  /* Device Context Base Address Array Pointer (64-bit) */
#define XHCI_OP_CONFIG     0x38

/* USBCMD bits */
#define XHCI_CMD_RUN        (1U << 0)
#define XHCI_CMD_HCRST      (1U << 1)   /* Host Controller Reset */
#define XHCI_CMD_INTE       (1U << 2)   /* Interrupt Enable */
#define XHCI_CMD_HSEE       (1U << 3)   /* Host System Error Enable */

/* USBSTS bits */
#define XHCI_STS_HCH        (1U << 0)   /* HC Halted */
#define XHCI_STS_HSE         (1U << 2)   /* Host System Error */
#define XHCI_STS_EINT        (1U << 3)   /* Event Interrupt */
#define XHCI_STS_PCD         (1U << 4)   /* Port Change Detect */
#define XHCI_STS_CNR         (1U << 11)  /* Controller Not Ready */

/* ── Port Registers (port_base + 0x10 * port_index) ────────────── */

#define XHCI_PORTSC_OFFSET  0x00
#define XHCI_PORTPMSC_OFFSET 0x04
#define XHCI_PORTLI_OFFSET  0x08
#define XHCI_PORTHLPMC_OFFSET 0x0C

/* PORTSC bits */
#define XHCI_PORTSC_CCS     (1U << 0)   /* Current Connect Status */
#define XHCI_PORTSC_PED     (1U << 1)   /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA     (1U << 3)   /* Over-current Active */
#define XHCI_PORTSC_PR      (1U << 4)   /* Port Reset */
#define XHCI_PORTSC_PLS_MASK (0xFU << 5) /* Port Link State */
#define XHCI_PORTSC_PLS_SHIFT 5
#define XHCI_PORTSC_PP      (1U << 9)   /* Port Power */
#define XHCI_PORTSC_SPEED_MASK (0xFU << 10) /* Port Speed */
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_PIC_MASK (3U << 14) /* Port Indicator Control */
#define XHCI_PORTSC_LWS     (1U << 16)  /* Link State Write Strobe */
#define XHCI_PORTSC_CSC     (1U << 17)  /* Connect Status Change */
#define XHCI_PORTSC_PEC     (1U << 18)  /* Port Enabled/Disabled Change */
#define XHCI_PORTSC_WRC     (1U << 19)  /* Warm Port Reset Change */
#define XHCI_PORTSC_OCC     (1U << 20)  /* Over-current Change */
#define XHCI_PORTSC_PRC     (1U << 21)  /* Port Reset Change */
#define XHCI_PORTSC_PLC     (1U << 22)  /* Port Link State Change */
#define XHCI_PORTSC_CEC     (1U << 23)  /* Config Error Change */
#define XHCI_PORTSC_CAS     (1U << 24)  /* Cold Attach Status */
#define XHCI_PORTSC_WCE     (1U << 25)  /* Wake on Connect Enable */
#define XHCI_PORTSC_WDE     (1U << 26)  /* Wake on Disconnect Enable */
#define XHCI_PORTSC_WOE     (1U << 27)  /* Wake on Over-current Enable */

/* PORTSC change bits mask — write 1 to clear */
#define XHCI_PORTSC_CHANGE_MASK \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
     XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

/* Port speed values (from PORTSC bits 13:10) */
#define XHCI_SPEED_FULL  1
#define XHCI_SPEED_LOW   2
#define XHCI_SPEED_HIGH  3
#define XHCI_SPEED_SUPER 4

/* Port Link State values */
#define XHCI_PLS_U0          0   /* Enabled */
#define XHCI_PLS_U3          3   /* Suspended */
#define XHCI_PLS_DISABLED     4
#define XHCI_PLS_RX_DETECT    5
#define XHCI_PLS_POLLING      7

/* ── Transfer Request Block (TRB) ──────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/* TRB Types (control[15:10]) */
#define XHCI_TRB_TYPE_SHIFT   10
#define XHCI_TRB_TYPE_MASK    (0x3FU << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_TYPE(t)      (((uint32_t)(t)) << XHCI_TRB_TYPE_SHIFT)

/* Transfer TRB types */
#define XHCI_TRB_NORMAL         1
#define XHCI_TRB_SETUP          2
#define XHCI_TRB_DATA           3
#define XHCI_TRB_STATUS         4
#define XHCI_TRB_LINK           6

/* Command TRB types */
#define XHCI_TRB_ENABLE_SLOT    9
#define XHCI_TRB_DISABLE_SLOT   10
#define XHCI_TRB_ADDRESS_DEVICE 11
#define XHCI_TRB_CONFIG_EP      12
#define XHCI_TRB_EVALUATE_CTX   13
#define XHCI_TRB_RESET_EP       14
#define XHCI_TRB_STOP_EP        15
#define XHCI_TRB_SET_TR_DEQUEUE 16
#define XHCI_TRB_NOOP_CMD       23

/* Event TRB types */
#define XHCI_TRB_TRANSFER_EVENT     32
#define XHCI_TRB_CMD_COMPLETION     33
#define XHCI_TRB_PORT_STATUS_CHANGE 34
#define XHCI_TRB_HOST_CONTROLLER    37

/* TRB control bits */
#define XHCI_TRB_CYCLE        (1U << 0)
#define XHCI_TRB_ENT          (1U << 1)  /* Evaluate Next TRB */
#define XHCI_TRB_ISP          (1U << 2)  /* Interrupt on Short Packet */
#define XHCI_TRB_NS           (1U << 3)  /* No Snoop */
#define XHCI_TRB_CH           (1U << 4)  /* Chain bit */
#define XHCI_TRB_IOC          (1U << 5)  /* Interrupt on Completion */
#define XHCI_TRB_IDT          (1U << 6)  /* Immediate Data */
#define XHCI_TRB_BSR          (1U << 9)  /* Block Set Address Request (in Address Device) */
#define XHCI_TRB_DIR_IN       (1U << 16) /* Data direction IN (for Data Stage TRB) */

/* TRB status/completion code extraction */
#define XHCI_TRB_COMP_CODE(s)    (((s) >> 24) & 0xFF)
#define XHCI_TRB_SLOT_ID(c)      (((c) >> 24) & 0xFF)
#define XHCI_TRB_EP_ID(c)        (((c) >> 16) & 0x1F)
#define XHCI_TRB_GET_TYPE(c)     (((c) >> 10) & 0x3F)

/* Completion codes */
#define XHCI_CC_SUCCESS           1
#define XHCI_CC_DATA_BUFFER_ERROR 2
#define XHCI_CC_BABBLE_ERROR      3
#define XHCI_CC_USB_TRANSACTION   4
#define XHCI_CC_TRB_ERROR         5
#define XHCI_CC_STALL             6
#define XHCI_CC_SHORT_PACKET      13
#define XHCI_CC_COMMAND_RING_STOPPED 24
#define XHCI_CC_COMMAND_ABORTED   25

/* ── xHCI Ring ──────────────────────────────────────────────────── */

#define XHCI_RING_SIZE 256  /* Number of TRBs per ring (power of 2 - 1 usable + 1 link) */

typedef struct {
    xhci_trb_t* trbs;        /* Virtual address of TRB array */
    uint32_t    trbs_phys;    /* Physical address */
    uint32_t    enqueue;      /* Enqueue index */
    uint32_t    dequeue;      /* Dequeue index */
    uint32_t    cycle;        /* Producer Cycle State */
    uint32_t    size;         /* Number of TRBs in ring */
} xhci_ring_t;

/* ── xHCI Event Ring Segment Table ──────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t ring_base;
    uint32_t ring_size;      /* Number of TRBs in segment */
    uint32_t reserved;
} xhci_erst_entry_t;

/* ── xHCI Interrupter Registers (at runtime base + 0x20 * n) ───── */

#define XHCI_IR_IMAN       0x00   /* Interrupter Management */
#define XHCI_IR_IMOD       0x04   /* Interrupter Moderation */
#define XHCI_IR_ERSTSZ     0x08   /* Event Ring Segment Table Size */
#define XHCI_IR_ERSTBA     0x10   /* Event Ring Segment Table Base Address (64-bit) */
#define XHCI_IR_ERDP       0x18   /* Event Ring Dequeue Pointer (64-bit) */

/* IMAN bits */
#define XHCI_IMAN_IP       (1U << 0)   /* Interrupt Pending */
#define XHCI_IMAN_IE       (1U << 1)   /* Interrupt Enable */

/* ── xHCI Slot Context ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t route_speed_entries;   /* Route String [19:0], Speed [23:20], Entries [31:27] */
    uint32_t latency_hub_port;     /* Max Exit Latency [15:0], Root Hub Port [23:16], Num Ports [31:24] */
    uint32_t parent_slot_port;     /* TT Hub Slot [7:0], TT Port [15:8], TTT [19:16], Interrupter [31:22] */
    uint32_t device_state;         /* USB Device Address [7:0], Slot State [31:27] */
    uint32_t reserved[4];
} xhci_slot_ctx_t;

/* ── xHCI Endpoint Context ──────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t ep_state_info;     /* EP State [2:0], Mult [9:8], MaxPStreams [14:10], LSA [15], Interval [23:16], MaxESITPayloadHi [31:24] */
    uint32_t ep_info2;          /* CErr [2:1], EP Type [5:3], HID [7], MaxBurstSize [15:8], MaxPacketSize [31:16] */
    uint64_t dequeue_ptr;       /* TR Dequeue Pointer [63:4], DCS [0] */
    uint32_t avg_trb_len;       /* Average TRB Length [15:0], MaxESITPayloadLo [31:16] */
    uint32_t reserved[3];
} xhci_ep_ctx_t;

/* Endpoint types (in ep_info2 bits[5:3]) */
#define XHCI_EP_TYPE_NOT_VALID      0
#define XHCI_EP_TYPE_ISOCH_OUT      1
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTERRUPT_OUT  3
#define XHCI_EP_TYPE_CONTROL        4
#define XHCI_EP_TYPE_ISOCH_IN       5
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTERRUPT_IN   7

/* ── xHCI Input Context ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t drop_flags;     /* Drop Context Flags */
    uint32_t add_flags;      /* Add Context Flags */
    uint32_t reserved[6];
} xhci_input_ctrl_ctx_t;

/* ── xHCI Extended Capability IDs ───────────────────────────────── */

#define XHCI_EXT_CAP_LEGACY    1     /* USB Legacy Support */
#define XHCI_EXT_CAP_PROTOCOL  2     /* Supported Protocol */
#define XHCI_EXT_CAP_POWER     3     /* Extended Power Management */

/* USB Legacy Support register offsets */
#define XHCI_USBLEGSUP_OFFSET  0x00
#define XHCI_USBLEGCTLSTS_OFFSET 0x04

/* USBLEGSUP bits */
#define XHCI_USBLEGSUP_BIOS_OWNED  (1U << 16)
#define XHCI_USBLEGSUP_OS_OWNED    (1U << 24)

/* ── Maximum devices we track ───────────────────────────────────── */

#define XHCI_MAX_DEVICES    32
#define XHCI_MAX_PORTS      32
#define XHCI_MAX_EPS        31    /* EP 0 + 15 IN + 15 OUT */

/* ── xHCI Device tracking ──────────────────────────────────────── */

typedef struct {
    int      slot_id;
    int      port;             /* Root hub port (0-based) */
    int      speed;            /* XHCI_SPEED_xxx */
    uint32_t out_ctx_phys;     /* Output Device Context physical address */
    void*    out_ctx;          /* Output Device Context virtual (identity-mapped) */
    uint32_t in_ctx_phys;      /* Input Context physical address */
    void*    in_ctx;           /* Input Context virtual */
    xhci_ring_t ep_ring[XHCI_MAX_EPS]; /* Transfer rings for endpoints */
    int      configured;
    int      is_hid_keyboard;
    int      is_hid_mouse;
    int      hid_kbd_ep_in;     /* DCI of keyboard interrupt IN endpoint */
    int      hid_mouse_ep_in;   /* DCI of mouse interrupt IN endpoint */
    uint16_t hid_max_packet;   /* Max packet size for HID endpoint */
    int      hid_kbd_iface;    /* bInterfaceNumber of the keyboard HID interface */
    int      hid_mouse_iface;  /* bInterfaceNumber of the mouse HID interface */
    /* Hub support */
    int      is_hub;           /* 1 if this device is a USB hub */
    int      hub_ports;        /* Number of downstream ports */
    int      parent_slot;      /* Slot ID of parent hub (0 = root hub) */
    int      parent_port;      /* Port on parent hub (1-based) */
    uint32_t route_string;     /* xHCI route string for this device */
} xhci_device_t;

/* ── xHCI Controller State ──────────────────────────────────────── */

typedef struct {
    volatile uint32_t* cap_base;    /* Capability registers */
    volatile uint32_t* op_base;     /* Operational registers */
    volatile uint32_t* rt_base;     /* Runtime registers */
    volatile uint32_t* db_base;     /* Doorbell registers */
    volatile uint32_t* port_base;   /* Port register set base */

    uint64_t mmio_phys;       /* Physical BAR0 address (may be >4GB) */
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    uint32_t context_size;    /* 32 or 64 bytes */
    uint8_t  irq_line;

    /* DCBAA */
    uint64_t* dcbaa;          /* Device Context Base Address Array */
    uint32_t  dcbaa_phys;

    /* Command Ring */
    xhci_ring_t cmd_ring;

    /* Event Ring */
    xhci_ring_t evt_ring;
    xhci_erst_entry_t* erst;   /* Event Ring Segment Table */
    uint32_t erst_phys;

    /* Scratchpad */
    uint64_t* scratchpad_array;
    uint32_t  scratchpad_array_phys;
    uint32_t  scratchpad_count;

    /* Devices */
    xhci_device_t devices[XHCI_MAX_DEVICES];
    int device_count;
    int enabled;
} xhci_controller_t;

/* ── Public API ─────────────────────────────────────────────────── */

void xhci_init(void);
int  xhci_is_enabled(void);
void xhci_irq_handler(void);
xhci_controller_t* xhci_get_controller(void);

/* Non-blocking event ring check — returns 0 if event found, -1 if none */
int  xhci_check_event(xhci_trb_t* out);

/* Queue an interrupt IN transfer (fire-and-forget, non-blocking) */
void xhci_queue_interrupt_in(int slot, int ep_dci, uint32_t buf_phys, int len);

/* Reset a halted/stalled endpoint and re-sync its transfer ring */
int xhci_reset_endpoint(int slot, int ep_dci);

/* Read EP state from Output Context (0=Disabled,1=Running,2=Halted,3=Stopped,4=Error) */
int xhci_read_ep_state(int slot, int ep_dci);

/* Read USBSTS register (check for HSE, HCH errors) */
uint32_t xhci_read_usbsts(void);

/* Deep diagnostics — dump event ring and EP state to serial+terminal */
void xhci_dump_evt_ring_diag(void);
void xhci_dump_ep_diag(int slot, int ep_dci);
int  xhci_get_irq_count(void);

/* USB transfer helpers for upper layers */
int xhci_control_transfer(int slot, uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                           void* data);
int xhci_interrupt_transfer(int slot, int ep_index, void* buf, int len);

#endif
