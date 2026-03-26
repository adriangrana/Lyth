/*
 * WiFi Configuration app — scan, select, and connect to wireless networks.
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │  WiFi: On/Off        [Scan]         │
 *   │ ─────────────────────────────────── │
 *   │  ▸ LythNet         ████░  WPA2     │
 *   │    Vecino-5G        ███░░  WPA2     │
 *   │    CafeWiFi         █░░░░  Open     │
 *   │    RedPrivada       ██░░░  WPA      │
 *   │ ─────────────────────────────────── │
 *   │  Password: [______________]         │
 *   │  [Connect]  [Disconnect]            │
 *   │  Status: Connected to LythNet       │
 *   └─────────────────────────────────────┘
 *
 * Up/Down      — navigate network list
 * Enter        — select network / connect
 * Tab          — switch focus (list → password → buttons)
 * R / F5       — rescan
 * D            — disconnect
 */

#include "wificfg.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "wifi.h"
#include "input.h"

/* ── colours ─────────────────────────────────────────────────────── */
#define COL_BG           THEME_COL_BASE
#define COL_TEXT         THEME_COL_TEXT
#define COL_DIM          THEME_COL_DIM
#define COL_LABEL        THEME_COL_SUBTEXT0
#define COL_ACCENT       THEME_COL_ACCENT
#define COL_UP           THEME_COL_SUCCESS
#define COL_DOWN         THEME_COL_ERROR
#define COL_SEL_BG       THEME_COL_SURFACE1
#define COL_FIELD_BG     THEME_COL_SURFACE0
#define COL_FIELD_ACTIVE THEME_COL_SURFACE1
#define COL_FIELD_TEXT   THEME_COL_TEXT
#define COL_CURSOR       THEME_COL_CURSOR
#define COL_BORDER       THEME_COL_BORDER
#define COL_BTN_BG       THEME_COL_SURFACE0
#define COL_BTN_ACTIVE   THEME_COL_FOCUS
#define COL_STATUS_OK    THEME_COL_SUCCESS
#define COL_STATUS_ERR   THEME_COL_ERROR
#define COL_SIGNAL_HI    THEME_COL_SUCCESS
#define COL_SIGNAL_MED   THEME_COL_WARNING
#define COL_SIGNAL_LO    THEME_COL_ERROR

/* ── layout ──────────────────────────────────────────────────────── */
#define WIN_W          380
#define WIN_H          420
#define MAX_VISIBLE    6   /* max visible SSIDs in list area */

#define PASS_MAX       64

/* focus areas */
#define FOCUS_LIST     0
#define FOCUS_PASS     1
#define FOCUS_CONNECT  2
#define FOCUS_DISCON   3
#define FOCUS_COUNT    4

/* ── state ────────────────────────────────────────────────────────── */
static gui_window_t* wf_win;
static int wf_open;

static int focus_area;    /* FOCUS_* */
static int list_sel;      /* selected index in scan list */
static int list_scroll;   /* scroll offset */

/* password field */
static char pass_buf[PASS_MAX];
static int  pass_len;
static int  pass_cursor;

/* status */
static char status_msg[48];
static int  status_color;

/* ── helpers ─────────────────────────────────────────────────────── */

static void wf_str_copy(char* dst, const char* src) {
	while (*src) *dst++ = *src++;
	*dst = '\0';
}

static void wf_redraw(void) {
	if (wf_win) {
		wf_win->needs_redraw = 1;
		gui_dirty_add(wf_win->x, wf_win->y,
		              wf_win->width, wf_win->height);
	}
}

/* security label string */
static const char* sec_str(int sec) {
	switch (sec) {
		case WIFI_SEC_WPA2: return "WPA2";
		case WIFI_SEC_WPA:  return "WPA";
		case WIFI_SEC_WEP:  return "WEP";
		default:            return "Open";
	}
}

/* signal to bars (0-4) */
static int signal_bars(int dbm) {
	if (dbm >= -40) return 4;
	if (dbm >= -55) return 3;
	if (dbm >= -70) return 2;
	if (dbm >= -85) return 1;
	return 0;
}

/* signal bar colour */
static uint32_t signal_color(int bars) {
	if (bars >= 3) return COL_SIGNAL_HI;
	if (bars >= 2) return COL_SIGNAL_MED;
	return COL_SIGNAL_LO;
}

