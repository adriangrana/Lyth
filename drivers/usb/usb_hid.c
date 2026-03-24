/* ================================================================
 * Lyth OS — USB HID Driver (Boot Protocol Keyboard + Mouse)
 *
 * Polls USB HID devices via the xHCI driver and injects events
 * into the existing keyboard/mouse input queues.
 * ================================================================ */

#include "usb_hid.h"
#include "usb.h"
#include "xhci.h"
#include "keyboard.h"
#include "mouse.h"
#include "serial.h"
#include "klog.h"
#include "physmem.h"
#include "paging.h"
#include "string.h"
#include "terminal.h"

/* ── Forward declarations for injection into existing drivers ───── */

extern void keyboard_inject_event(keyboard_event_type_t type, char character, unsigned char modifiers);
extern void mouse_inject_event(int dx, int dy, unsigned char buttons);

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static void delay_ms(int ms) {
    for (int i = 0; i < ms * 1000; i++)
        io_wait();
}

/* ── State ──────────────────────────────────────────────────────── */

static int usb_hid_ready = 0;

/* Per-device async state for non-blocking polling */
typedef struct {
    int      slot_id;
    int      ep_dci;
    uint32_t buf_phys;
    uint8_t* buf;
    int      buf_len;
    int      is_keyboard;
    int      is_mouse;
    int      hid_iface;       /* bInterfaceNumber for SET_REPORT */
    int      needs_reset;     /* Deferred halt recovery flag */
} hid_dev_t;

#define MAX_HID_DEVS 8
static hid_dev_t hid_devs[MAX_HID_DEVS];
static int hid_dev_count = 0;

static usb_kbd_report_t prev_kbd_report;

/* ── HID Usage ID → keyboard event translation  ────────────────── */

/* Map HID usage IDs (0x04–0x38) to ASCII characters (lowercase, US layout) */
static const char hid_to_ascii_lower[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=',
    [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`',
    [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

static const char hid_to_ascii_upper[256] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',
    [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+',
    [0x2F] = '{', [0x30] = '}', [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~',
    [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

/* ── Spanish layout: HID usage → character (overrides US for symbol keys) ── */

#define CHAR_ENYE_LOWER  ((char)0xA4)
#define CHAR_ENYE_UPPER  ((char)0xA5)
#define CHAR_CEDILLA_LO  ((char)0x87)
#define CHAR_CEDILLA_UP  ((char)0x80)
#define CHAR_INV_QUEST   ((char)0xA8)
#define CHAR_INV_EXCLAM  ((char)0xAD)
#define CHAR_FEMININE_O  ((char)0xA6)
#define CHAR_MASCULINE_O ((char)0xA7)
#define CHAR_MIDDLE_DOT  ((char)0xFA)
#define CHAR_NOT_SIGN    ((char)0xAA)

static char hid_to_es_char(uint8_t key, int shift, int altgr) {
    /* AltGr combinations (Right Alt on ES keyboard) */
    if (altgr) {
        switch (key) {
            case 0x1E: return '|';   /* 1 */
            case 0x1F: return '@';   /* 2 */
            case 0x20: return '#';   /* 3 */
            case 0x21: return '~';   /* 4 */
            case 0x23: return CHAR_NOT_SIGN; /* 6 */
            case 0x2F: return '[';   /* [ key = ` in ES */
            case 0x30: return ']';   /* ] key = + in ES */
            case 0x34: return '{';   /* ' key = accent in ES */
            case 0x35: return '\\';  /* ` key = º in ES */
            case 0x31: return '}';   /* \ key = ç in ES */
            case 0x64: return '|';   /* non-US \ (key between LShift and Z) */
            default: return 0;
        }
    }

    /* ES layout symbol key overrides */
    switch (key) {
        case 0x1E: return shift ? '!' : '1';
        case 0x1F: return shift ? '"' : '2';
        case 0x20: return shift ? CHAR_MIDDLE_DOT : '3';
        case 0x21: return shift ? '$' : '4';
        case 0x22: return shift ? '%' : '5';
        case 0x23: return shift ? '&' : '6';
        case 0x24: return shift ? '/' : '7';
        case 0x25: return shift ? '(' : '8';
        case 0x26: return shift ? ')' : '9';
        case 0x27: return shift ? '=' : '0';
        case 0x2D: return shift ? '?' : '\'';   /* - key → '/? */
        case 0x2E: return shift ? CHAR_INV_QUEST : CHAR_INV_EXCLAM; /* = key → ¡/¿ */
        case 0x2F: return shift ? '^' : '`';     /* [ key → `/^ */
        case 0x30: return shift ? '*' : '+';     /* ] key → +/* */
        case 0x31: return shift ? CHAR_CEDILLA_UP : CHAR_CEDILLA_LO; /* \ key → ç */
        case 0x33: return shift ? CHAR_ENYE_UPPER : CHAR_ENYE_LOWER; /* ; key → ñ */
        case 0x34: return shift ? '"' : '\'';    /* ' key → accent/dieresis */
        case 0x35: return shift ? CHAR_FEMININE_O : CHAR_MASCULINE_O; /* ` key → º/ª */
        case 0x36: return shift ? ';' : ',';
        case 0x37: return shift ? ':' : '.';
        case 0x38: return shift ? '_' : '-';
        case 0x64: return shift ? '>' : '<';     /* non-US \ → </> */
        default: return 0; /* Not an ES-overridden key */
    }
}

