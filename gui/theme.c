/*
 * theme.c — Runtime theme state (light / dark / accent switching)
 */

#include "theme.h"

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
