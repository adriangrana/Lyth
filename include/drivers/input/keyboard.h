#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KEY_MOD_SHIFT 0x01
#define KEY_MOD_CTRL  0x02
#define KEY_MOD_ALT   0x04

typedef enum {
	KEYBOARD_LAYOUT_US = 0,
	KEYBOARD_LAYOUT_ES = 1
} keyboard_layout_t;

typedef enum {
	KEY_EVENT_NONE = 0,
	KEY_EVENT_CHAR,
	KEY_EVENT_ENTER,
	KEY_EVENT_BACKSPACE,
	KEY_EVENT_DELETE,
	KEY_EVENT_TAB,
	KEY_EVENT_UP,
	KEY_EVENT_DOWN,
	KEY_EVENT_LEFT,
	KEY_EVENT_RIGHT,
	KEY_EVENT_HOME,
	KEY_EVENT_END,
	KEY_EVENT_PAGE_UP,
	KEY_EVENT_PAGE_DOWN,
	KEY_EVENT_INSERT,
	KEY_EVENT_F1,
	KEY_EVENT_F2,
	KEY_EVENT_F3,
	KEY_EVENT_F4,
	KEY_EVENT_CTRL_C,
	KEY_EVENT_SUPER
} keyboard_event_type_t;

typedef struct {
	keyboard_event_type_t type;
	char character;
	unsigned char modifiers;
} keyboard_event_t;

void keyboard_init(void);
void keyboard_handle_interrupt(void);
int keyboard_poll_event(keyboard_event_t* event);
void keyboard_set_layout(keyboard_layout_t layout);
keyboard_layout_t keyboard_get_layout(void);
const char* keyboard_layout_name(keyboard_layout_t layout);

/* Inject an event from external sources (e.g. USB HID keyboard) */
void keyboard_inject_event(keyboard_event_type_t type, char character, unsigned char modifiers);

#endif