static int capslock_on = 0;
static int numlock_on = 0; /* Start OFF — matches most BIOS/UEFI default; our init turns LED on */

static char apply_capslock(char c) {
    if (!capslock_on) return c;
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    if (c == CHAR_ENYE_LOWER) return CHAR_ENYE_UPPER;
    if (c == CHAR_ENYE_UPPER) return CHAR_ENYE_LOWER;
    return c;
}

static void process_kbd_report(const usb_kbd_report_t* report) {
    unsigned char modifiers = 0;
    int shift = (report->modifiers & (USB_HID_MOD_LSHIFT | USB_HID_MOD_RSHIFT)) ? 1 : 0;
    int ctrl  = (report->modifiers & (USB_HID_MOD_LCTRL  | USB_HID_MOD_RCTRL))  ? 1 : 0;
    int lalt  = (report->modifiers & USB_HID_MOD_LALT) ? 1 : 0;
    int ralt  = (report->modifiers & USB_HID_MOD_RALT) ? 1 : 0;
    int altgr = ralt; /* Right Alt = AltGr on ES layout */

    if (shift) modifiers |= KEY_MOD_SHIFT;
    if (ctrl)  modifiers |= KEY_MOD_CTRL;
    if (lalt || ralt) modifiers |= KEY_MOD_ALT;

    keyboard_layout_t layout = keyboard_get_layout();
    int is_es = (layout == KEYBOARD_LAYOUT_ES);

    /* Process each key slot — only inject keys that weren't in the previous report */
    for (int i = 0; i < 6; i++) {
        uint8_t key = report->keys[i];
        if (key == 0) continue;
        if (key == 0x01) continue; /* ErrorRollOver */

        /* Check if this key was already in the previous report */
        int was_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (prev_kbd_report.keys[j] == key) {
                was_pressed = 1;
                break;
            }
        }
        if (was_pressed) continue;

        /* CapsLock toggle */
        if (key == USB_HID_KEY_CAPSLOCK) {
            capslock_on = !capslock_on;
            continue;
        }

        /* NumLock toggle */
        if (key == 0x53) {
            numlock_on = !numlock_on;
            continue;
        }

        /* Numpad Enter (0x58) */
        if (key == 0x58) {
            keyboard_inject_event(KEY_EVENT_ENTER, 0, modifiers);
            continue;
        }

        /* Numpad keys: numbers when NumLock on, navigation when off */
        if (key >= 0x59 && key <= 0x63) {
            if (numlock_on) {
                /* Numpad digits */
                static const char numpad_chars[] = {
                    '1','2','3','4','5','6','7','8','9','0','.'
                };
                char c = numpad_chars[key - 0x59];
                keyboard_inject_event(KEY_EVENT_CHAR, c, modifiers);
            } else {
                /* Numpad navigation */
                switch (key) {
                    case 0x59: keyboard_inject_event(KEY_EVENT_END, 0, modifiers); break;
                    case 0x5A: keyboard_inject_event(KEY_EVENT_DOWN, 0, modifiers); break;
                    case 0x5B: keyboard_inject_event(KEY_EVENT_PAGE_DOWN, 0, modifiers); break;
                    case 0x5C: keyboard_inject_event(KEY_EVENT_LEFT, 0, modifiers); break;
                    /* 0x5D = Numpad 5: no standard function without NumLock */
                    case 0x5E: keyboard_inject_event(KEY_EVENT_RIGHT, 0, modifiers); break;
                    case 0x5F: keyboard_inject_event(KEY_EVENT_HOME, 0, modifiers); break;
                    case 0x60: keyboard_inject_event(KEY_EVENT_UP, 0, modifiers); break;
                    case 0x61: keyboard_inject_event(KEY_EVENT_PAGE_UP, 0, modifiers); break;
                    case 0x62: keyboard_inject_event(KEY_EVENT_INSERT, 0, modifiers); break;
                    case 0x63: keyboard_inject_event(KEY_EVENT_DELETE, 0, modifiers); break;
                }
            }
            continue;
        }

        /* Numpad operators always produce characters */
        if (key == 0x54) { keyboard_inject_event(KEY_EVENT_CHAR, '/', modifiers); continue; }
        if (key == 0x55) { keyboard_inject_event(KEY_EVENT_CHAR, '*', modifiers); continue; }
        if (key == 0x56) { keyboard_inject_event(KEY_EVENT_CHAR, '-', modifiers); continue; }
        if (key == 0x57) { keyboard_inject_event(KEY_EVENT_CHAR, '+', modifiers); continue; }

        /* Special keys */
        if (key == USB_HID_KEY_ENTER) {
            keyboard_inject_event(KEY_EVENT_ENTER, 0, modifiers);
        } else if (key == USB_HID_KEY_BACKSPACE) {
            keyboard_inject_event(KEY_EVENT_BACKSPACE, 0, modifiers);
        } else if (key == USB_HID_KEY_TAB) {
            keyboard_inject_event(KEY_EVENT_TAB, 0, modifiers);
        } else if (key == USB_HID_KEY_DELETE) {
            keyboard_inject_event(KEY_EVENT_DELETE, 0, modifiers);
        } else if (key == USB_HID_KEY_INSERT) {
            keyboard_inject_event(KEY_EVENT_INSERT, 0, modifiers);
        } else if (key == USB_HID_KEY_HOME) {
            keyboard_inject_event(KEY_EVENT_HOME, 0, modifiers);
        } else if (key == USB_HID_KEY_END) {
            keyboard_inject_event(KEY_EVENT_END, 0, modifiers);
        } else if (key == USB_HID_KEY_PAGEUP) {
            keyboard_inject_event(KEY_EVENT_PAGE_UP, 0, modifiers);
        } else if (key == USB_HID_KEY_PAGEDOWN) {
            keyboard_inject_event(KEY_EVENT_PAGE_DOWN, 0, modifiers);
        } else if (key == USB_HID_KEY_UP) {
            keyboard_inject_event(KEY_EVENT_UP, 0, modifiers);
        } else if (key == USB_HID_KEY_DOWN) {
            keyboard_inject_event(KEY_EVENT_DOWN, 0, modifiers);
        } else if (key == USB_HID_KEY_LEFT) {
            keyboard_inject_event(KEY_EVENT_LEFT, 0, modifiers);
        } else if (key == USB_HID_KEY_RIGHT) {
            keyboard_inject_event(KEY_EVENT_RIGHT, 0, modifiers);
        } else if (key == USB_HID_KEY_F1) {
            keyboard_inject_event(KEY_EVENT_F1, 0, modifiers);
        } else if (key == USB_HID_KEY_F2) {
            keyboard_inject_event(KEY_EVENT_F2, 0, modifiers);
        } else if (key == USB_HID_KEY_F3) {
            keyboard_inject_event(KEY_EVENT_F3, 0, modifiers);
        } else if (key == USB_HID_KEY_F4) {
            keyboard_inject_event(KEY_EVENT_F4, 0, modifiers);
        } else if (key == USB_HID_KEY_ESCAPE) {
            keyboard_inject_event(KEY_EVENT_CTRL_C, 0, KEY_MOD_CTRL);
        } else if (ctrl && !altgr && key >= USB_HID_KEY_A && key <= USB_HID_KEY_Z) {
            char c = (char)('a' + (key - USB_HID_KEY_A));
            if (c == 'c')
                keyboard_inject_event(KEY_EVENT_CTRL_C, 0, modifiers);
            else
                keyboard_inject_event(KEY_EVENT_CHAR, c, modifiers);
        } else {
            char c = 0;

            /* Try ES layout first for symbol/number keys */
            if (is_es) {
                c = hid_to_es_char(key, shift, altgr);
            }

            /* Fall back to US ASCII tables for letters and other keys */
            if (c == 0 && !altgr) {
                c = shift ? hid_to_ascii_upper[key] : hid_to_ascii_lower[key];
            }

            /* Apply CapsLock to letters */
            if (c != 0) {
                c = apply_capslock(c);
                keyboard_inject_event(KEY_EVENT_CHAR, c, modifiers);
            }
        }
    }

    /* Save for next comparison */
    prev_kbd_report = *report;
}