/* draw signal strength bars */
static void draw_signal(gui_surface_t* s, int x, int y, int bars) {
	uint32_t col = signal_color(bars);
	int bar_w = 3, gap = 2;
	int i;
	for (i = 0; i < 4; i++) {
		int bh = 4 + i * 3;  /* 4, 7, 10, 13 */
		int bx = x + i * (bar_w + gap);
		int by = y + 13 - bh;
		uint32_t c = (i < bars) ? col : THEME_COL_SURFACE2;
		gui_surface_fill(s, bx, by, bar_w, bh, c);
	}
}

/* ── paint ────────────────────────────────────────────────────────── */

static void wf_paint(gui_window_t* win) {
	gui_surface_t* s = &win->surface;
	int ox = GUI_BORDER_WIDTH + 12;
	int oy = GUI_TITLEBAR_HEIGHT + 10;
	int row_h = THEME_LH_NORMAL;
	int i;

	if (!s->pixels) return;

	gui_surface_clear(s, COL_BG);
	gui_window_draw_decorations(win);

	int state = wifi_get_state();

	/* ── header: WiFi status + Scan hint ── */
	gui_surface_draw_string(s, ox, oy, "WiFi", COL_ACCENT, 0, 0);

	if (state == WIFI_STATE_OFF) {
		gui_surface_draw_string(s, ox + 5 * FONT_PSF_WIDTH, oy,
		    "  Disabled", COL_DOWN, 0, 0);
		oy += row_h;
		gui_surface_draw_string(s, ox, oy,
		    "No wireless adapter found.", COL_DIM, 0, 0);
		return;
	}

	{
		const char* sl;
		uint32_t sc;
		switch (state) {
			case WIFI_STATE_IDLE:       sl = "  Idle";        sc = COL_DIM;    break;
			case WIFI_STATE_SCANNING:   sl = "  Scanning..."; sc = COL_LABEL;  break;
			case WIFI_STATE_CONNECTING: sl = "  Connecting";  sc = COL_LABEL;  break;
			case WIFI_STATE_CONNECTED:  sl = "  Connected";   sc = COL_UP;     break;
			default:                    sl = "  Unknown";     sc = COL_DIM;    break;
		}
		gui_surface_draw_string(s, ox + 4 * FONT_PSF_WIDTH, oy, sl, sc, 0, 0);
	}

	/* Scan hint */
	gui_surface_draw_string(s, win->width - ox - 10 * FONT_PSF_WIDTH, oy,
	    "[R] Scan", COL_DIM, 0, 0);
	oy += row_h + 2;

	gui_surface_hline(s, ox, oy, win->width - ox * 2, COL_BORDER);
	oy += 6;

	/* ── network list ── */
	int count = wifi_scan_count();
	const wifi_network_t* nets = wifi_scan_results();
	const wifi_network_t* cur = wifi_current_network();

	if (count == 0) {
		gui_surface_draw_string(s, ox, oy,
		    "No networks found. Press R to scan.", COL_DIM, 0, 0);
		oy += row_h * MAX_VISIBLE;
	} else {
		int vis_count = count < MAX_VISIBLE ? count : MAX_VISIBLE;
		/* clamp scroll */
		if (list_scroll > count - vis_count) list_scroll = count - vis_count;
		if (list_scroll < 0) list_scroll = 0;

		for (i = 0; i < vis_count; i++) {
			int ni = list_scroll + i;
			if (ni >= count) break;

			const wifi_network_t* n = &nets[ni];
			int ry = oy + i * (row_h + 2);
			int is_sel = (focus_area == FOCUS_LIST && ni == list_sel);
			int is_connected = (cur && cur->bssid[0] == n->bssid[0] &&
			                    cur->bssid[5] == n->bssid[5]);

			/* selection highlight */
			if (is_sel) {
				gui_surface_fill(s, ox - 4, ry, win->width - 2 * ox + 8,
				                 row_h, COL_SEL_BG);
			}

			/* connected indicator */
			if (is_connected) {
				gui_surface_draw_string(s, ox, ry + 2, "*", COL_UP, 0, 0);
			}

			/* SSID */
			gui_surface_draw_string(s, ox + 12, ry + 2, n->ssid,
			    is_sel ? COL_TEXT : COL_LABEL, 0, 0);

			/* signal bars */
			int bars = signal_bars(n->signal);
			draw_signal(s, win->width - ox - 70, ry + 3, bars);

			/* security type */
			gui_surface_draw_string(s, win->width - ox - 36, ry + 2,
			    sec_str(n->security), COL_DIM, 0, 0);
		}
		oy += vis_count * (row_h + 2);
	}

	oy += 4;
	gui_surface_hline(s, ox, oy, win->width - ox * 2, COL_BORDER);
	oy += 8;

	/* ── password field ── */
	gui_surface_draw_string(s, ox, oy + 3, "Password:", COL_LABEL, 0, 0);
	{
		int fx = ox + 10 * FONT_PSF_WIDTH;
		int fw = win->width - fx - ox;
		int fh = THEME_LH_NORMAL;
		int active = (focus_area == FOCUS_PASS);
		uint32_t fbg = active ? COL_FIELD_ACTIVE : COL_FIELD_BG;

		gui_surface_fill(s, fx, oy, fw, fh, fbg);

		/* draw asterisks for password */
		{
			int px = fx + 4;
			int pi;
			for (pi = 0; pi < pass_len && pi < (fw / FONT_PSF_WIDTH - 1); pi++) {
				gui_surface_draw_string(s, px + pi * FONT_PSF_WIDTH,
				    oy + 3, "*", COL_FIELD_TEXT, 0, 0);
			}
		}

		/* cursor */
		if (active) {
			int cx = fx + 4 + pass_cursor * FONT_PSF_WIDTH;
			gui_surface_fill(s, cx, oy + 2, 2, FONT_PSF_HEIGHT, COL_CURSOR);
			gui_surface_fill(s, fx - 3, oy, 2, fh, COL_CURSOR);
		}
	}
	oy += THEME_LH_NORMAL + 10;

	/* ── buttons: Connect / Disconnect ── */
	{
		int btn_w = 90;
		int btn_h = 24;
		int bx1 = ox;
		int bx2 = ox + btn_w + 12;

		/* Connect button */
		uint32_t btn1_bg = (focus_area == FOCUS_CONNECT) ? COL_BTN_ACTIVE : COL_BTN_BG;
		gui_surface_fill(s, bx1, oy, btn_w, btn_h, btn1_bg);
		gui_surface_draw_string(s, bx1 + (btn_w - 7 * FONT_PSF_WIDTH) / 2,
		    oy + (btn_h - FONT_PSF_HEIGHT) / 2, "Connect", COL_TEXT, 0, 0);

		/* Disconnect button */
		uint32_t btn2_bg = (focus_area == FOCUS_DISCON) ? COL_DOWN : COL_BTN_BG;
		gui_surface_fill(s, bx2, oy, btn_w + 12, btn_h, btn2_bg);
		gui_surface_draw_string(s, bx2 + (btn_w + 12 - 10 * FONT_PSF_WIDTH) / 2,
		    oy + (btn_h - FONT_PSF_HEIGHT) / 2, "Disconnect", COL_TEXT, 0, 0);
	}
	oy += 24 + 10;

	/* ── hints ── */
	gui_surface_draw_string(s, ox, oy,
	    "[Tab] Focus  [Enter] Select  [D] Disconnect",
	    COL_DIM, 0, 0);
	oy += row_h;

	/* ── status ── */
	if (status_msg[0]) {
		gui_surface_draw_string(s, ox, oy, status_msg, status_color, 0, 0);
	}
}

