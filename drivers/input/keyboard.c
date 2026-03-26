#include "keyboard.h"
#include "klog.h"

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb_kb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void kb_io_wait(void) {
    outb_kb(0x80, 0);
}

static int kb_wait_input_ready(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if ((inb(0x64) & 0x02) == 0)
            return 1;
    }
    return 0;
}

static int kb_wait_output_ready(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (inb(0x64) & 0x01)
            return 1;
    }
    return 0;
}

static void kb_flush_output_buffer(void) {
    for (int i = 0; i < 32; i++) {
        if ((inb(0x64) & 0x01) == 0)
            return;
        (void)inb(0x60);
        kb_io_wait();
    }
}

void keyboard_init(void) {
    unsigned char config;

    /* Flush any pending data from the controller */
    kb_flush_output_buffer();

    /* Enable first PS/2 port (keyboard) */
    if (!kb_wait_input_ready()) {
        klog_write(KLOG_LEVEL_WARN, "kbd", "Timeout habilitando puerto PS/2");
        return;
    }
    outb_kb(0x64, 0xAE);   /* Enable first PS/2 port */
    kb_io_wait();

    /* Read controller configuration byte */
    if (!kb_wait_input_ready()) return;
    outb_kb(0x64, 0x20);   /* Read config command */
    if (!kb_wait_output_ready()) {
        klog_write(KLOG_LEVEL_WARN, "kbd", "No se pudo leer config i8042");
        return;
    }
    config = inb(0x60);

    /* Set bit 0: enable IRQ1 (keyboard interrupt)
       Clear bit 4: enable first PS/2 port clock */
    config |= 0x01;
    config &= (unsigned char)~0x10;

    /* Write config back */
    if (!kb_wait_input_ready()) return;
    outb_kb(0x64, 0x60);   /* Write config command */
    if (!kb_wait_input_ready()) return;
    outb_kb(0x60, config);
    kb_io_wait();

    /* Flush again after config change */
    kb_flush_output_buffer();

    klog_write(KLOG_LEVEL_INFO, "kbd", "Controlador PS/2 teclado inicializado");
}

#define KEYBOARD_EVENT_QUEUE_SIZE 256

static keyboard_event_t event_queue[KEYBOARD_EVENT_QUEUE_SIZE];
static int event_head = 0;
static int event_tail = 0;
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int altgr_pressed = 0;
static int capslock_enabled = 0;
static int numlock_enabled = 1;
static int scancode_set = 1;
static int set2_break_prefix = 0;
static int extended_prefix = 0;
static keyboard_layout_t active_layout = KEYBOARD_LAYOUT_US;

#define KEYBOARD_CHAR_ENYE_LOWER ((char)0xA4)
#define KEYBOARD_CHAR_ENYE_UPPER ((char)0xA5)
#define KEYBOARD_CHAR_C_CEDILLA_LOWER ((char)0x87)
#define KEYBOARD_CHAR_C_CEDILLA_UPPER ((char)0x80)
#define KEYBOARD_CHAR_INVERTED_QUESTION ((char)0xA8)
#define KEYBOARD_CHAR_INVERTED_EXCLAMATION ((char)0xAD)
#define KEYBOARD_CHAR_FEMININE_ORDINAL ((char)0xA6)
#define KEYBOARD_CHAR_MASCULINE_ORDINAL ((char)0xA7)
#define KEYBOARD_CHAR_MIDDLE_DOT ((char)0xFA)
#define KEYBOARD_CHAR_NOT_SIGN ((char)0xAA)

static char apply_caps(char c) {
    if (!capslock_enabled) {
        return c;
    }

    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }

    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }

    switch (c) {
        case KEYBOARD_CHAR_ENYE_LOWER:
            return KEYBOARD_CHAR_ENYE_UPPER;
        case KEYBOARD_CHAR_ENYE_UPPER:
            return KEYBOARD_CHAR_ENYE_LOWER;
        case KEYBOARD_CHAR_C_CEDILLA_LOWER:
            return KEYBOARD_CHAR_C_CEDILLA_UPPER;
        case KEYBOARD_CHAR_C_CEDILLA_UPPER:
            return KEYBOARD_CHAR_C_CEDILLA_LOWER;
        default:
            return c;
    }

    return c;
}

static int altgr_is_active(void) {
    return altgr_pressed || (ctrl_pressed && alt_pressed);
}

