/*
 * Lyth OS — Design System / Theme
 *
 * Single source of truth for all visual constants:
 * colours, spacing, radii, shadows, layout dimensions.
 *
 * Identity: elegant, dark, minimalist, technical.
 * Palette based on Catppuccin Mocha with Lyth adjustments.
 *
 * All GUI code should #include "theme.h" instead of defining local colours.
 */

#ifndef GUI_THEME_H
#define GUI_THEME_H

#include <stdint.h>

/* ==================================================================
 *  1. COLOUR PALETTE
 * ================================================================== */

/* ---- base backgrounds ---- */
#define THEME_COL_BASE          0x1E1E2E   /* main window background */
#define THEME_COL_MANTLE        0x181825   /* deeper panels, sidebars */
#define THEME_COL_CRUST         0x11111B   /* deepest (input fields, insets) */

/* ---- surfaces (elevated layers) ---- */
#define THEME_COL_SURFACE0      0x313244   /* cards, table headers, buttons */
#define THEME_COL_SURFACE1      0x45475A   /* hover state, elevated cards */
#define THEME_COL_SURFACE2      0x585B70   /* higher elevation, active state */

/* ---- overlay / backdrop ---- */
#define THEME_COL_OVERLAY0      0x6C7086   /* disabled, muted elements */
#define THEME_COL_OVERLAY1      0x7F849C   /* secondary borders */

/* ---- text ---- */
#define THEME_COL_TEXT          0xCDD6F4   /* primary text */
#define THEME_COL_SUBTEXT1     0xBAC2DE   /* secondary text */
#define THEME_COL_SUBTEXT0     0xA6ADC8   /* tertiary / labels */
#define THEME_COL_DIM          0x6C7086   /* placeholder, muted */

/* ---- accent colours ---- */
#define THEME_COL_ACCENT       0x00D4FF   /* cyan — primary accent */
#define THEME_COL_ACCENT_HOVER 0x33DFFF   /* lighter cyan — accent hover */
#define THEME_COL_FOCUS        0x00B4D8   /* deep cyan — focus rings, active dock */
#define THEME_COL_ACCENT_ALT   0x89B4FA   /* blue — secondary accent */
#define THEME_COL_ACCENT_PINK  0xD946EF   /* magenta/pink — tertiary accent */

/* ---- semantic colours ---- */
#define THEME_COL_SUCCESS      0xA6E3A1   /* green — OK, connected, running */
#define THEME_COL_WARNING      0xF9E2AF   /* yellow — warnings, sleeping */
#define THEME_COL_ERROR        0xF38BA8   /* red/pink — errors, kill, disconnect */
#define THEME_COL_INFO         0x89B4FA   /* blue — information, links */

/* ---- borders ---- */
#define THEME_COL_BORDER       0x313244   /* default border */
#define THEME_COL_BORDER_FOCUS 0x89B4FA   /* focused input border */
#define THEME_COL_BORDER_DIM   0x45475A   /* subtle separator */

/* ---- close / window control buttons (macOS-like traffic lights) ---- */
#define THEME_COL_CLOSE        0xFF5F57   /* close — red */
#define THEME_COL_CLOSE_HOVER  0xFF7B73   /* close hover */
#define THEME_COL_MINIMIZE     0xFFBD2E   /* minimize — yellow */
#define THEME_COL_MAXIMIZE     0x28C840   /* maximize — green */

/* ---- titlebar (glass-ready) ---- */
#define THEME_COL_TITLEBAR     0x161B22   /* active titlebar bg (glass base) */
#define THEME_COL_TITLEBAR_INACTIVE 0x0D1117 /* inactive titlebar */
#define THEME_COL_TITLEBAR_TEXT 0xE6EDF3   /* title text active */
#define THEME_COL_TITLEBAR_TEXT_DIM 0x7D8590 /* title text inactive */
#define THEME_COL_TITLEBAR_BORDER 0x30363D  /* subtle top/bottom edge */

/* ---- cursor ---- */
#define THEME_COL_CURSOR       0xF5C2E7   /* text cursor (pink) */

/* ---- shell / taskbar (glass-style, solid fallback until blur) ---- */
#define THEME_COL_TASKBAR_BG   0x0D1117   /* near-black glass base */
#define THEME_COL_TASKBAR_TEXT 0xE6EDF3
#define THEME_COL_TASKBAR_DIM  0x7D8590
#define THEME_COL_TASKBAR_SEP  0x21262D

#define THEME_COL_DOCK_BG      0x161B22   /* dock glass base */
#define THEME_COL_DOCK_HOVER   0x21262D
#define THEME_COL_DOCK_ACTIVE  0x00D4FF   /* cyan accent */
#define THEME_COL_DOCK_DOT     0x00D4FF   /* running indicator */

