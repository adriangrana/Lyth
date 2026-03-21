#ifndef INPUT_H
#define INPUT_H

#include "keyboard.h"

typedef enum {
    INPUT_DEVICE_NONE = 0,
    INPUT_DEVICE_KEYBOARD = 1,
    INPUT_DEVICE_MOUSE = 2
} input_device_type_t;

typedef enum {
    INPUT_EVENT_NONE = KEY_EVENT_NONE,
    INPUT_EVENT_CHAR = KEY_EVENT_CHAR,
    INPUT_EVENT_ENTER = KEY_EVENT_ENTER,
    INPUT_EVENT_BACKSPACE = KEY_EVENT_BACKSPACE,
    INPUT_EVENT_DELETE = KEY_EVENT_DELETE,
    INPUT_EVENT_TAB = KEY_EVENT_TAB,
    INPUT_EVENT_UP = KEY_EVENT_UP,
    INPUT_EVENT_DOWN = KEY_EVENT_DOWN,
    INPUT_EVENT_LEFT = KEY_EVENT_LEFT,
    INPUT_EVENT_RIGHT = KEY_EVENT_RIGHT,
    INPUT_EVENT_HOME = KEY_EVENT_HOME,
    INPUT_EVENT_END = KEY_EVENT_END,
    INPUT_EVENT_PAGE_UP = KEY_EVENT_PAGE_UP,
    INPUT_EVENT_PAGE_DOWN = KEY_EVENT_PAGE_DOWN,
    INPUT_EVENT_INSERT = KEY_EVENT_INSERT,
    INPUT_EVENT_CTRL_C = KEY_EVENT_CTRL_C
} input_event_type_t;

typedef struct {
    input_device_type_t device_type;
    input_event_type_t type;
    char character;
    unsigned char modifiers;
    int delta_x;
    int delta_y;
    unsigned char buttons;
} input_event_t;

int input_poll_event(input_event_t* event);

#endif