static char translate_set1_altgr(unsigned char scancode, char current) {
    if (active_layout != KEYBOARD_LAYOUT_ES || !altgr_is_active()) {
        return current;
    }

    switch (scancode) {
        case 0x02: return '|';
        case 0x03: return '@';
        case 0x04: return '#';
        case 0x05: return '~';
        case 0x07: return KEYBOARD_CHAR_NOT_SIGN;
        case 0x1A: return '[';
        case 0x1B: return ']';
        case 0x28: return '{';
        case 0x29: return '\\';
        case 0x2B: return '}';
        case 0x56: return '|';
        default:   return current;
    }
}

static char translate_set2_altgr(unsigned char scancode, char current) {
    if (active_layout != KEYBOARD_LAYOUT_ES || !altgr_is_active()) {
        return current;
    }

    switch (scancode) {
        case 0x16: return '|';
        case 0x1E: return '@';
        case 0x26: return '#';
        case 0x25: return '~';
        case 0x36: return KEYBOARD_CHAR_NOT_SIGN;
        case 0x54: return '[';
        case 0x5B: return ']';
        case 0x52: return '{';
        case 0x0E: return '\\';
        case 0x5D: return '}';
        case 0x61: return '|';
        default:   return current;
    }
}

static char translate_set1_layout(unsigned char scancode, char current) {
    if (active_layout != KEYBOARD_LAYOUT_ES) {
        return current;
    }

    if (altgr_is_active()) {
        return translate_set1_altgr(scancode, current);
    }

    switch (scancode) {
        case 0x02: return shift_pressed ? '!' : '1';
        case 0x03: return shift_pressed ? '"' : '2';
        case 0x04: return shift_pressed ? KEYBOARD_CHAR_MIDDLE_DOT : '3';
        case 0x05: return shift_pressed ? '$' : '4';
        case 0x06: return shift_pressed ? '%' : '5';
        case 0x07: return shift_pressed ? '&' : '6';
        case 0x08: return shift_pressed ? '/' : '7';
        case 0x09: return shift_pressed ? '(' : '8';
        case 0x1A: return shift_pressed ? '^' : '`';
        case 0x1B: return shift_pressed ? '*' : '+';
        case 0x27: return shift_pressed ? KEYBOARD_CHAR_ENYE_UPPER : KEYBOARD_CHAR_ENYE_LOWER;
        case 0x28: return shift_pressed ? '"' : '\'';
        case 0x2B: return shift_pressed ? KEYBOARD_CHAR_C_CEDILLA_UPPER : KEYBOARD_CHAR_C_CEDILLA_LOWER;
        case 0x33: return shift_pressed ? ';' : ',';
        case 0x34: return shift_pressed ? ':' : '.';
        case 0x35: return shift_pressed ? '_' : '-';
        case 0x56: return shift_pressed ? '>' : '<';
        default:   return current;
    }
}

static char translate_set2_layout(unsigned char scancode, char current) {
    if (active_layout != KEYBOARD_LAYOUT_ES) {
        return current;
    }

    if (altgr_is_active()) {
        return translate_set2_altgr(scancode, current);
    }

    switch (scancode) {
        case 0x16: return shift_pressed ? '!' : '1';
        case 0x1E: return shift_pressed ? '"' : '2';
        case 0x26: return shift_pressed ? KEYBOARD_CHAR_MIDDLE_DOT : '3';
        case 0x25: return shift_pressed ? '$' : '4';
        case 0x2E: return shift_pressed ? '%' : '5';
        case 0x36: return shift_pressed ? '&' : '6';
        case 0x3D: return shift_pressed ? '/' : '7';
        case 0x3E: return shift_pressed ? '(' : '8';
        case 0x46: return shift_pressed ? ')' : '9';
        case 0x45: return shift_pressed ? '=' : '0';
        case 0x0E: return shift_pressed ? KEYBOARD_CHAR_FEMININE_ORDINAL : KEYBOARD_CHAR_MASCULINE_ORDINAL;
        case 0x4E: return shift_pressed ? '?' : '\'';
        case 0x55: return shift_pressed ? KEYBOARD_CHAR_INVERTED_QUESTION : KEYBOARD_CHAR_INVERTED_EXCLAMATION;
        case 0x54: return shift_pressed ? '^' : '`';
        case 0x5B: return shift_pressed ? '*' : '+';
        case 0x4C: return shift_pressed ? KEYBOARD_CHAR_ENYE_UPPER : KEYBOARD_CHAR_ENYE_LOWER;
        case 0x52: return shift_pressed ? '"' : '\'';
        case 0x5D: return shift_pressed ? KEYBOARD_CHAR_C_CEDILLA_UPPER : KEYBOARD_CHAR_C_CEDILLA_LOWER;
        case 0x41: return shift_pressed ? ';' : ',';
        case 0x49: return shift_pressed ? ':' : '.';
        case 0x4A: return shift_pressed ? '_' : '-';
        case 0x61: return shift_pressed ? '>' : '<';
        default:   return current;
    }
}