/* ── actions ─────────────────────────────────────────────────────── */

static void do_scan(void) {
	wf_str_copy(status_msg, "Scanning...");
	status_color = COL_DIM;
	wf_redraw();

	int n = wifi_scan();
	if (n < 0) {
		wf_str_copy(status_msg, "Scan failed.");
		status_color = COL_STATUS_ERR;
	} else {
		status_msg[0] = '\0';
	}
	list_sel = 0;
	list_scroll = 0;
	wf_redraw();
}

static void do_connect(void) {
	int count = wifi_scan_count();
	if (list_sel < 0 || list_sel >= count) {
		wf_str_copy(status_msg, "Select a network first");
		status_color = COL_STATUS_ERR;
		wf_redraw();
		return;
	}

	const wifi_network_t* nets = wifi_scan_results();
	const wifi_network_t* n = &nets[list_sel];

	wf_str_copy(status_msg, "Connecting...");
	status_color = COL_DIM;
	wf_redraw();

	int ret = wifi_connect(n->ssid, pass_buf);
	if (ret == 0) {
		wf_str_copy(status_msg, "Connected!");
		status_color = COL_STATUS_OK;
	} else {
		wf_str_copy(status_msg, "Connection failed.");
		status_color = COL_STATUS_ERR;
	}
	wf_redraw();
}

static void do_disconnect(void) {
	wifi_disconnect();
	wf_str_copy(status_msg, "Disconnected.");
	status_color = COL_DIM;
	wf_redraw();
}

/* ── key handler ─────────────────────────────────────────────────── */