static void process_mouse_report(const usb_mouse_report_t* report) {
    mouse_inject_event((int)report->delta_x, (int)report->delta_y, report->buttons);
}

/* ── Polling (non-blocking — drains event ring for Transfer Events) ── */

void usb_hid_poll(void) {
    if (!usb_hid_ready) return;

    static int diag_events = 0;       /* total events seen */
    static int diag_kbd_reports = 0;  /* keyboard reports processed */
    static int diag_mouse_reports = 0;/* mouse reports processed */
    static int diag_calls = 0;        /* poll call counter */
    static int diag_printed = 0;      /* one-time terminal diagnostic */
    static int diag_errors = 0;       /* transfer errors seen */
    diag_calls++;

    /* First diagnostic at poll #1000 */
    if (diag_calls == 1000) {
        serial_print("[hid-poll] STATUS: calls=");
        serial_print_uint(diag_calls);
        serial_print(" evts=");
        serial_print_uint(diag_events);
        serial_print(" kbd=");
        serial_print_uint(diag_kbd_reports);
        serial_print(" mouse=");
        serial_print_uint(diag_mouse_reports);
        serial_print(" errs=");
        serial_print_uint(diag_errors);
        serial_print(" irqs=");
        serial_print_uint(xhci_get_irq_count());
        serial_print("\n");

        for (int i = 0; i < hid_dev_count; i++) {
            hid_dev_t* hd = &hid_devs[i];
            xhci_dump_ep_diag(hd->slot_id, hd->ep_dci);
        }

        /* Always dump event ring state at first diagnostic */
        xhci_dump_evt_ring_diag();
    }

    /* Deferred halt recovery: reset halted endpoints BEFORE draining events.
       This avoids xhci_send_command consuming events mid-drain. */
    for (int i = 0; i < hid_dev_count; i++) {
        hid_dev_t* hd = &hid_devs[i];
        if (hd->needs_reset) {
            hd->needs_reset = 0;
            xhci_reset_endpoint(hd->slot_id, hd->ep_dci);
            /* Re-queue after reset */
            memset(hd->buf, 0, hd->buf_len);
            xhci_queue_interrupt_in(hd->slot_id, hd->ep_dci,
                                     hd->buf_phys, hd->buf_len);
        }
    }

    xhci_trb_t evt;
    while (xhci_check_event(&evt) == 0) {
        uint32_t type = XHCI_TRB_GET_TYPE(evt.control);

        /* Diagnostic: show first 5 events of any type */
        if (diag_events < 5) {
            serial_print("[hid-poll] evt type=");
            serial_print_uint(type);
            serial_print(" slot=");
            serial_print_uint(XHCI_TRB_SLOT_ID(evt.control));
            serial_print(" ep=");
            serial_print_uint(XHCI_TRB_EP_ID(evt.control));
            serial_print(" cc=");
            serial_print_uint(XHCI_TRB_COMP_CODE(evt.status));
            serial_print("\n");
        }
        diag_events++;

        if (type != XHCI_TRB_TRANSFER_EVENT)
            continue; /* Port status change, etc. — skip */

        uint8_t slot_id = XHCI_TRB_SLOT_ID(evt.control);
        uint8_t ep_id   = XHCI_TRB_EP_ID(evt.control);
        uint8_t cc      = XHCI_TRB_COMP_CODE(evt.status);

        /* Find matching HID device */
        int matched = 0;
        
        for (int i = 0; i < hid_dev_count; i++) {
            hid_dev_t* hd = &hid_devs[i];
            if (hd->slot_id != slot_id || hd->ep_dci != ep_id)
                continue;
            matched = 1;

            if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                int residual = evt.status & 0xFFFFFF;
                int actual = hd->buf_len - residual;
                if (actual > 0) {
                    if (hd->is_keyboard) {
                        diag_kbd_reports++;
                        process_kbd_report((usb_kbd_report_t*)hd->buf);
                    }
                    if (hd->is_mouse) {
                        diag_mouse_reports++;
                        process_mouse_report((usb_mouse_report_t*)hd->buf);
                    }
                }
            } else {
                diag_errors++;
                if (diag_errors <= 10) {
                    serial_print("[hid-poll] Transfer error cc=");
                    serial_print_uint(cc);
                    serial_print(" slot=");
                    serial_print_uint(slot_id);
                    serial_print(" ep=");
                    serial_print_uint(ep_id);
                    serial_print("\n");
                }
                /* Show first 5 errors on serial for diagnosis */
                /* Do NOT call xhci_reset_endpoint here!
                   It uses xhci_send_command which consumes events
                   from the shared event ring, stealing events that
                   belong to other HID devices. Mark for deferred reset. */
                hd->needs_reset = 1;
            }

            /* Re-queue the interrupt IN transfer for next data */
            memset(hd->buf, 0, hd->buf_len);
            xhci_queue_interrupt_in(hd->slot_id, hd->ep_dci,
                                     hd->buf_phys, hd->buf_len);
            break;
        }

        /* Diagnostic: unmatched transfer event */
        if (!matched && diag_events <= 10) {
            serial_print("[hid-poll] UNMATCHED transfer slot=");
            serial_print_uint(slot_id);
            serial_print(" ep=");
            serial_print_uint(ep_id);
            serial_print("\n");
        }
    }

    /* Second diagnostic at poll #5000: full dump + IMAN/ERDP */
    if (!diag_printed && diag_calls == 5000) {
        diag_printed = 1;
        for (int i = 0; i < hid_dev_count; i++) {
            hid_dev_t* hd = &hid_devs[i];
            xhci_dump_ep_diag(hd->slot_id, hd->ep_dci);
        }
        xhci_dump_evt_ring_diag();
    }
}