static void queue_event(keyboard_event_type_t type, char character) {
    int next_head = (event_head + 1) % KEYBOARD_EVENT_QUEUE_SIZE;

    if (next_head == event_tail) {
        return;
    }

    event_queue[event_head].type = type;
    event_queue[event_head].character = character;
    event_queue[event_head].modifiers = 0;

    if (shift_pressed) {
        event_queue[event_head].modifiers |= KEY_MOD_SHIFT;
    }
    if (ctrl_pressed) {
        event_queue[event_head].modifiers |= KEY_MOD_CTRL;
    }
    if (alt_pressed || altgr_pressed) {
        event_queue[event_head].modifiers |= KEY_MOD_ALT;
    }

    event_head = next_head;
}

static int event_is_available(void) {
    return event_head != event_tail;
}

static void queue_char(char c) {
    queue_event(KEY_EVENT_CHAR, c);
}

static int queue_function_event_set1(unsigned char scancode) {
    switch (scancode) {
        case 0x3B:
            queue_event(KEY_EVENT_F1, 0);
            return 1;
        case 0x3C:
            queue_event(KEY_EVENT_F2, 0);
            return 1;
        case 0x3D:
            queue_event(KEY_EVENT_F3, 0);
            return 1;
        case 0x3E:
            queue_event(KEY_EVENT_F4, 0);
            return 1;
        default:
            return 0;
    }
}

static int queue_function_event_set2(unsigned char scancode) {
    switch (scancode) {
        case 0x05:
            queue_event(KEY_EVENT_F1, 0);
            return 1;
        case 0x06:
            queue_event(KEY_EVENT_F2, 0);
            return 1;
        case 0x04:
            queue_event(KEY_EVENT_F3, 0);
            return 1;
        case 0x0C:
            queue_event(KEY_EVENT_F4, 0);
            return 1;
        default:
            return 0;
    }
}

static void queue_navigation_event(unsigned char scancode) {
    if (scancode == 0x48 || scancode == 0x75) {
        queue_event(KEY_EVENT_UP, 0);
        return;
    }

    if (scancode == 0x50 || scancode == 0x72) {
        queue_event(KEY_EVENT_DOWN, 0);
        return;
    }

    if (scancode == 0x4B || scancode == 0x6B) {
        queue_event(KEY_EVENT_LEFT, 0);
        return;
    }

    if (scancode == 0x4D || scancode == 0x74) {
        queue_event(KEY_EVENT_RIGHT, 0);
        return;
    }

    if (scancode == 0x52 || scancode == 0x70) {
        queue_event(KEY_EVENT_INSERT, 0);
        return;
    }

    if (scancode == 0x53 || scancode == 0x71) {
        queue_event(KEY_EVENT_DELETE, 0);
        return;
    }

    if (scancode == 0x47 || scancode == 0x6C) {
        queue_event(KEY_EVENT_HOME, 0);
        return;
    }

    if (scancode == 0x4F || scancode == 0x69) {
        queue_event(KEY_EVENT_END, 0);
        return;
    }

    if (scancode == 0x49 || scancode == 0x7D) {
        queue_event(KEY_EVENT_PAGE_UP, 0);
        return;
    }

    if (scancode == 0x51 || scancode == 0x7A) {
        queue_event(KEY_EVENT_PAGE_DOWN, 0);
        return;
    }

    /* Windows / Super key: set1 0x5B (left), 0x5C (right); set2 0x1F, 0x27 */
    if (scancode == 0x5B || scancode == 0x5C ||
        scancode == 0x1F || scancode == 0x27) {
        queue_event(KEY_EVENT_SUPER, 0);
        return;
    }
}