/* ---- launcher / start menu (glass-ready) ---- */
#define THEME_COL_LAUNCHER_BG     0x0D1117
#define THEME_COL_LAUNCHER_PANEL  0x161B22
#define THEME_COL_LAUNCHER_SEARCH 0x21262D
#define THEME_COL_LAUNCHER_TEXT   0xE6EDF3
#define THEME_COL_LAUNCHER_DIM    0x7D8590
#define THEME_COL_LAUNCHER_HOVER  0x00D4FF
#define THEME_COL_LAUNCHER_SEP    0x21262D
#define THEME_COL_LAUNCHER_ICON   0x21262D

/* ---- popup / context / tooltip (glass-ready) ---- */
#define THEME_COL_POPUP_BG     0x161B22
#define THEME_COL_POPUP_BORDER 0x30363D
#define THEME_COL_POPUP_TEXT   0xE6EDF3
#define THEME_COL_POPUP_DIM    0x7D8590
#define THEME_COL_POPUP_HOVER  0x00D4FF

/* ---- wallpaper gradient (vibrant aurora) ---- */
#define THEME_COL_WALL_TOP     0x1A0533   /* deep purple top */
#define THEME_COL_WALL_MID     0x0D2B5E   /* deep blue mid */
#define THEME_COL_WALL_BOT     0x0A4F6E   /* teal-blue bottom */
#define THEME_COL_WALL_ACCENT1 0x6B21A8   /* purple glow */
#define THEME_COL_WALL_ACCENT2 0x0891B2   /* cyan glow */
#define THEME_COL_WALL_ACCENT3 0xDB2777   /* pink glow */

/* ---- dock icon per-app colours (identity, not theme) ---- */
#define THEME_COL_ICON_FILES   0x4FC3F7
#define THEME_COL_ICON_SETTINGS 0x78909C
#define THEME_COL_ICON_TERMINAL 0x263238
#define THEME_COL_ICON_CALC    0xEF5350
#define THEME_COL_ICON_EDITOR  0xFFB74D
#define THEME_COL_ICON_VIEWER  0xAB47BC
#define THEME_COL_ICON_TASKMAN 0x66BB6A
#define THEME_COL_ICON_NETCFG  0x42A5F5
#define THEME_COL_ICON_ABOUT   0x5C6BC0
#define THEME_COL_ICON_SYSINFO 0x26A69A

/* ==================================================================
 *  2. SPACING SCALE (in pixels)
 * ================================================================== */

#define THEME_SP_2      2
#define THEME_SP_4      4
#define THEME_SP_8      8
#define THEME_SP_12    12
#define THEME_SP_16    16
#define THEME_SP_24    24
#define THEME_SP_32    32

/* ==================================================================
 *  3. CORNER RADII
 * ================================================================== */

#define THEME_RADIUS_NONE   0
#define THEME_RADIUS_SM     2   /* subtle rounding (buttons, inputs) */
#define THEME_RADIUS_MD     3   /* cards, panels, popups */
#define THEME_RADIUS_LG     3   /* windows, modals, launcher — max supported by draw_rounded_rect */
/* NOTE: when GPU compositor lands, increase LG to 12-16 for glass look */

/* ==================================================================
 *  4. SHADOW LEVELS (conceptual — used for visual hierarchy)
 *     Each defines an approximate alpha factor (0–255) and spread.
 * ================================================================== */

#define THEME_SHADOW_NONE     0
#define THEME_SHADOW_LOW      1   /* subtle, cards */
#define THEME_SHADOW_MEDIUM   2   /* windows, panels */
#define THEME_SHADOW_HIGH     3   /* modals, context menus */

/* multi-layer soft shadow parameters (3 concentric rings) */
#define THEME_SHADOW_COLOR    0x000000
#define THEME_SHADOW_LAYERS   3
/* layer 0 (inner):  offset 2, spread +2, alpha 45 */
#define THEME_SHADOW_OFF0     2
#define THEME_SHADOW_SPREAD0  2
#define THEME_SHADOW_ALPHA0  45
/* layer 1 (middle): offset 4, spread +5, alpha 25 */
#define THEME_SHADOW_OFF1     4
#define THEME_SHADOW_SPREAD1  5
#define THEME_SHADOW_ALPHA1  25
/* layer 2 (outer):  offset 6, spread +8, alpha 12 */
#define THEME_SHADOW_OFF2     6
#define THEME_SHADOW_SPREAD2  8
#define THEME_SHADOW_ALPHA2  12
/* total max shadow extent (for dirty rect sizing) = OFF2 + SPREAD2 */
#define THEME_SHADOW_EXTENT  14

