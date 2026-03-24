/*
 * Network Configuration app — shows network interfaces,
 * IP addresses, MAC, gateway, DNS, and link status.
 */

#include "netcfg.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "netif.h"

#define COL_NET_BG     0x1E1E2E
#define COL_NET_TEXT   0xCDD6F4
#define COL_NET_DIM    0x6C7086
#define COL_NET_UP     0xA6E3A1
#define COL_NET_DOWN   0xF38BA8
#define COL_NET_LABEL  0xA6ADC8

static gui_window_t* net_window;
static int net_open;

static void ip_to_str(uint32_t ip, char* buf, int bufsz) {
    /* ip is in network byte order (big-endian) */
    uint8_t* b = (uint8_t*)&ip;
    int pos = 0;
    int i;
    for (i = 0; i < 4; i++) {
        unsigned int v = b[i];
        if (v >= 100) { buf[pos++] = '0' + v / 100; v %= 100; buf[pos++] = '0' + v / 10; buf[pos++] = '0' + v % 10; }
        else if (v >= 10) { buf[pos++] = '0' + v / 10; buf[pos++] = '0' + v % 10; }
        else { buf[pos++] = '0' + v; }
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    (void)bufsz;
}

static void mac_to_str(const uint8_t mac[6], char* buf, int bufsz) {
    static const char hex[] = "0123456789ABCDEF";
    int i, pos = 0;
    for (i = 0; i < 6; i++) {
        buf[pos++] = hex[mac[i] >> 4];
        buf[pos++] = hex[mac[i] & 0xF];
        if (i < 5) buf[pos++] = ':';
    }
    buf[pos] = '\0';
    (void)bufsz;
}

static void net_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int ox = GUI_BORDER_WIDTH + 12;
    int oy = GUI_TITLEBAR_HEIGHT + 12;
    int row_h = FONT_PSF_HEIGHT + 6;
    int count, i;
    char buf[64];

    if (!s->pixels) return;

    gui_surface_clear(s, COL_NET_BG);

    /* title bar */
    gui_surface_fill(s, 0, 0, win->width, GUI_TITLEBAR_HEIGHT, 0x181825);
    {
        int cx = win->width - 20, cy = GUI_TITLEBAR_HEIGHT / 2, r;
        for (r = -5; r <= 5; r++) {
            int dx;
            for (dx = -5; dx <= 5; dx++) {
                if (r * r + dx * dx <= 25)
                    gui_surface_putpixel(s, cx + dx, cy + r, 0xF38BA8);
            }
        }
    }
    gui_surface_draw_string(s, 10, (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2,
                            win->title, 0xCDD6F4, 0, 0);
    gui_surface_hline(s, 0, GUI_TITLEBAR_HEIGHT - 1, win->width, 0x313244);

    count = netif_count();
    if (count == 0) {
        gui_surface_draw_string(s, ox, oy, "No network interfaces found.", COL_NET_DIM, 0, 0);
        return;
    }

    for (i = 0; i < count; i++) {
        netif_t* iface = netif_get(i);
        if (!iface) continue;

        /* interface header */
        gui_surface_draw_string(s, ox, oy, iface->name, 0x89B4FA, 0, 0);
        gui_surface_draw_string(s, ox + 80, oy, iface->up ? "UP" : "DOWN",
                                iface->up ? COL_NET_UP : COL_NET_DOWN, 0, 0);
        oy += row_h;

        /* MAC */
        mac_to_str(iface->mac, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 16, oy, "MAC:", COL_NET_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 80, oy, buf, COL_NET_TEXT, 0, 0);
        oy += row_h;

        /* IP */
        ip_to_str(iface->ip_addr, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 16, oy, "IP:", COL_NET_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 80, oy, buf, COL_NET_TEXT, 0, 0);
        oy += row_h;

        /* Netmask */
        ip_to_str(iface->netmask, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 16, oy, "Mask:", COL_NET_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 80, oy, buf, COL_NET_TEXT, 0, 0);
        oy += row_h;

        /* Gateway */
        ip_to_str(iface->gateway, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 16, oy, "GW:", COL_NET_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 80, oy, buf, COL_NET_TEXT, 0, 0);
        oy += row_h;

        /* DNS */
        ip_to_str(iface->dns_server, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 16, oy, "DNS:", COL_NET_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 80, oy, buf, COL_NET_TEXT, 0, 0);
        oy += row_h + 4;

        gui_surface_hline(s, ox, oy, win->width - ox * 2, 0x313244);
        oy += 8;
    }
}

static void net_on_key(gui_window_t* win, int event_type, char key) {
    (void)event_type;
    if (key == 'r' || key == 'R') {
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

static void net_on_close(gui_window_t* win) {
    net_open = 0;
    net_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

void netcfg_app_open(void) {
    if (net_open && net_window) {
        gui_window_focus(net_window);
        gui_dirty_add(net_window->x, net_window->y,
                      net_window->width, net_window->height);
        return;
    }

    net_window = gui_window_create("Network", 280, 100, 340, 320,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!net_window) return;

    net_window->on_paint = net_paint;
    net_window->on_key = net_on_key;
    net_window->on_close = net_on_close;

    net_open = 1;
    gui_window_focus(net_window);
    gui_dirty_add(net_window->x, net_window->y,
                  net_window->width, net_window->height);
}
