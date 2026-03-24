#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

/* Boot protocol keyboard report (8 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;    /* Bit field: Ctrl, Shift, Alt, GUI */
    uint8_t reserved;
    uint8_t keys[6];      /* Up to 6 keys pressed simultaneously */
} usb_kbd_report_t;

/* Boot protocol mouse report (3-4 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t buttons;      /* Bit 0=left, 1=right, 2=middle */
    int8_t  delta_x;
    int8_t  delta_y;
} usb_mouse_report_t;

/* HID modifier bits (in boot protocol keyboard report byte 0) */
#define USB_HID_MOD_LCTRL   (1U << 0)
#define USB_HID_MOD_LSHIFT  (1U << 1)
#define USB_HID_MOD_LALT    (1U << 2)
#define USB_HID_MOD_LGUI    (1U << 3)
#define USB_HID_MOD_RCTRL   (1U << 4)
#define USB_HID_MOD_RSHIFT  (1U << 5)
#define USB_HID_MOD_RALT    (1U << 6)
#define USB_HID_MOD_RGUI    (1U << 7)

/* HID Usage IDs (USB HID Usage Tables — Keyboard Page 0x07) */
#define USB_HID_KEY_A         0x04
#define USB_HID_KEY_Z         0x1D
#define USB_HID_KEY_1         0x1E
#define USB_HID_KEY_0         0x27
#define USB_HID_KEY_ENTER     0x28
#define USB_HID_KEY_ESCAPE    0x29
#define USB_HID_KEY_BACKSPACE 0x2A
#define USB_HID_KEY_TAB       0x2B
#define USB_HID_KEY_SPACE     0x2C
#define USB_HID_KEY_MINUS     0x2D
#define USB_HID_KEY_EQUAL     0x2E
#define USB_HID_KEY_LBRACKET  0x2F
#define USB_HID_KEY_RBRACKET  0x30
#define USB_HID_KEY_BACKSLASH 0x31
#define USB_HID_KEY_SEMICOLON 0x33
#define USB_HID_KEY_APOSTROPHE 0x34
#define USB_HID_KEY_GRAVE     0x35
#define USB_HID_KEY_COMMA     0x36
#define USB_HID_KEY_DOT       0x37
#define USB_HID_KEY_SLASH     0x38
#define USB_HID_KEY_CAPSLOCK  0x39
#define USB_HID_KEY_F1        0x3A
#define USB_HID_KEY_F2        0x3B
#define USB_HID_KEY_F3        0x3C
#define USB_HID_KEY_F4        0x3D
#define USB_HID_KEY_INSERT    0x49
#define USB_HID_KEY_HOME      0x4A
#define USB_HID_KEY_PAGEUP    0x4B
#define USB_HID_KEY_DELETE    0x4C
#define USB_HID_KEY_END       0x4D
#define USB_HID_KEY_PAGEDOWN  0x4E
#define USB_HID_KEY_RIGHT     0x4F
#define USB_HID_KEY_LEFT      0x50
#define USB_HID_KEY_DOWN      0x51
#define USB_HID_KEY_UP        0x52

/* Public API */
void usb_hid_init(void);
void usb_hid_poll(void);

#endif
