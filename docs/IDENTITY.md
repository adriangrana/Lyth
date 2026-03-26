# Lyth OS — Design Identity

## Core Traits (5 pillars)

### 1. Quiet Precision
Everything has a reason. No ornamental effects. Spacing follows the 2/4/8/12/16/24 scale.
Corners are consistently small (SM=2, MD=3). Shadows are subtle and purposeful —
they communicate depth, not decoration. Animations are short (80–200 ms) with ease-out.

### 2. Glass & Depth
Translucent surfaces with soft shadows create a layered, spatial feel.
The dock, launcher, popups, and context menus use alpha-blended backgrounds
(200–220/255). A single accent border anchors floating elements visually.
Every overlay sits on a clear hierarchy: desktop → windows → popups → OSD.

### 3. Warm Palette, Cool Structure
Catppuccin-derived colours give warmth (Mocha dark, Latte light). The accent
system lets users personalise without breaking consistency. Text contrast is
always guaranteed via `theme_contrast_text()`. Shadow intensity adapts between
themes (`shadow_alpha_mul`).

### 4. Tools, Not Toys
Apps are functional first. Empty states guide the user ("Start typing to begin",
"No files found — drag or open"). No splash animations beyond boot. The desktop
is a workspace, not a showcase. The perf overlay (F3) and dirty-rect visualiser
(F4) are available for developers without entering a special mode.

### 5. One Voice
Every icon uses the same grid and single-char fallback convention (APP_ICON_*).
Radius, shadow, and spacing constants are defined once in `theme.h` and used
everywhere. No style mixing: the widget kit (`sf_rrect`) renders all interactive
elements with the same 2px corners. Tabs, buttons, and inputs share a visual
language.

## What Lyth Is Not

- Not a macOS/Windows clone — no heavy blur, no vibrancy, no translucent titlebars.
- Not maximalist — no animated wallpapers, particle effects, or gratuitous transitions.
- Not inconsistent — every screen is designed, every element uses the theme system.

## Design Tokens (quick reference)

| Token | Value | Usage |
|---|---|---|
| Radius SM | 2px | Buttons, inputs, hover highlights |
| Radius MD | 3px | Panels, popups, tooltips, icons |
| Radius LG | 3px | Windows, launcher, modals |
| Radius Pill | 3px | Taskbar items, dock background (future: larger) |
| Shadow layers | 3 | Inner(2+2,α45), Mid(4+5,α25), Outer(6+8,α12) |
| Border width | 1px | Window borders, separators |
| Titlebar height | 28px | All windows |
| Font | 8×16 PSF | Body text; 16×32 for subtitles/titles |
| Animation normal | 200ms | Window open/close/move |
| Animation micro | 80ms | Hover, press feedback |
| Frame budget | 16ms | 60 fps target |