static int queue_keypad_or_navigation_set1(unsigned char scancode) {
    switch (scancode) {
        case 0x47:
            if (numlock_enabled) {
                queue_char('7');
            } else {
                queue_event(KEY_EVENT_HOME, 0);
            }
            return 1;
        case 0x48:
            if (numlock_enabled) {
                queue_char('8');
            } else {
                queue_event(KEY_EVENT_UP, 0);
            }
            return 1;
        case 0x49:
            if (numlock_enabled) {
                queue_char('9');
            } else {
                queue_event(KEY_EVENT_PAGE_UP, 0);
            }
            return 1;
        case 0x4B:
            if (numlock_enabled) {
                queue_char('4');
            } else {
                queue_event(KEY_EVENT_LEFT, 0);
            }
            return 1;
        case 0x4C:
            if (numlock_enabled) {
                queue_char('5');
            }
            return 1;
        case 0x4D:
            if (numlock_enabled) {
                queue_char('6');
            } else {
                queue_event(KEY_EVENT_RIGHT, 0);
            }
            return 1;
        case 0x4F:
            if (numlock_enabled) {
                queue_char('1');
            } else {
                queue_event(KEY_EVENT_END, 0);
            }
            return 1;
        case 0x50:
            if (numlock_enabled) {
                queue_char('2');
            } else {
                queue_event(KEY_EVENT_DOWN, 0);
            }
            return 1;
        case 0x51:
            if (numlock_enabled) {
                queue_char('3');
            } else {
                queue_event(KEY_EVENT_PAGE_DOWN, 0);
            }
            return 1;
        case 0x52:
            if (numlock_enabled) {
                queue_char('0');
            } else {
                queue_event(KEY_EVENT_INSERT, 0);
            }
            return 1;
        case 0x53:
            if (numlock_enabled) {
                queue_char('.');
            } else {
                queue_event(KEY_EVENT_DELETE, 0);
            }
            return 1;
        default:
            return 0;
    }
}

static int queue_keypad_or_navigation_set2(unsigned char scancode) {
    switch (scancode) {
        case 0x4A:
            queue_char('/');
            return 1;
        case 0x6C:
            if (numlock_enabled) {
                queue_char('7');
            } else {
                queue_event(KEY_EVENT_HOME, 0);
            }
            return 1;
        case 0x75:
            if (numlock_enabled) {
                queue_char('8');
            } else {
                queue_event(KEY_EVENT_UP, 0);
            }
            return 1;
        case 0x7D:
            if (numlock_enabled) {
                queue_char('9');
            } else {
                queue_event(KEY_EVENT_PAGE_UP, 0);
            }
            return 1;
        case 0x6B:
            if (numlock_enabled) {
                queue_char('4');
            } else {
                queue_event(KEY_EVENT_LEFT, 0);
            }
            return 1;
        case 0x73:
            if (numlock_enabled) {
                queue_char('5');
            }
            return 1;
        case 0x74:
            if (numlock_enabled) {
                queue_char('6');
            } else {
                queue_event(KEY_EVENT_RIGHT, 0);
            }
            return 1;
        case 0x69:
            if (numlock_enabled) {
                queue_char('1');
            } else {
                queue_event(KEY_EVENT_END, 0);
            }
            return 1;
        case 0x72:
            if (numlock_enabled) {
                queue_char('2');
            } else {
                queue_event(KEY_EVENT_DOWN, 0);
            }
            return 1;
        case 0x7A:
            if (numlock_enabled) {
                queue_char('3');
            } else {
                queue_event(KEY_EVENT_PAGE_DOWN, 0);
            }
            return 1;
        case 0x70:
            if (numlock_enabled) {
                queue_char('0');
            } else {
                queue_event(KEY_EVENT_INSERT, 0);
            }
            return 1;
        case 0x71:
            if (numlock_enabled) {
                queue_char('.');
            } else {
                queue_event(KEY_EVENT_DELETE, 0);
            }
            return 1;
        case 0x7B:
            queue_char('-');
            return 1;
        case 0x79:
            queue_char('+');
            return 1;
        default:
            return 0;
    }
}

static char map_set1(unsigned char scancode) {
    static const char keymap_normal[128] = {
        0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0,
        '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
        0,
        '*',
        0,
        ' ',
    };

    static const char keymap_shift[128] = {
        0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        0,
        '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
        0,
        '*',
        0,
        ' ',
    };

    char c = shift_pressed ? keymap_shift[scancode] : keymap_normal[scancode];
    c = translate_set1_layout(scancode, c);
    return apply_caps(c);
}

