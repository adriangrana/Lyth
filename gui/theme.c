/*
 * theme.c — Runtime theme state (light / dark / accent switching)
 */

#include "theme.h"
#include "vfs.h"
#include "string.h"
#include "desktop.h"

gui_theme_t theme;

static int current_mode = THEME_MODE_DARK;
static uint32_t accent_col = THEME_COL_ACCENT;

/* ---- Dark palette (Catppuccin Mocha) ---- */
static void apply_dark(void) {
    theme.base     = THEME_COL_BASE;
    theme.mantle   = THEME_COL_MANTLE;
    theme.crust    = THEME_COL_CRUST;
    theme.surface0 = THEME_COL_SURFACE0;
    theme.surface1 = THEME_COL_SURFACE1;
    theme.surface2 = THEME_COL_SURFACE2;
    theme.overlay0 = THEME_COL_OVERLAY0;
    theme.overlay1 = THEME_COL_OVERLAY1;
    theme.text     = THEME_COL_TEXT;
    theme.subtext1 = THEME_COL_SUBTEXT1;
    theme.subtext0 = THEME_COL_SUBTEXT0;
    theme.dim      = THEME_COL_DIM;
    theme.accent   = accent_col;
    theme.border   = THEME_COL_BORDER;
    theme.titlebar = THEME_COL_TITLEBAR;
    theme.titlebar_inactive = THEME_COL_TITLEBAR_INACTIVE;
    theme.titlebar_text     = THEME_COL_TITLEBAR_TEXT;
    theme.titlebar_text_dim = THEME_COL_TITLEBAR_TEXT_DIM;
    theme.taskbar_bg = THEME_COL_TASKBAR_BG;
    theme.dock_bg    = THEME_COL_DOCK_BG;
    theme.popup_bg   = THEME_COL_POPUP_BG;
    theme.popup_border = THEME_COL_POPUP_BORDER;
    theme.launcher_bg  = THEME_COL_LAUNCHER_BG;
    theme.wall_top = THEME_COL_WALL_TOP;
    theme.wall_mid = THEME_COL_WALL_MID;
    theme.wall_bot = THEME_COL_WALL_BOT;
    theme.shadow_alpha_mul = 100;  /* dark: normal shadow intensity */
}

/* ---- Light palette (Catppuccin Latte inspired) ---- */
static void apply_light(void) {
    theme.base     = 0xEFF1F5;
    theme.mantle   = 0xE6E9EF;
    theme.crust    = 0xDCE0E8;
    theme.surface0 = 0xCCD0DA;
    theme.surface1 = 0xBCC0CC;
    theme.surface2 = 0xACB0BE;
    theme.overlay0 = 0x9CA0B0;
    theme.overlay1 = 0x8C8FA1;
    theme.text     = 0x4C4F69;
    theme.subtext1 = 0x5C5F77;
    theme.subtext0 = 0x6C6F85;
    theme.dim      = 0x9CA0B0;
    theme.accent   = accent_col;
    theme.border   = 0xCCD0DA;
    theme.titlebar = 0xE6E9EF;
    theme.titlebar_inactive = 0xEFF1F5;
    theme.titlebar_text     = 0x4C4F69;
    theme.titlebar_text_dim = 0x9CA0B0;
    theme.taskbar_bg = 0xDCE0E8;
    theme.dock_bg    = 0xE6E9EF;
    theme.popup_bg   = 0xE6E9EF;
    theme.popup_border = 0xCCD0DA;
    theme.launcher_bg  = 0xDCE0E8;
    theme.wall_top = 0xB4BEFE;
    theme.wall_mid = 0x89B4FA;
    theme.wall_bot = 0x74C7EC;
    theme.shadow_alpha_mul = 140;  /* light: stronger shadows for contrast */
}

void theme_init(void) {
    current_mode = THEME_MODE_DARK;
    accent_col = THEME_COL_ACCENT;
    apply_dark();
}

void theme_set_dark(void) {
    current_mode = THEME_MODE_DARK;
    apply_dark();
}

void theme_set_light(void) {
    current_mode = THEME_MODE_LIGHT;
    apply_light();
}

void theme_toggle(void) {
    if (current_mode == THEME_MODE_DARK)
        theme_set_light();
    else
        theme_set_dark();
}

int theme_get_mode(void) {
    return current_mode;
}

void theme_set_accent(uint32_t colour) {
    accent_col = colour;
    theme.accent = colour;
}

uint32_t theme_get_accent(void) {
    return accent_col;
}

/* ---- Persistence: /etc/gui.conf ---- */

#define CONF_PATH "/etc/gui.conf"

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static uint32_t parse_hex6(const char *s) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < 6; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) return 0x00D4FF; /* fallback = default accent */
        v = (v << 4) | (uint32_t)d;
    }
    return v;
}

static void hex6_to_str(uint32_t v, char *out) {
    static const char hx[] = "0123456789ABCDEF";
    out[0] = hx[(v >> 20) & 0xF];
    out[1] = hx[(v >> 16) & 0xF];
    out[2] = hx[(v >> 12) & 0xF];
    out[3] = hx[(v >>  8) & 0xF];
    out[4] = hx[(v >>  4) & 0xF];
    out[5] = hx[ v        & 0xF];
    out[6] = '\0';
}

void theme_save(void) {
    char buf[128];
    int pos = 0;
    int fd;
    char hex[8];

    /* theme=dark\n or theme=light\n */
    memcpy(buf + pos, "theme=", 6); pos += 6;
    if (current_mode == THEME_MODE_LIGHT) {
        memcpy(buf + pos, "light", 5); pos += 5;
    } else {
        memcpy(buf + pos, "dark", 4); pos += 4;
    }
    buf[pos++] = '\n';

    /* accent=RRGGBB\n */
    memcpy(buf + pos, "accent=", 7); pos += 7;
    hex6_to_str(accent_col, hex);
    memcpy(buf + pos, hex, 6); pos += 6;
    buf[pos++] = '\n';

    /* autohide=0|1\n */
    memcpy(buf + pos, "autohide=", 9); pos += 9;
    buf[pos++] = desktop_taskbar_autohide_get() ? '1' : '0';
    buf[pos++] = '\n';

    fd = vfs_open_flags(CONF_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        vfs_write(fd, (const unsigned char*)buf, (unsigned int)pos);
        vfs_close(fd);
    }
}

void theme_load(void) {
    char buf[128];
    int fd, n;

    fd = vfs_open_flags(CONF_PATH, VFS_O_RDONLY);
    if (fd < 0) return; /* no saved settings — use defaults */

    n = vfs_read(fd, (unsigned char*)buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    /* parse line by line */
    {
        const char *p = buf;
        while (*p) {
            if (str_starts_with(p, "theme=")) {
                const char *val = p + 6;
                if (str_starts_with(val, "light"))
                    current_mode = THEME_MODE_LIGHT;
                else
                    current_mode = THEME_MODE_DARK;
            } else if (str_starts_with(p, "accent=")) {
                accent_col = parse_hex6(p + 7);
            } else if (str_starts_with(p, "autohide=")) {
                desktop_taskbar_autohide_set(p[9] == '1');
            }
            /* advance to next line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }

    /* apply loaded settings */
    if (current_mode == THEME_MODE_LIGHT)
        apply_light();
    else
        apply_dark();
}