/* window corner radius (used by compositor for corner masking) */
#define THEME_WIN_RADIUS      3

/* snap zone detection threshold (pixels from screen edge) */
#define THEME_SNAP_EDGE       4
#define THEME_SNAP_PREVIEW_ALPHA 50
#define THEME_SNAP_PREVIEW_COL THEME_COL_ACCENT  /* cyan */

/* ==================================================================
 *  5. LAYOUT DIMENSIONS
 * ================================================================== */

#define THEME_TITLEBAR_H     28   /* window titlebar height */
#define THEME_BORDER_W        1   /* window border width */
#define THEME_TASKBAR_H      36   /* top taskbar height */
#define THEME_DOCK_H         52   /* bottom dock bar height */

#define THEME_FONT_W          8   /* current font glyph width */
#define THEME_FONT_H         16   /* current font glyph height */

/* ---- Typographic scale ---- */
/* Caption:     normal font (8x16), tighter line height  */
/* Body:        normal font (8x16), standard line height */
/* Body Strong: normal font (8x16), bold emulated        */
/* Subtitle:    2x scaled (16x32)                        */
/* Title:       2x scaled (16x32)                        */
#define THEME_TYPO_CAPTION_H   16   /* caption line height */
#define THEME_TYPO_BODY_H      18   /* body line height (16 + 2 leading) */
#define THEME_TYPO_BODY_W       8   /* body character width */
#define THEME_TYPO_SUBTITLE_H  34   /* subtitle line height (32 + 2) */
#define THEME_TYPO_SUBTITLE_W  16   /* subtitle character width */
#define THEME_TYPO_TITLE_H     34   /* title line height */
#define THEME_TYPO_TITLE_W     16   /* title character width */

#define THEME_DOCK_ICON_SZ   28   /* dock icon size */
#define THEME_DOCK_ICON_PAD   8   /* dock inter-icon padding */

#define THEME_BTN_H          28   /* standard button height */
#define THEME_INPUT_H        28   /* standard input height */
#define THEME_SCROLLBAR_W    10   /* scrollbar width */
#define THEME_MENU_ITEM_H    24   /* context menu item height */

/* ==================================================================
 *  6. ANIMATION DURATIONS (ms)  — for future use
 * ================================================================== */

#define THEME_ANIM_MICRO     80   /* hover, press feedback */
#define THEME_ANIM_NORMAL   200   /* open/close, transitions */
#define THEME_ANIM_MODAL    300   /* modal appear/disappear */
#define THEME_ANIM_WORKSPACE 400  /* workspace switch */

/* ==================================================================
 *  7. TYPOGRAPHY SIZES (future — currently single font)
 * ================================================================== */

#define THEME_FONT_SZ_CAPTION    12
#define THEME_FONT_SZ_BODY       14
#define THEME_FONT_SZ_BODY_BOLD  14
#define THEME_FONT_SZ_TITLE      18
#define THEME_FONT_SZ_HEADING    24

/* ==================================================================
 *  8. RUNTIME THEME (light / dark switching)
 * ================================================================== */

#define THEME_MODE_DARK   0
#define THEME_MODE_LIGHT  1

/* Runtime palette — subset of colours that change between themes */
typedef struct {
    uint32_t base;
    uint32_t mantle;
    uint32_t crust;
    uint32_t surface0;
    uint32_t surface1;
    uint32_t surface2;
    uint32_t overlay0;
    uint32_t overlay1;
    uint32_t text;
    uint32_t subtext1;
    uint32_t subtext0;
    uint32_t dim;
    uint32_t accent;
    uint32_t border;
    uint32_t titlebar;
    uint32_t titlebar_inactive;
    uint32_t titlebar_text;
    uint32_t titlebar_text_dim;
    uint32_t taskbar_bg;
    uint32_t dock_bg;
    uint32_t popup_bg;
    uint32_t popup_border;
    uint32_t launcher_bg;
    uint32_t wall_top;
    uint32_t wall_mid;
    uint32_t wall_bot;
} gui_theme_t;

extern gui_theme_t theme;    /* active palette — use instead of THEME_COL_* */

void theme_init(void);       /* call once at startup */
void theme_set_dark(void);
void theme_set_light(void);
void theme_toggle(void);
int  theme_get_mode(void);   /* returns THEME_MODE_DARK or THEME_MODE_LIGHT */
void theme_set_accent(uint32_t colour);
uint32_t theme_get_accent(void);
void theme_save(void);       /* persist theme/accent/autohide to /etc/gui.conf */
void theme_load(void);       /* restore settings from /etc/gui.conf */

#endif /* GUI_THEME_H */
