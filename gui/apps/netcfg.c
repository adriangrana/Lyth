/*
 * Network Configuration app — view and configure network interfaces.
 * Supports manual IP/Mask/GW/DNS entry and DHCP auto-configuration.
 *
 * Mode selection with radio buttons:
 *   ( ) DHCP  — auto-configure, fields read-only
 *   ( ) Manual — editable IP/Mask/GW/DNS fields
 *
 * Tab / Up / Down  — switch between fields (manual mode)
 * Left / Right     — move cursor within field
 * Backspace        — delete character
 * Enter            — apply configuration
 * R                — refresh display
 */

#include "netcfg.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "netif.h"
#include "dhcp.h"
#include "input.h"
#include "e1000.h"
#include "timer.h"

#define COL_NET_BG       THEME_COL_BASE
#define COL_NET_TEXT     THEME_COL_TEXT
#define COL_NET_DIM      THEME_COL_DIM
#define COL_NET_UP       THEME_COL_SUCCESS
#define COL_NET_DOWN     THEME_COL_ERROR
#define COL_NET_LABEL    THEME_COL_SUBTEXT0
#define COL_FIELD_BG     THEME_COL_SURFACE0
#define COL_FIELD_ACTIVE THEME_COL_SURFACE1
#define COL_FIELD_TEXT   THEME_COL_TEXT
#define COL_FIELD_RO     THEME_COL_DIM
#define COL_CURSOR       THEME_COL_CURSOR
#define COL_STATUS_OK    THEME_COL_SUCCESS
#define COL_STATUS_ERR   THEME_COL_ERROR
#define COL_RADIO_ON     THEME_COL_FOCUS
#define COL_RADIO_OFF    THEME_COL_SURFACE2
#define COL_RADIO_TEXT   THEME_COL_TEXT

#define FIELD_MAX        16  /* "255.255.255.255" + NUL */
#define NUM_FIELDS       4   /* IP, Mask, GW, DNS */
#define FIELD_W          140
#define FIELD_H          (FONT_PSF_HEIGHT + 6)

#define MODE_DHCP        0
#define MODE_MANUAL      1

/* Focus areas: 0 = DHCP radio, 1 = Manual radio, 2..5 = IP/Mask/GW/DNS fields */
#define FOCUS_DHCP       0
#define FOCUS_MANUAL     1
#define FOCUS_FIELD_FIRST 2
#define FOCUS_COUNT      (2 + NUM_FIELDS) /* 6 total */

static gui_window_t* net_window;
static int net_open;

/* Mode: DHCP or Manual */
static int net_mode;        /* MODE_DHCP or MODE_MANUAL */
static int focus_idx;       /* 0..5 focus area */

/* Editable fields */
static char field_buf[NUM_FIELDS][FIELD_MAX];
static int  field_cursor[NUM_FIELDS];
static int  field_len[NUM_FIELDS];

static const char* field_labels[NUM_FIELDS] = { "IP:", "Mask:", "GW:", "DNS:" };

/* Status message */
static char status_msg[48];
static int  status_color;

/* ---- helpers ---- */

static void nc_str_copy(char* dst, const char* src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void ip_to_str(uint32_t ip, char* buf, int bufsz) {
    uint8_t* b = (uint8_t*)&ip;
    int pos = 0, i;
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

static int str_to_ip(const char* s, uint32_t* out) {
    uint8_t octets[4];
    int i, val;
    for (i = 0; i < 4; i++) {
        val = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) return 0;
            s++;
        }
        octets[i] = (uint8_t)val;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s != '\0') return 0;
    uint8_t* dst = (uint8_t*)out;
    dst[0] = octets[0]; dst[1] = octets[1];
    dst[2] = octets[2]; dst[3] = octets[3];
    return 1;
}

static void load_fields_from_iface(netif_t* iface) {
    int i;
    ip_to_str(iface->ip_addr,    field_buf[0], FIELD_MAX);
    ip_to_str(iface->netmask,    field_buf[1], FIELD_MAX);
    ip_to_str(iface->gateway,    field_buf[2], FIELD_MAX);
    ip_to_str(iface->dns_server, field_buf[3], FIELD_MAX);
    for (i = 0; i < NUM_FIELDS; i++) {
        field_len[i] = strlen(field_buf[i]);
        field_cursor[i] = field_len[i];
    }
}