static char map_set2(unsigned char scancode) {
    char c = 0;

    switch (scancode) {
        case 0x16: c = shift_pressed ? '!' : '1'; break;
        case 0x1E: c = shift_pressed ? '@' : '2'; break;
        case 0x26: c = shift_pressed ? '#' : '3'; break;
        case 0x25: c = shift_pressed ? '$' : '4'; break;
        case 0x2E: c = shift_pressed ? '%' : '5'; break;
        case 0x36: c = shift_pressed ? '^' : '6'; break;
        case 0x3D: c = shift_pressed ? '&' : '7'; break;
        case 0x3E: c = shift_pressed ? '*' : '8'; break;
        case 0x46: c = shift_pressed ? '(' : '9'; break;
        case 0x45: c = shift_pressed ? ')' : '0'; break;
        case 0x4E: c = shift_pressed ? '_' : '-'; break;
        case 0x55: c = shift_pressed ? '+' : '='; break;
        case 0x66: c = '\b'; break;
        case 0x0D: c = '\t'; break;
        case 0x5A: c = '\n'; break;
        case 0x15: c = shift_pressed ? 'Q' : 'q'; break;
        case 0x1D: c = shift_pressed ? 'W' : 'w'; break;
        case 0x24: c = shift_pressed ? 'E' : 'e'; break;
        case 0x2D: c = shift_pressed ? 'R' : 'r'; break;
        case 0x2C: c = shift_pressed ? 'T' : 't'; break;
        case 0x35: c = shift_pressed ? 'Y' : 'y'; break;
        case 0x3C: c = shift_pressed ? 'U' : 'u'; break;
        case 0x43: c = shift_pressed ? 'I' : 'i'; break;
        case 0x44: c = shift_pressed ? 'O' : 'o'; break;
        case 0x4D: c = shift_pressed ? 'P' : 'p'; break;
        case 0x54: c = shift_pressed ? '{' : '['; break;
        case 0x5B: c = shift_pressed ? '}' : ']'; break;
        case 0x1C: c = shift_pressed ? 'A' : 'a'; break;
        case 0x1B: c = shift_pressed ? 'S' : 's'; break;
        case 0x23: c = shift_pressed ? 'D' : 'd'; break;
        case 0x2B: c = shift_pressed ? 'F' : 'f'; break;
        case 0x34: c = shift_pressed ? 'G' : 'g'; break;
        case 0x33: c = shift_pressed ? 'H' : 'h'; break;
        case 0x3B: c = shift_pressed ? 'J' : 'j'; break;
        case 0x42: c = shift_pressed ? 'K' : 'k'; break;
        case 0x4B: c = shift_pressed ? 'L' : 'l'; break;
        case 0x4C: c = shift_pressed ? ':' : ';'; break;
        case 0x52: c = shift_pressed ? '"' : '\''; break;
        case 0x0E: c = shift_pressed ? '~' : '`'; break;
        case 0x5D: c = shift_pressed ? '|' : '\\'; break;
        case 0x1A: c = shift_pressed ? 'Z' : 'z'; break;
        case 0x22: c = shift_pressed ? 'X' : 'x'; break;
        case 0x21: c = shift_pressed ? 'C' : 'c'; break;
        case 0x2A: c = shift_pressed ? 'V' : 'v'; break;
        case 0x32: c = shift_pressed ? 'B' : 'b'; break;
        case 0x31: c = shift_pressed ? 'N' : 'n'; break;
        case 0x3A: c = shift_pressed ? 'M' : 'm'; break;
        case 0x41: c = shift_pressed ? '<' : ','; break;
        case 0x49: c = shift_pressed ? '>' : '.'; break;
        case 0x4A: c = shift_pressed ? '?' : '/'; break;
        case 0x29: c = ' '; break;
        default:   c = 0; break;
    }

    c = translate_set2_layout(scancode, c);
    return apply_caps(c);
}

static void update_modifier_make(unsigned char scancode, int extended) {
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }

    if (scancode == 0x1D || scancode == 0x14) {
        ctrl_pressed = 1;
        return;
    }

    if (scancode == 0x38 || scancode == 0x11) {
        if (extended) {
            altgr_pressed = 1;
            return;
        }

        alt_pressed = 1;
        return;
    }
}

static void update_modifier_break(unsigned char scancode, int extended) {
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 0;
        return;
    }

    if (scancode == 0x1D || scancode == 0x14) {
        ctrl_pressed = 0;
        return;
    }

    if (scancode == 0x38 || scancode == 0x11) {
        if (extended) {
            altgr_pressed = 0;
            return;
        }

        alt_pressed = 0;
        return;
    }
}

