#include "keyboard.h"
#include "task.h"

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

#define KEYBOARD_EVENT_QUEUE_SIZE 256

static keyboard_event_t event_queue[KEYBOARD_EVENT_QUEUE_SIZE];
static int event_head = 0;
static int event_tail = 0;

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int scancode_set = 1;
static int set2_break_prefix = 0;
static int extended_prefix = 0;

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char apply_caps(char c) {
    return c;
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
    if (alt_pressed) {
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
        default: c = 0; break;
    }

    return apply_caps(c);
}

static void update_modifier_make(unsigned char scancode) {
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }

    if (scancode == 0x1D || scancode == 0x14) {
        ctrl_pressed = 1;
        return;
    }

    if (scancode == 0x38 || scancode == 0x11) {
        alt_pressed = 1;
        return;
    }

}

static void update_modifier_break(unsigned char scancode) {
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 0;
        return;
    }

    if (scancode == 0x1D || scancode == 0x14) {
        ctrl_pressed = 0;
        return;
    }

    if (scancode == 0x38 || scancode == 0x11) {
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
        update_modifier_break(scancode);
        set2_break_prefix = 0;
        extended_prefix = 0;
        return;
    }

    if (scancode & 0x80) {
        scancode_set = 1;
        update_modifier_break((unsigned char)(scancode & 0x7F));
        extended_prefix = 0;
        return;
    }

    update_modifier_make(scancode);

    if (extended_prefix) {
        switch (scancode) {
            case 0x75:
            case 0x72:
            case 0x6B:
            case 0x74:
            case 0x52:
            case 0x70:
            case 0x48:
            case 0x50:
            case 0x4B:
            case 0x4D:
                queue_navigation_event(scancode);
                break;
            default:
                break;
        }

        extended_prefix = 0;
        return;
    }

    if (scancode_set == 2) {
        if (scancode == 0x75 || scancode == 0x72 || scancode == 0x6B || scancode == 0x74) {
            queue_navigation_event(scancode);
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
                    task_request_cancel();
                    if (!task_is_running()) {
                        queue_event(KEY_EVENT_CTRL_C, 0);
                    }
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
        case 0x48:
        case 0x50:
        case 0x4B:
        case 0x4D:
            queue_navigation_event(scancode);
            return;
        default:
            break;
    }

    {
        char c = map_set1(scancode);
        if (c != 0) {
            if (ctrl_pressed && !shift_pressed && (c == 'c' || c == 'C')) {
                task_request_cancel();

                if (!task_is_running()) {
                    queue_event(KEY_EVENT_CTRL_C, 0);
                }

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