static void redraw(void) {
    if (net_window) {
        net_window->needs_redraw = 1;
        gui_dirty_add(net_window->x, net_window->y,
                      net_window->width, net_window->height);
    }
}

/* ---- DHCP auto-config ---- */

static void do_dhcp(void) {
    netif_t* iface = netif_get(0);
    if (!iface) {
        nc_str_copy(status_msg, "No interface");
        status_color = COL_STATUS_ERR;
        return;
    }
    nc_str_copy(status_msg, "Sending DHCP discover...");
    status_color = COL_NET_DIM;
    redraw();

    dhcp_discover(iface);

    /* Poll for DHCP reply (up to ~3 seconds) */
    {
        const dhcp_result_t* res = dhcp_get_result();
        unsigned int start = timer_get_uptime_ms();
        while (!res->ok && (timer_get_uptime_ms() - start) < 3000) {
            e1000_poll_rx();
        }
        if (res->ok) {
            nc_str_copy(status_msg, "DHCP configured OK.");
            status_color = COL_STATUS_OK;
            load_fields_from_iface(iface);
        } else {
            nc_str_copy(status_msg, "DHCP failed / timeout.");
            status_color = COL_STATUS_ERR;
        }
    }
}

/* ---- paint ---- */

static void draw_radio(gui_surface_t* s, int x, int y, int selected, int focused,
                       const char* label) {
    /* outer circle (7px radius) */
    int cx = x + 7, cy = y + FONT_PSF_HEIGHT / 2;
    int r;
    uint32_t ring_col = focused ? COL_CURSOR : COL_RADIO_OFF;
    for (r = -7; r <= 7; r++) {
        int dx;
        for (dx = -7; dx <= 7; dx++) {
            int d2 = r * r + dx * dx;
            if (d2 >= 36 && d2 <= 49)
                gui_surface_putpixel(s, cx + dx, cy + r, ring_col);
        }
    }
    /* filled inner circle if selected */
    if (selected) {
        for (r = -4; r <= 4; r++) {
            int dx;
            for (dx = -4; dx <= 4; dx++) {
                if (r * r + dx * dx <= 16)
                    gui_surface_putpixel(s, cx + dx, cy + r, COL_RADIO_ON);
            }
        }
    }
    /* label */
    gui_surface_draw_string(s, x + 18, y, label, COL_RADIO_TEXT, 0, 0);
}

static void net_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int ox = GUI_BORDER_WIDTH + 12;
    int oy = GUI_TITLEBAR_HEIGHT + 10;
    int row_h = FONT_PSF_HEIGHT + 6;
    int label_w = 48;
    int field_x = ox + label_w + 4;
    char buf[64];
    int i;
    int editing_fields = (net_mode == MODE_MANUAL);

    if (!s->pixels) return;

    gui_surface_clear(s, COL_NET_BG);

    /* Decorations */
    gui_window_draw_decorations(win);

    int count = netif_count();
    if (count == 0) {
        gui_surface_draw_string(s, ox, oy,
            "No network interfaces found.", COL_NET_DIM, 0, 0);
        return;
    }

    /* Show first interface info */
    netif_t* iface = netif_get(0);
    if (!iface) return;

    /* Interface name + status */
    gui_surface_draw_string(s, ox, oy, iface->name, 0x89B4FA, 0, 0);
    gui_surface_draw_string(s, ox + 80, oy, iface->up ? "UP" : "DOWN",
                            iface->up ? COL_NET_UP : COL_NET_DOWN, 0, 0);
    oy += row_h;

    /* MAC */
    mac_to_str(iface->mac, buf, sizeof(buf));
    gui_surface_draw_string(s, ox + 16, oy, "MAC:", COL_NET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 80, oy, buf, COL_NET_TEXT, 0, 0);
    oy += row_h + 4;

    gui_surface_hline(s, ox, oy, win->width - ox * 2, 0x313244);
    oy += 8;

    /* Radio buttons: DHCP / Manual */
    draw_radio(s, ox, oy, net_mode == MODE_DHCP,
               focus_idx == FOCUS_DHCP, "DHCP (automatic)");
    oy += row_h + 2;

    draw_radio(s, ox, oy, net_mode == MODE_MANUAL,
               focus_idx == FOCUS_MANUAL, "Manual");
    oy += row_h + 6;

    gui_surface_hline(s, ox, oy, win->width - ox * 2, 0x313244);
    oy += 8;

    /* IP fields */
    for (i = 0; i < NUM_FIELDS; i++) {
        int fy = oy + i * (FIELD_H + 4);
        int fi = FOCUS_FIELD_FIRST + i;
        int is_active = (focus_idx == fi && editing_fields);

        /* Label */
        gui_surface_draw_string(s, ox, fy + 3, field_labels[i],
                                COL_NET_LABEL, 0, 0);

        /* Field background */
        uint32_t fbg = is_active ? COL_FIELD_ACTIVE : COL_FIELD_BG;
        gui_surface_fill(s, field_x, fy, FIELD_W, FIELD_H, fbg);

        /* Field text */
        uint32_t ftxt = editing_fields ? COL_FIELD_TEXT : COL_FIELD_RO;
        gui_surface_draw_string(s, field_x + 4, fy + 3,
                                field_buf[i], ftxt, 0, 0);

        /* Cursor (only in manual mode, active field) */
        if (is_active) {
            int cx = field_x + 4 + field_cursor[i] * FONT_PSF_WIDTH;
            gui_surface_fill(s, cx, fy + 2, 2, FONT_PSF_HEIGHT, COL_CURSOR);
            gui_surface_fill(s, field_x - 3, fy, 2, FIELD_H, COL_CURSOR);
        }
    }

    oy += NUM_FIELDS * (FIELD_H + 4) + 8;

    /* Hints */
    if (editing_fields) {
        gui_surface_draw_string(s, ox, oy,
            "[Enter] Apply  [Tab] Next field  [R] Refresh",
            COL_NET_DIM, 0, 0);
    } else {
        gui_surface_draw_string(s, ox, oy,
            "[R] Refresh",
            COL_NET_DIM, 0, 0);
    }
    oy += row_h + 4;

    /* Status message */
    if (status_msg[0]) {
        gui_surface_draw_string(s, ox, oy, status_msg, status_color, 0, 0);
    }
}