/* ── Initialization ─────────────────────────────────────────────── */

void usb_hid_init(void) {
    if (!xhci_is_enabled()) return;

    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    hid_dev_count = 0;

    extern xhci_controller_t* xhci_get_controller(void);
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl) return;

    /* Allocate a temporary DMA buffer for LED control */
    uint32_t led_buf_phys = physmem_alloc_frame();
    if (!led_buf_phys) {
        serial_print("[usb-hid] Failed to allocate LED buffer\n");
        return;
    }
    paging_map_mmio(led_buf_phys);
    uint8_t* led_buf = (uint8_t*)(uintptr_t)led_buf_phys;

    /* Build per-device async state — separate entries for kbd and mouse */
    for (int i = 0; i < ctrl->device_count && hid_dev_count < MAX_HID_DEVS; i++) {
        xhci_device_t* dev = &ctrl->devices[i];
        if (!dev->configured) continue;

        /* Keyboard endpoint */
        if (dev->is_hid_keyboard && dev->hid_kbd_ep_in != 0 && hid_dev_count < MAX_HID_DEVS) {
            hid_dev_t* hd = &hid_devs[hid_dev_count];
            hd->slot_id     = dev->slot_id;
            hd->ep_dci      = dev->hid_kbd_ep_in;
            hd->is_keyboard = 1;
            hd->is_mouse    = 0;
            hd->hid_iface   = dev->hid_kbd_iface;
            /* Use max_packet for TRB length to avoid BABBLE if device
               sends more than the struct size (Logitech receivers do). */
            hd->buf_len     = (dev->hid_max_packet > 0) ? (int)dev->hid_max_packet : (int)sizeof(usb_kbd_report_t);

            hd->buf_phys = physmem_alloc_frame();
            if (!hd->buf_phys) {
                serial_print("[usb-hid] Failed to allocate DMA buffer\n");
            } else {
                paging_map_mmio(hd->buf_phys);
                hd->buf = (uint8_t*)(uintptr_t)hd->buf_phys;
                memset(hd->buf, 0, PHYSMEM_FRAME_SIZE);

                /* Skip LED SET_REPORT — causes some Logitech receivers
                   to stop sending keyboard reports entirely. */

                hid_dev_count++;
            }
        }

        /* Mouse endpoint */
        if (dev->is_hid_mouse && dev->hid_mouse_ep_in != 0 && hid_dev_count < MAX_HID_DEVS) {
            hid_dev_t* hd = &hid_devs[hid_dev_count];
            hd->slot_id     = dev->slot_id;
            hd->ep_dci      = dev->hid_mouse_ep_in;
            hd->is_keyboard = 0;
            hd->is_mouse    = 1;
            hd->hid_iface   = dev->hid_mouse_iface;
            /* Use max_packet for TRB length to avoid BABBLE if device
               sends more than the struct size (Logitech receivers do). */
            hd->buf_len     = (dev->hid_max_packet > 0) ? (int)dev->hid_max_packet : (int)sizeof(usb_mouse_report_t);

            hd->buf_phys = physmem_alloc_frame();
            if (!hd->buf_phys) {
                serial_print("[usb-hid] Failed to allocate DMA buffer\n");
            } else {
                paging_map_mmio(hd->buf_phys);
                hd->buf = (uint8_t*)(uintptr_t)hd->buf_phys;
                memset(hd->buf, 0, PHYSMEM_FRAME_SIZE);
                hid_dev_count++;
            }
        }
    }

    /* Pre-queue ONE interrupt IN transfer per device.
       Only one TRB per device avoids race conditions since all TRBs
       share the same DMA buffer.  Re-queue happens in usb_hid_poll()
       after each completion. */
    for (int i = 0; i < hid_dev_count; i++) {
        hid_dev_t* hd = &hid_devs[i];
        xhci_queue_interrupt_in(hd->slot_id, hd->ep_dci,
                                 hd->buf_phys, hd->buf_len);

        /* Verify endpoint is Running after doorbell ring */
        int ep_st = xhci_read_ep_state(hd->slot_id, hd->ep_dci);
        serial_print("[usb-hid] Queued TRB slot=");
        serial_print_uint(hd->slot_id);
        serial_print(" ep=");
        serial_print_uint(hd->ep_dci);
        serial_print(" len=");
        serial_print_uint(hd->buf_len);
        serial_print(" ep_state=");
        serial_print_uint(ep_st);
        serial_print("\n");
        if (ep_st != 1) {
            serial_print("[usb-hid] WARN: EP state not Running\n");
        }
    }

    /* Keyboard blocking test: try a single blocking interrupt transfer
       to check if the keyboard device responds at all. Wait up to 2s. */
    for (int i = 0; i < hid_dev_count; i++) {
        hid_dev_t* hd = &hid_devs[i];
        if (!hd->is_keyboard) continue;

        serial_print("[usb-hid] KBD blocking test: slot=");
        serial_print_uint(hd->slot_id);
        serial_print(" ep=");
        serial_print_uint(hd->ep_dci);
        serial_print("\n");

        /* The pre-queued TRB is already pending. Just wait for a
           Transfer Event matching this endpoint for up to 2 seconds. */
        {
            xhci_trb_t evt;
            int got_kbd = 0;
            for (int attempt = 0; attempt < 2000; attempt++) {
                if (xhci_check_event(&evt) == 0) {
                    uint32_t type = XHCI_TRB_GET_TYPE(evt.control);
                    if (type == XHCI_TRB_TRANSFER_EVENT &&
                        XHCI_TRB_SLOT_ID(evt.control) == (uint8_t)hd->slot_id &&
                        XHCI_TRB_EP_ID(evt.control) == (uint8_t)hd->ep_dci) {
                        uint8_t cc = XHCI_TRB_COMP_CODE(evt.status);
                        serial_print("[usb-hid] KBD blocking test: cc=");
                        serial_print_uint(cc);
                        serial_print("\n");
                        got_kbd = 1;
                        /* Re-queue for async polling */
                        memset(hd->buf, 0, hd->buf_len);
                        xhci_queue_interrupt_in(hd->slot_id, hd->ep_dci,
                                                 hd->buf_phys, hd->buf_len);
                        break;
                    }
                    /* Not our event — some other device, just continue */
                }
                delay_ms(1);
            }
            if (!got_kbd) {
                serial_print("[usb-hid] KBD blocking test: TIMEOUT\n");
                /* Re-queue anyway for async polling */
                xhci_queue_interrupt_in(hd->slot_id, hd->ep_dci,
                                         hd->buf_phys, hd->buf_len);
            }
        }
        break; /* Only test first keyboard */
    }

    physmem_free_frame(led_buf_phys);
    usb_hid_ready = 1;

    /* Log summary */
    int kbd_count = 0, mouse_count = 0;
    for (int i = 0; i < hid_dev_count; i++) {
        if (hid_devs[i].is_keyboard) kbd_count++;
        if (hid_devs[i].is_mouse) mouse_count++;
    }
    if (kbd_count > 0) {
        serial_print("[usb-hid] USB keyboard(s): ");
        serial_print_uint(kbd_count);
        serial_print(" (async polling active)\n");
        klog_write(KLOG_LEVEL_INFO, "usb", "USB keyboard detected");
    }
    if (mouse_count > 0) {
        serial_print("[usb-hid] USB mouse(s): ");
        serial_print_uint(mouse_count);
        serial_print(" (async polling active)\n");
        klog_write(KLOG_LEVEL_INFO, "usb", "USB mouse detected");
    }
    if (kbd_count == 0 && mouse_count == 0) {
        serial_print("[usb-hid] No HID devices found\n");
    }
}
