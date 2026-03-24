#include "mouse.h"
#include "klog.h"

#define MOUSE_QUEUE_SIZE 64
#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04
#define MOUSE_STATUS_OUTPUT_FULL 0x01
#define MOUSE_STATUS_INPUT_FULL  0x02
#define MOUSE_STATUS_AUX_DATA    0x20
#define MOUSE_PACKET_SYNC_BIT    0x08
#define MOUSE_PACKET_X_SIGN      0x10
#define MOUSE_PACKET_Y_SIGN      0x20
#define MOUSE_PACKET_X_OVERFLOW  0x40
#define MOUSE_PACKET_Y_OVERFLOW  0x80

static mouse_event_t event_queue[MOUSE_QUEUE_SIZE];
static int event_head = 0;
static int event_tail = 0;
static int mouse_enabled = 0;
static unsigned char packet[3];
static int packet_index = 0;
static mouse_state_t mouse_state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void mouse_reset_runtime_state(void) {
    event_head = 0;
    event_tail = 0;
    packet_index = 0;
    mouse_enabled = 0;
    mouse_state.enabled = 0;
    mouse_state.x = 0;
    mouse_state.y = 0;
    mouse_state.buttons = 0;
    mouse_state.packets_received = 0;
    mouse_state.packets_dropped = 0;
    mouse_state.invalid_packets = 0;
    mouse_state.overflow_packets = 0;
    mouse_state.resync_count = 0;
    mouse_state.init_failures = 0;
}

static int mouse_wait_input_ready(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if ((inb(0x64) & MOUSE_STATUS_INPUT_FULL) == 0) {
            return 1;
        }
    }

    return 0;
}

static int mouse_wait_output_ready(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (inb(0x64) & MOUSE_STATUS_OUTPUT_FULL) {
            return 1;
        }
    }

    return 0;
}

static void mouse_write(unsigned char value) {
    if (!mouse_wait_input_ready()) {
        return;
    }

    outb(0x64, 0xD4);
    if (!mouse_wait_input_ready()) {
        return;
    }

    outb(0x60, value);
}

static unsigned char mouse_read(void) {
    if (!mouse_wait_output_ready()) {
        return 0;
    }

    return inb(0x60);
}

static void mouse_flush_output_buffer(void) {
    for (int timeout = 0; timeout < 32; timeout++) {
        unsigned char status = inb(0x64);

        if ((status & MOUSE_STATUS_OUTPUT_FULL) == 0) {
            return;
        }

        (void)inb(0x60);
        io_wait();
    }
}

static int mouse_expect_ack(unsigned char command, const char* failure_message) {
    unsigned char ack;

    mouse_write(command);
    ack = mouse_read();
    if (ack == 0xFA) {
        return 1;
    }

    mouse_state.init_failures++;
    klog_write(KLOG_LEVEL_WARN, "mouse", failure_message);
    return 0;
}

static void queue_mouse_event(int delta_x, int delta_y, unsigned char buttons) {
    int next_head = (event_head + 1) % MOUSE_QUEUE_SIZE;

    if (next_head == event_tail) {
        mouse_state.packets_dropped++;
        return;
    }

    event_queue[event_head].delta_x = delta_x;
    event_queue[event_head].delta_y = delta_y;
    event_queue[event_head].buttons = buttons;
    event_head = next_head;
}

void mouse_init(void) {
    unsigned char status;

    mouse_reset_runtime_state();
    mouse_flush_output_buffer();

    if (!mouse_wait_input_ready()) {
        mouse_state.init_failures++;
        klog_write(KLOG_LEVEL_WARN, "mouse", "Timeout esperando controlador PS/2");
        return;
    }

    outb(0x64, 0xA8);
    io_wait();

    if (!mouse_wait_input_ready()) {
        return;
    }

    outb(0x64, 0x20);
    if (!mouse_wait_output_ready()) {
        mouse_state.init_failures++;
        klog_write(KLOG_LEVEL_WARN, "mouse", "No se pudo leer config PS/2");
        return;
    }

    status = inb(0x60);
    status |= 0x02;
    status &= (unsigned char)~0x20;

    if (!mouse_wait_input_ready()) {
        mouse_state.init_failures++;
        klog_write(KLOG_LEVEL_WARN, "mouse", "Timeout escribiendo config PS/2");
        return;
    }

    outb(0x64, 0x60);
    if (!mouse_wait_input_ready()) {
        mouse_state.init_failures++;
        klog_write(KLOG_LEVEL_WARN, "mouse", "Timeout aplicando config PS/2");
        return;
    }

    outb(0x60, status);
    io_wait();
    mouse_flush_output_buffer();

    if (!mouse_expect_ack(0xF6, "El raton no acepto valores por defecto")) {
        return;
    }

    if (!mouse_expect_ack(0xF4, "El raton no entro en streaming")) {
        return;
    }

    mouse_enabled = 1;
    mouse_state.enabled = 1;
    klog_write(KLOG_LEVEL_INFO, "mouse", "PS/2 listo");
}

int mouse_is_enabled(void) {
    return mouse_enabled;
}

void mouse_handle_interrupt(void) {
    int delta_x;
    int delta_y;
    unsigned char status;
    unsigned char data;

    if (!mouse_enabled) {
        if (inb(0x64) & MOUSE_STATUS_OUTPUT_FULL) {
            (void)inb(0x60);
        }
        return;
    }

    status = inb(0x64);
    if ((status & MOUSE_STATUS_OUTPUT_FULL) == 0 || (status & MOUSE_STATUS_AUX_DATA) == 0) {
        return;
    }

    data = inb(0x60);

    if (packet_index == 0 && (data & MOUSE_PACKET_SYNC_BIT) == 0) {
        mouse_state.invalid_packets++;
        mouse_state.resync_count++;
        return;
    }

    packet[packet_index++] = data;
    if (packet_index < 3) {
        return;
    }

    packet_index = 0;
    if ((packet[0] & MOUSE_PACKET_SYNC_BIT) == 0) {
        mouse_state.invalid_packets++;
        mouse_state.resync_count++;
        return;
    }

    if ((packet[0] & (MOUSE_PACKET_X_OVERFLOW | MOUSE_PACKET_Y_OVERFLOW)) != 0) {
        mouse_state.overflow_packets++;
        return;
    }

    delta_x = (packet[0] & MOUSE_PACKET_X_SIGN) ? (int)packet[1] - 256 : (int)packet[1];
    delta_y = (packet[0] & MOUSE_PACKET_Y_SIGN) ? (int)packet[2] - 256 : (int)packet[2];
    delta_y = -delta_y;

    mouse_state.x += delta_x;
    mouse_state.y += delta_y;
    mouse_state.buttons = packet[0] & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT | MOUSE_BUTTON_MIDDLE);
    mouse_state.packets_received++;

    queue_mouse_event(delta_x, delta_y, mouse_state.buttons);
}

int mouse_poll_event(mouse_event_t* event) {
    if (event == 0 || event_head == event_tail) {
        return 0;
    }

    *event = event_queue[event_tail];
    event_tail = (event_tail + 1) % MOUSE_QUEUE_SIZE;
    return 1;
}

void mouse_get_state(mouse_state_t* state) {
    if (state == 0) {
        return;
    }

    *state = mouse_state;
}

void mouse_inject_event(int dx, int dy, unsigned char buttons) {
    mouse_state.x += dx;
    mouse_state.y += dy;
    mouse_state.buttons = buttons;
    mouse_state.packets_received++;
    queue_mouse_event(dx, dy, buttons);
}