static void wf_on_key(gui_window_t* win, int event_type, char key) {
	(void)win;
	int count = wifi_scan_count();

	/* Tab: cycle focus */
	if (event_type == INPUT_EVENT_TAB) {
		focus_area = (focus_area + 1) % FOCUS_COUNT;
		wf_redraw();
		return;
	}

	/* R / r: rescan */
	if (event_type == INPUT_EVENT_CHAR && (key == 'r' || key == 'R')) {
		if (focus_area != FOCUS_PASS) {
			do_scan();
			return;
		}
		/* fall through to field input if in password */
	}

	/* D / d: disconnect (if not in password field) */
	if (event_type == INPUT_EVENT_CHAR && (key == 'd' || key == 'D')) {
		if (focus_area != FOCUS_PASS) {
			do_disconnect();
			return;
		}
	}

	/* Network list navigation */
	if (focus_area == FOCUS_LIST) {
		if (event_type == INPUT_EVENT_UP) {
			if (list_sel > 0) list_sel--;
			if (list_sel < list_scroll) list_scroll = list_sel;
			wf_redraw();
			return;
		}
		if (event_type == INPUT_EVENT_DOWN) {
			if (list_sel < count - 1) list_sel++;
			if (list_sel >= list_scroll + MAX_VISIBLE)
				list_scroll = list_sel - MAX_VISIBLE + 1;
			wf_redraw();
			return;
		}
		if (event_type == INPUT_EVENT_ENTER) {
			/* Select network → move to password field */
			focus_area = FOCUS_PASS;
			wf_redraw();
			return;
		}
	}

	/* Password field editing */
	if (focus_area == FOCUS_PASS) {
		if (event_type == INPUT_EVENT_LEFT) {
			if (pass_cursor > 0) pass_cursor--;
			wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_RIGHT) {
			if (pass_cursor < pass_len) pass_cursor++;
			wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_HOME) {
			pass_cursor = 0; wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_END) {
			pass_cursor = pass_len; wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_BACKSPACE) {
			if (pass_cursor > 0) {
				int j;
				for (j = pass_cursor - 1; j < pass_len - 1; j++)
					pass_buf[j] = pass_buf[j + 1];
				pass_len--;
				pass_buf[pass_len] = '\0';
				pass_cursor--;
			}
			wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_DELETE) {
			if (pass_cursor < pass_len) {
				int j;
				for (j = pass_cursor; j < pass_len - 1; j++)
					pass_buf[j] = pass_buf[j + 1];
				pass_len--;
				pass_buf[pass_len] = '\0';
			}
			wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_CHAR && key) {
			if (pass_len < PASS_MAX - 1) {
				int j;
				for (j = pass_len; j > pass_cursor; j--)
					pass_buf[j] = pass_buf[j - 1];
				pass_buf[pass_cursor] = key;
				pass_len++;
				pass_buf[pass_len] = '\0';
				pass_cursor++;
			}
			wf_redraw(); return;
		}
		if (event_type == INPUT_EVENT_ENTER) {
			do_connect();
			return;
		}
	}

	/* Button focus */
	if (focus_area == FOCUS_CONNECT && event_type == INPUT_EVENT_ENTER) {
		do_connect();
		return;
	}
	if (focus_area == FOCUS_DISCON && event_type == INPUT_EVENT_ENTER) {
		do_disconnect();
		return;
	}
}

/* ── lifecycle ───────────────────────────────────────────────────── */

static void wf_on_close(gui_window_t* win) {
	wf_open = 0;
	wf_win = 0;
	gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
	gui_window_destroy(win);
}

void wificfg_app_open(void) {
	if (wf_open && wf_win) {
		gui_window_focus(wf_win);
		gui_dirty_add(wf_win->x, wf_win->y, wf_win->width, wf_win->height);
		return;
	}

	wf_win = gui_window_create("WiFi", 260, 60, WIN_W, WIN_H,
	    GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
	if (!wf_win) return;

	/* Reset state */
	focus_area   = FOCUS_LIST;
	list_sel     = 0;
	list_scroll  = 0;
	pass_buf[0]  = '\0';
	pass_len     = 0;
	pass_cursor  = 0;
	status_msg[0] = '\0';

	wf_win->on_paint = wf_paint;
	wf_win->on_key   = wf_on_key;
	wf_win->on_close = wf_on_close;

	wf_open = 1;
	gui_window_focus(wf_win);
	gui_dirty_add(wf_win->x, wf_win->y, wf_win->width, wf_win->height);

	/* Auto-scan on open */
	do_scan();
}
