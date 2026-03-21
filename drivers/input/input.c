#include "input.h"
#include "keyboard.h"
#include "mouse.h"

int input_poll_event(input_event_t* event) {
    keyboard_event_t keyboard_event;
    mouse_event_t mouse_event;

    if (event == 0) {
        return 0;
    }

    if (!keyboard_poll_event(&keyboard_event)) {
        if (!mouse_poll_event(&mouse_event)) {
            return 0;
        }

        event->device_type = INPUT_DEVICE_MOUSE;
        event->type = INPUT_EVENT_NONE;
        event->character = 0;
        event->modifiers = 0;
        event->delta_x = mouse_event.delta_x;
        event->delta_y = mouse_event.delta_y;
        event->buttons = mouse_event.buttons;
        return 1;
    }

    event->device_type = INPUT_DEVICE_KEYBOARD;
    event->type = (input_event_type_t)keyboard_event.type;
    event->character = keyboard_event.character;
    event->modifiers = keyboard_event.modifiers;
    event->delta_x = 0;
    event->delta_y = 0;
    event->buttons = 0;
    return 1;
}
