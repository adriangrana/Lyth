#ifndef MOUSE_H
#define MOUSE_H

typedef struct {
    int delta_x;
    int delta_y;
    unsigned char buttons;
} mouse_event_t;

typedef struct {
    int enabled;
    int x;
    int y;
    unsigned char buttons;
    unsigned int packets_received;
    unsigned int packets_dropped;
    unsigned int invalid_packets;
    unsigned int overflow_packets;
    unsigned int resync_count;
    unsigned int init_failures;
} mouse_state_t;

void mouse_init(void);
int mouse_is_enabled(void);
void mouse_handle_interrupt(void);
int mouse_poll_event(mouse_event_t* event);
void mouse_get_state(mouse_state_t* state);

#endif