/* ---- key handling ---- */

static void apply_config(void) {
    netif_t* iface = netif_get(0);
    if (!iface) {
        nc_str_copy(status_msg, "No interface");
        status_color = COL_STATUS_ERR;
        return;
    }

    uint32_t ip, mask, gw, dns;
    if (!str_to_ip(field_buf[0], &ip))   { nc_str_copy(status_msg, "Invalid IP address");  status_color = COL_STATUS_ERR; return; }
    if (!str_to_ip(field_buf[1], &mask))  { nc_str_copy(status_msg, "Invalid netmask");     status_color = COL_STATUS_ERR; return; }
    if (!str_to_ip(field_buf[2], &gw))    { nc_str_copy(status_msg, "Invalid gateway");     status_color = COL_STATUS_ERR; return; }
    if (!str_to_ip(field_buf[3], &dns))   { nc_str_copy(status_msg, "Invalid DNS address"); status_color = COL_STATUS_ERR; return; }

    netif_set_addr(iface, ip, mask, gw);
    iface->dns_server = dns;

    nc_str_copy(status_msg, "Configuration applied.");
    status_color = COL_STATUS_OK;
}

static void field_insert_char(int fi, char ch) {
    if (field_len[fi] >= FIELD_MAX - 1) return;
    if (!((ch >= '0' && ch <= '9') || ch == '.')) return;

    int pos = field_cursor[fi];
    int j;
    for (j = field_len[fi]; j > pos; j--)
        field_buf[fi][j] = field_buf[fi][j - 1];
    field_buf[fi][pos] = ch;
    field_len[fi]++;
    field_buf[fi][field_len[fi]] = '\0';
    field_cursor[fi]++;
}

static void field_backspace(int fi) {
    if (field_cursor[fi] <= 0) return;
    int pos = field_cursor[fi];
    int j;
    for (j = pos - 1; j < field_len[fi] - 1; j++)
        field_buf[fi][j] = field_buf[fi][j + 1];
    field_len[fi]--;
    field_buf[fi][field_len[fi]] = '\0';
    field_cursor[fi]--;
}

static void field_delete(int fi) {
    int pos = field_cursor[fi];
    if (pos >= field_len[fi]) return;
    int j;
    for (j = pos; j < field_len[fi] - 1; j++)
        field_buf[fi][j] = field_buf[fi][j + 1];
    field_len[fi]--;
    field_buf[fi][field_len[fi]] = '\0';
}

