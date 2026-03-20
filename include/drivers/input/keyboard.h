#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KEY_MOD_SHIFT 0x01
#define KEY_MOD_CTRL  0x02
#define KEY_MOD_ALT   0x04

typedef enum {
	KEY_EVENT_NONE = 0,
	KEY_EVENT_CHAR,
	KEY_EVENT_ENTER,
	KEY_EVENT_BACKSPACE,
	KEY_EVENT_TAB,
	KEY_EVENT_UP,
	KEY_EVENT_DOWN,
	KEY_EVENT_LEFT,
	KEY_EVENT_RIGHT,
	KEY_EVENT_INSERT,
	KEY_EVENT_CTRL_C
} keyboard_event_type_t;

typedef struct {
	keyboard_event_type_t type;
	char character;
	unsigned char modifiers;
} keyboard_event_t;

void keyboard_handle_interrupt(void);
int keyboard_poll_event(keyboard_event_t* event);

#endif