void keyboard_handle_interrupt(void) {
    unsigned char scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended_prefix = 1;
        return;
    }

    if (scancode == 0xF0) {
        scancode_set = 2;
        set2_break_prefix = 1;
        return;
    }

    if (set2_break_prefix) {
        if (scancode == 0x58) {
            set2_break_prefix = 0;
            extended_prefix = 0;
            return;
        }

        if (scancode == 0x77) {
            set2_break_prefix = 0;
            extended_prefix = 0;
            return;
        }

        update_modifier_break(scancode, extended_prefix);
        set2_break_prefix = 0;
        extended_prefix = 0;
        return;
    }

    if (scancode & 0x80) {
        scancode_set = 1;

        if ((unsigned char)(scancode & 0x7F) == 0x3A) {
            extended_prefix = 0;
            return;
        }

        update_modifier_break((unsigned char)(scancode & 0x7F), extended_prefix);
        extended_prefix = 0;
        return;
    }

    update_modifier_make(scancode, extended_prefix);

    if (!extended_prefix) {
        if ((scancode_set == 2 && scancode == 0x58) ||
            (scancode_set != 2 && scancode == 0x3A)) {
            capslock_enabled = !capslock_enabled;
            return;
        }
    }

    if (!extended_prefix) {
        if ((scancode_set == 2 && scancode == 0x77) ||
            (scancode_set != 2 && scancode == 0x45)) {
            numlock_enabled = !numlock_enabled;
            return;
        }
    }

    if (extended_prefix) {
        if (scancode == 0x35 || scancode == 0x4A) {
            queue_char('/');
        } else {
            queue_navigation_event(scancode);
        }

        extended_prefix = 0;
        return;
    }

    if (scancode_set == 2) {
        if (queue_function_event_set2(scancode)) {
            return;
        }

        if (queue_keypad_or_navigation_set2(scancode)) {
            return;
        }

        switch (scancode) {
            case 0x66:
                queue_event(KEY_EVENT_BACKSPACE, 0);
                return;
            case 0x0D:
                queue_event(KEY_EVENT_TAB, 0);
                return;
            case 0x5A:
                queue_event(KEY_EVENT_ENTER, 0);
                return;
            default:
                break;
        }

        {
            char c = map_set2(scancode);
            if (c != 0) {
                if (ctrl_pressed && !shift_pressed && (c == 'c' || c == 'C')) {
                    queue_event(KEY_EVENT_CTRL_C, 0);
                    return;
                }

                queue_char(c);
            }
        }

        return;
    }

    switch (scancode) {
        case 0x0E:
            queue_event(KEY_EVENT_BACKSPACE, 0);
            return;
        case 0x0F:
            queue_event(KEY_EVENT_TAB, 0);
            return;
        case 0x1C:
            queue_event(KEY_EVENT_ENTER, 0);
            return;
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
            if (queue_keypad_or_navigation_set1(scancode)) {
                return;
            }
            return;
        default:
            break;
    }

    if (queue_function_event_set1(scancode)) {
        return;
    }

    {
        char c = map_set1(scancode);
        if (c != 0) {
            if (ctrl_pressed && !shift_pressed && (c == 'c' || c == 'C')) {
                queue_event(KEY_EVENT_CTRL_C, 0);
                return;
            }

            queue_char(c);
        }
    }
}

int keyboard_poll_event(keyboard_event_t* event) {
    if (event == 0 || !event_is_available()) {
        return 0;
    }

    *event = event_queue[event_tail];
    event_tail = (event_tail + 1) % KEYBOARD_EVENT_QUEUE_SIZE;
    return 1;
}

void keyboard_set_layout(keyboard_layout_t layout) {
    if (layout != KEYBOARD_LAYOUT_US && layout != KEYBOARD_LAYOUT_ES) {
        return;
    }

    active_layout = layout;
}

keyboard_layout_t keyboard_get_layout(void) {
    return active_layout;
}

const char* keyboard_layout_name(keyboard_layout_t layout) {
    switch (layout) {
        case KEYBOARD_LAYOUT_ES:
            return "es";
        default:
            return "us";
    }
}

void keyboard_inject_event(keyboard_event_type_t type, char character, unsigned char modifiers) {
    int next_head = (event_head + 1) % KEYBOARD_EVENT_QUEUE_SIZE;
    if (next_head == event_tail) return;

    event_queue[event_head].type = type;
    event_queue[event_head].character = character;
    event_queue[event_head].modifiers = modifiers;
    event_head = next_head;
}

unsigned char keyboard_get_modifiers(void) {
    unsigned char m = 0;
    if (shift_pressed) m |= KEY_MOD_SHIFT;
    if (ctrl_pressed)  m |= KEY_MOD_CTRL;
    if (alt_pressed)   m |= KEY_MOD_ALT;
    return m;
}