static void switch_to_dhcp(void) {
    net_mode = MODE_DHCP;
    focus_idx = FOCUS_DHCP;
    do_dhcp();
    redraw();
}

static void switch_to_manual(void) {
    net_mode = MODE_MANUAL;
    focus_idx = FOCUS_FIELD_FIRST;
    status_msg[0] = '\0';
    redraw();
}

static void net_on_key(gui_window_t* win, int event_type, char key) {
    (void)win;

    /* Tab / Down: move focus forward */
    if (event_type == INPUT_EVENT_TAB || event_type == INPUT_EVENT_DOWN) {
        focus_idx++;
        if (net_mode == MODE_DHCP) {
            /* In DHCP mode, skip the IP fields; wrap from Manual back to DHCP */
            if (focus_idx > FOCUS_MANUAL) focus_idx = FOCUS_DHCP;
        } else {
            if (focus_idx >= FOCUS_COUNT) focus_idx = FOCUS_DHCP;
        }
        redraw(); return;
    }
    /* Up: move focus backward */
    if (event_type == INPUT_EVENT_UP) {
        focus_idx--;
        if (focus_idx < FOCUS_DHCP) {
            if (net_mode == MODE_DHCP) focus_idx = FOCUS_MANUAL;
            else focus_idx = FOCUS_COUNT - 1;
        }
        redraw(); return;
    }

    /* Enter / Space on radio buttons: select mode */
    if ((event_type == INPUT_EVENT_ENTER || (event_type == INPUT_EVENT_CHAR && key == ' ')) &&
        focus_idx <= FOCUS_MANUAL) {
        if (focus_idx == FOCUS_DHCP && net_mode != MODE_DHCP) {
            switch_to_dhcp();
        } else if (focus_idx == FOCUS_MANUAL && net_mode != MODE_MANUAL) {
            switch_to_manual();
        }
        return;
    }

    /* Field editing (only in manual mode) */
    if (net_mode == MODE_MANUAL && focus_idx >= FOCUS_FIELD_FIRST) {
        int fi = focus_idx - FOCUS_FIELD_FIRST;

        if (event_type == INPUT_EVENT_LEFT) {
            if (field_cursor[fi] > 0) field_cursor[fi]--;
            redraw(); return;
        }
        if (event_type == INPUT_EVENT_RIGHT) {
            if (field_cursor[fi] < field_len[fi]) field_cursor[fi]++;
            redraw(); return;
        }
        if (event_type == INPUT_EVENT_HOME) {
            field_cursor[fi] = 0; redraw(); return;
        }
        if (event_type == INPUT_EVENT_END) {
            field_cursor[fi] = field_len[fi]; redraw(); return;
        }
        if (event_type == INPUT_EVENT_BACKSPACE) {
            field_backspace(fi); redraw(); return;
        }
        if (event_type == INPUT_EVENT_DELETE) {
            field_delete(fi); redraw(); return;
        }
        if (event_type == INPUT_EVENT_CHAR && key) {
            field_insert_char(fi, key);
            redraw(); return;
        }
        if (event_type == INPUT_EVENT_ENTER) {
            apply_config();
            redraw(); return;
        }
    }

    /* R to refresh */
    if (event_type == INPUT_EVENT_CHAR && (key == 'r' || key == 'R')) {
        netif_t* iface = netif_get(0);
        if (iface) load_fields_from_iface(iface);
        status_msg[0] = '\0';
        redraw();
        return;
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

    net_window = gui_window_create("Network", 280, 80, 360, 400,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!net_window) return;

    /* Initialize: default to DHCP, auto-discover */
    net_mode = MODE_DHCP;
    focus_idx = FOCUS_DHCP;
    status_msg[0] = '\0';
    {
        netif_t* iface = netif_get(0);
        if (iface) {
            load_fields_from_iface(iface);
        } else {
            int i;
            for (i = 0; i < NUM_FIELDS; i++) {
                nc_str_copy(field_buf[i], "0.0.0.0");
                field_len[i] = 7;
                field_cursor[i] = 7;
            }
        }
    }

    net_window->on_paint = net_paint;
    net_window->on_key = net_on_key;
    net_window->on_close = net_on_close;

    net_open = 1;
    gui_window_focus(net_window);
    gui_dirty_add(net_window->x, net_window->y,
                  net_window->width, net_window->height);

    /* Auto-discover via DHCP on open */
    do_dhcp();
}
