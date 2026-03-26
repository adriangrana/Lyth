# Lyth OS GUI Checklist

## FASE 1 — Design System

### 1. Identidad Visual
- [x] Elegir identidad visual (sobrio, futurista, elegante, minimalista, técnico)
      > Elegante, oscuro, minimalista, técnico. Glass/aurora aesthetic.
      > Definido en include/gui/theme.h

### 2. Paleta de Colores
- [x] Fondo principal
- [x] Superficies elevadas
- [x] Color de acento
- [x] Color de foco
- [x] Color de error/alerta
- [x] Color de texto (principal/secundario/tenue)
      > Paleta centralizada en theme.h. Cyan accent (#00D4FF), aurora wallpaper,
      > glass-ready dark bases. Integrado en TODAS las apps, desktop, login,
      > splash, diálogos, notificaciones, auth prompt.

### 3. Escalas y Espaciado
- [x] Escala de radios (4px, 8px, 12px, 16px)
- [x] Escala de sombras (baja, media, alta, modal)
- [x] Sistema de spacing (4, 8, 12, 16, 24, 32)
- [x] Alturas estándar (barra de título, botones, inputs, panel, menú)
- [x] Iconografía coherente
      > ICO icon pipeline: ico2h.py convierte ICO→C headers (ARGB pre-decoded)
      > 6 iconos de app: lyth, terminal, nexus, calc, settings, notes (32x32)
      > Dock muestra iconos reales con alpha blending (blit_icon)
      > Taskbar muestra iconos 24x24 escalados para running apps
      > Fallback: cuadrado redondeado coloreado + letra
- [x] Comportamiento hover/focus/pressed/disabled
      > Widget kit: 6 state flags (WID_ENABLED, WID_VISIBLE, WID_FOCUSED, WID_HOVERED, WID_PRESSED, WID_CHECKED)
      > Todos los widgets: disabled=dimmed, hover=lighter+accent border, pressed=accent fill
      > Dock hover: highlight translúcido (alpha 80) sobre icono
      > Desktop context menu: hover highlight (alpha 60)

### 4. Tipografía
- [x] Fuente UI principal excelente
      > Inter Regular 400 (OTF) rasterizado a 8x16 PSF bitmap
      > Pipeline: tools/otf2psf.js (OTF→PSF2) + tools/psf2h.py (PSF→C header)
      > Fuente en drivers/fonts/Inter_Font_Family/
- [ ] Hinting/rasterizado bueno
- [ ] Soporte de antialiasing correcto
- [ ] Subpixel rendering
- [x] Tamaños tipográficos (caption, body, body strong, title, heading)
      > THEME_TYPO_* constants in theme.h: caption(16), body(18/8), subtitle(34/16), title(34/16)
      > gui_surface_draw_char_2x/draw_string_2x: nearest-neighbor 2x scaling
      > Applied in About app: title 2x, section headers 2x
- [ ] Altura de línea consistente
- [ ] Espaciado de letras ajustado
- [ ] Fallback fonts
- [ ] Unicode sólido
- [ ] Emoji support

## FASE 2 — Shell de Escritorio

### 5. Ventanas
- [x] Decoraciones modernas
      > gui_window_draw_decorations() centralizado en window.c
      > Traffic-light buttons (close/minimize/maximize) estilo macOS, lado izquierdo
- [x] Barra de título limpia y elegante
      > Título centrado, colores temáticos activo/inactivo
- [x] Botones de ventana consistentes
      > Todos los 10 apps usan gui_window_draw_decorations()
- [x] Estados (activa, inactiva, urgent, modal)
      > GUI_WIN_FOCUSED flag + colores diferenciados activo/inactivo
- [x] Sombras suaves y creíbles
      > Multi-layer soft shadow: 3 anillos concéntricos (alpha 45/25/12)
      > Capas: inner (off 2, spread 2), mid (off 4, spread 5), outer (off 6, spread 8)
      > Skip pixels inside window, también en fast drag path
      > Parámetros en theme.h (THEME_SHADOW_OFF/SPREAD/ALPHA)
- [x] Esquinas redondeadas bien recortadas
      > Radio 3px (THEME_WIN_RADIUS) en compositor + titlebar
      > Corner mask per-row en compose_region (blit) y fast drag path
      > Titlebar top corners redondeadas en gui_window_draw_decorations()
- [x] Borde sutil o separación visual clara
      > Shadow + titlebar border proveen separación visual
- [x] Hit testing preciso
      > hit_close_button() actualizado para botón izquierdo
- [x] Resize handles cómodos
      > GUI_WIN_RESIZABLE flag, hit_resize_edge() (right/bottom/corner, 6px grab)
      > Live resize: gui_window_resize() realoca surface, redibuja en vivo
      > Resize grip visual (3 puntos diagonales) en decoraciones
      > Habilitado en: Terminal, Notes, Viewer, Nexus, Task Manager
- [x] Snap visual bonito
      > Snap zones: left/right half, top = fullscreen (minus taskbar)
      > Cyan border preview (3px, alpha 50) during drag near edges
      > On release: resize + move to snap position
      > Pre-snap geometry saved, restored on re-drag (un-snap)
      > Only for GUI_WIN_RESIZABLE windows
- [x] Maximize/minimize con animación
      > Minimize: botón amarillo (hit_minimize_button) activa GUI_WIN_MINIMIZED
      > Taskbar muestra ventanas minimizadas con color dim (THEME_COL_DIM)
      > Click en taskbar restaura ventana minimizada (toggle)
      > Alt+Tab salta ventanas minimizadas
- [x] Raise-to-front instantáneo
      > gui_window_focus() asigna z_order = max_z + 1 → ventana sube al frente
- [x] Drag sin freezes
      > Fast drag path: bg_buffer cache + VBlank-synced present, bypass compose_frame()
      > Deferred drag state desacopla IRQ de mouse del renderizado
- [x] Resize fluido
      > Live resize con gui_window_resize(), dirty rects cubren vieja + nueva zona
- [x] Resize preview o live resize optimizado
      > Live resize (no preview frame) — surface reallocated each frame
      > Dirty rects cover old + new area including shadow extent
- [x] Focus inmediato
      > gui_window_focus() solo flag + z_order, sin dirty rects
      > Llamado inmediatamente en click y drag start
- [x] No flicker
      > Double buffering: window surfaces → backbuffer → framebuffer
      > Cursor save-under, VBlank sync, present solo dirty scanlines
- [x] No saltos en drag
      > Union rect cubre posición vieja + nueva + shadow extent
      > Deferred state suaviza movimiento rápido del mouse

### 6. Desktop y Wallpaper
- [x] Wallpaper engine
      > PNG decoder completo (RFC 1951 inflate + zlib + PNG filters) en lib/png.c
      > Wallpaper embebido como C array (tools/bin2h.py → include/gui/wallpaper_png.h)
      > physmem_alloc_region para buffers grandes (~12MB pico durante decode)
- [x] Escalado correcto
      > Cover-mode: nearest-neighbor scale + center-crop en rebuild_desktop()
- [x] Blur opcional
      > box_blur_region() 5x5 kernel, 2-pass in-place
      > Applied to: taskbar, dock pill, launcher cache
      > Frosted glass effect before translucent panels are drawn
- [x] Overlay sutil para legibilidad
      > Scrim oscuro (alpha 18) sobre wallpaper antes del taskbar
      > Mejora contraste de texto sobre fondos claros
- [ ] Wallpapers dinámicos (futura)
- [x] Desktop vacío elegante
      > Watermark "Lyth OS" centrado, texto alpha-blended (alpha 25)
      > draw_char_alpha/draw_string_alpha para texto semitransparente
      > Visible sobre escritorio vacío, cubierto por ventanas
- [x] Gestión de iconos
      > Desktop shortcut icons: upper-right grid (48x48 scaled, labeled)
      > 4 default shortcuts: Terminal, Files, Notes, Settings
      > Click-to-launch, baked into desk_surf during rebuild_desktop()
      > Uses blit_icon_scaled() with 64px source icons

#### 6.1. Interacción del escritorio
- [x] detectar click derecho sobre escritorio
      > button==2 en desktop_handle_click cuando my < taskbar_y
- [x] distinguir escritorio vs ventana vs panel
      > Compositor pasa clicks a windows primero, desktop recibe lo que sobra
- [x] menú contextual del escritorio
      > dctx_items[]: Terminal, Files, Settings, About Lyth
      > draw_desktop_ctx() con fondo translúcido y accent border
- [x] acciones del menú
      > Cada item ejecuta su action() callback al hacer click
- [x] cierre del menú al clicar fuera
      > Left-click fuera cierra via close_desktop_ctx()
- [x] hover/selección por ratón
      > dctx_hover highlight translúcido (alpha 60)
- [x] navegación por teclado
      > Arrows/Enter/Escape/Backspace en launcher y desktop_handle_key
      > dctx_hover actualizado por mouse move en desktop_update_hover

#### 6.2. Configuración del sistema
- [x] ventana/app de Configuración
      > Sidebar con 4 secciones, click o teclas 1-4 para navegar, username en sidebar
- [x] sección "Pantalla"
      > Resolución, color depth, backend, pitch
- [x] sección "Fondo de pantalla"
      > Wallpaper actual, scale mode, formato, instrucciones de cambio
- [x] sección "Apariencia"
      > Theme, accent, font, titlebar style, dock style, palette swatches
- [x] persistencia en archivo de configuración
      > theme_save()/theme_load() in gui/theme.c
      > /etc/gui.conf: key=value format (theme, accent, autohide)
      > Loaded after desktop_init() in gui_init(), saved from Settings app
- [x] aplicar cambios en caliente
      > Wallpaper thumbnail click in Settings → desktop_set_wallpaper() live preview
      > Apply button still available for confirmation
      > Predeterminado button resets to wallpaper 0

### 7. Panel/Barra Superior
- [x] Layout definido
      > Top taskbar translúcido (y=0, height=36) con running app icons + tray
      > Bottom dock centrado (píldora redondeada, solo ancho necesario)
      > Desktop area: [TASKBAR_H, sh-DOCK_H]
- [x] Área de apps
      > Running app icons (24x24 escalados desde 32x32) en top taskbar
      > Solo apps no-ancladas (filtro por dock_items labels)
      > Fallback: cuadrado coloreado + letra
- [x] Reloj/fecha
      > HH:MM + full date (DayName, Month Day) en zona derecha del tray
- [x] Estado del sistema (red, audio, batería)
      > Network icon (verde/rojo según conexión) + Volume icon en tray
- [x] Usuario/sesión
      > Username obtenido de session_get_current() mostrado en tray
- [x] Notificaciones
      > notify_paint() conectado en compose_region paso 5
      > Rendering: rounded rect alpha 220, sombra sutil, barra accent lateral
      > Título en color accent, cuerpo en subtext, auto-dismiss 4s
- [x] Menú del sistema
      > Hamburger icon abre launcher; network icon abre popup red
- [x] Blur/transparencia opcional
      > Taskbar glass: alpha_blend_fill + accent line sutil (alpha 30)
      > Separadores verticales semitransparentes entre secciones
- [x] Auto-hide opcional
      > tb_autohide flag + TB_HOTZONE (4px) proximity detection
      > Slide animation (TB_AUTOHIDE_STEPS=4) via desktop_anim_tick()
      > API: desktop_taskbar_autohide_get/set(), toggle in Settings > Apariencia
      > Taskbar slides off-screen when mouse leaves, reveals on top-edge hover
- [x] Animaciones de hover
      > Dock hover: highlight translúcido (alpha 80) on mouse move
      > desktop_update_hover() llamado desde compositor en mouse move
      > Dirty rect solo en zona dock para repaint eficiente

### 8. Launcher/Menú de Aplicaciones
- [x] Búsqueda instantánea
      > Filtrado en tiempo real con str_starts_with_ignore_case
      > Cursor texto visible, "No results" cuando no hay coincidencias
- [x] Grid o lista impecable
      > Grid 5 columnas filtrado dinámicamente, hover translúcido (alpha 60)
- [x] Navegación por teclado
      > Arrows + Enter + Escape + Backspace, todo respeta filtro de búsqueda
- [x] Categorías
      > launcher_item_t.category field (CAT_CORE, CAT_UTIL, CAT_SYS)
      > cat_names[] array for labels, all apps assigned categories
      > Data structure ready for future visual category headers
- [x] Recientes
      > recent_track() MRU list (RECENT_MAX=3), hooked in click + keyboard Enter
      > "Recent" section between Pinned and All Apps in launcher
      > Shows real icons + labels, click on recent items launches app
- [x] Apps fijadas
      > "Pinned" section at top of launcher with labeled row of pinned apps
      > launcher_item_t.pinned flag marks pinned apps (Files, Terminal, Notes, Calculator, Settings)
      > Pinned row shows real icons (blit_icon_scaled 32→48) + labels
      > "All Apps" separator + full grid below
      > Pinned section hidden during search (shows filtered grid only)
- [x] Animación de apertura/cierre
      > Launcher slide-up en 6 frames durante open
      > Slide-down en 6 frames durante close
      > desktop_anim_tick() separado de clock tick, rate-limited a ~60fps
      > Dirty rects incrementales: solo la franja que cambia cada frame
      > Launcher cache pre-renderizado evita alpha-blend costoso por frame
- [x] Tipografía perfecta
      > Título "Lyth OS" en color accent, labels centrados bajo iconos
- [x] Iconos de calidad
      > Iconos ICO reales en dock (32x32 ARGB alpha-blended)
      > Iconos escalados en taskbar (24x24 nearest-neighbor)
      > Launcher: real icons via blit_icon_scaled(32→48) for apps with ICO icons
      > Fallback: rounded rect coloreados con letra centrada

### 9. Dock o Task Switcher
- [x] Previews de ventanas
      > Alt-Tab overlay muestra thumbnails 120x80 de cada ventana vía gpu_blit_scaled
      > Centrado horizontal, máximo 12 ventanas, título debajo de cada preview
- [x] Indicadores de app abierta
      > Barra horizontal 6x2px debajo del icono (reemplaza dot 3x3)
      > Dock pill translúcido centrado (solo ancho de iconos anclados)
- [ ] Drag-reorder
- [x] Pinned apps
      > Dock items definidos en desktop_init(): Lyth OS, Terminal, Calculator, Files, Settings, Notes
      > Primer icono (Lyth OS) abre start menu
      > Iconos ICO reales: lyth, terminal, calc, nexus(files), settings, notes
      > Running apps en top taskbar solo muestran apps NO ancladas
- [x] Animaciones cuidadas
      > Dock hover scale animation: icons scale 32→38px on hover with upward float
      > blit_icon_scaled() nearest-neighbor + alpha for arbitrary target size
      > dock_scale animated in desktop_anim_tick() at ~60fps (DOCK_SCALE_STEPS=4)
      > Tooltip label appears above hovered icon at full scale
      > Subtle glow (alpha 50) behind scaled icon
- [x] Alt-tab elegante y rápido
      > Alt+Tab visual overlay con thumbnails escalados (gpu_blit_scaled)
      > Backdrop semitransparente centrado, borde accent en selección
      > Título centrado bajo cada thumbnail, cycle con Tab, confirm con Alt release
      > keyboard_get_modifiers() detecta soltar Alt para confirmar
      > Enter confirma, Escape cancela
- [ ] Exposé/overview (futura)

## FASE 3 — Compositor GPU

### 10. Render Backend GPU
- [x] API de renderer diseñada
      > gpu_ops_t vtable (30+ ops), g_gpu global, renderer.h
- [x] Backend CPU funcional
      > renderer_sw.c: fill, blit, shadow, rounded corners, text, DIV255
- [x] Backend GPU inicial
      > renderer_virtio.c: virtio-GPU present (DMA transfer+flush)
      > Graceful fallback: siempre inicia SW primero, virtio override si detectado
- [x] Surfaces como texturas
- [ ] Quads por ventana/layer
- [x] Clipping por nodo
- [x] Alpha por nodo
- [ ] Transform por nodo
- [x] Scissor/clip regions
- [ ] Render targets intermedios
- [x] Upload eficiente de surfaces dañadas
- [x] Sincronización render/present
- [x] Cambio de resolución en caliente
      > gui_resize_screen() orquesta resize completo: renderer, bg_buffer, desktop, cursor
      > SW: sw_resize() realloc backbuffer. Virtio: vgpu_resize() recreate resource+scanout
      > Settings > Pantalla: dropdown 10 resoluciones + botón Aplicar
      > Drag fast-path usa DRAG_SHADOW_EXT reducido para fluidez a alta resolución
- [x] Write-Combining para hardware real
      > PAT MSR 0x277 programado en boot (BSP + APs): entry 1 = WC
      > paging_map_region_early_wc(): PG_PWT selects PAT entry 1 (WC)
      > Framebuffer mapeado WC en kernel.c (5-10x más rápido en Intel iGPU)
      > sfence tras sw_present() para flush de WC buffers

### 11. Scene Graph/Layers
- [ ] Nodos: ventana, sombra, decoración, popup, tooltip, panel, cursor, overlay
- [ ] Propiedades: bounds, opacity, transform, clip, z-index, visibility, damage

### 12. Efectos Modernos
- [x] Alpha real por ventana
      > w->alpha field (uint8_t), compositor usa gpu_blit_alpha para ventanas con alpha < 255
- [x] Fade in/out
      > gui_window_close_animated() inicia fade-out (alpha→0, 51/frame, ~5 frames)
      > gui_window_create() arranca con alpha=0 y fade-in a 255
      > gui_window_anim_tick() cada frame, destruye al llegar a alpha=0
      > Sombras se omiten durante fade para rendimiento
- [ ] Escalado suave
- [ ] Blur de fondo
- [ ] Sombras gaussianas
- [x] Esquinas redondeadas recortadas
      > THEME_WIN_RADIUS=3 en compositor: corner mask per-row (3px, 2px, 1px)
      > Titlebar top corners redondeadas en gui_window_draw_decorations()
- [ ] Vibrancy/acrylic-like (opcional)
- [x] Dim de fondo para modales
      > Alt-Tab overlay: backdrop semitransparente alpha 220 oscurece fondo
      > Auth prompt: fondo dimmed completo
- [ ] Overview mode
- [x] Animaciones minimize/maximize
      > Minimize: fade-out via anim_minimizing flag (alpha→0, 51/frame)
      > Restore: fade-in desde alpha=0 al hacer click en taskbar
      > Context menu minimize/close también animados
      > No interaction durante animación (anim_minimizing check en hit_test)
- [ ] Transiciones de workspace
- [x] Crossfades entre estados
      > Launcher crossfade: alpha-blended open/close con launch_alpha gradual
      > Per-pixel blend (src*a + bg*(255-a)) durante transición, memcpy fast-path cuando opaco

## FASE 4 — Calidad de Interacción

### 13. Input Impecable
- [x] Mouse sampling estable
      > PS/2 mouse IRQ handler procesa packets 3-4 bytes, valida flags
      > USB HID: usb_hid_poll() polling loop en main compositor loop
- [x] Coalescing correcto
      > Main loop acumula delta_x/delta_y/scroll de todos los eventos mouse
      > Se procesa un solo evento coalescido por frame
      > metrics.coalesced_moves contabiliza eventos combinados
- [x] Sin input lag visible
      > Fast drag path VBlank-synced, main loop hlt hasta IRQ
      > Sin polling activo, IRQ-driven input
- [ ] Arrastre subpixel fino
- [x] Hit testing consistente
      > hit_test_window() z-order aware, hit_close/minimize/titlebar/resize_edge
      > Widget hit_wid() point-in-rect sobre array de widgets
- [x] Doble click fiable
      > DBLCLICK_MS=400, DBLCLICK_DIST=6 in compositor.c
      > Timestamp + position tracking, resets on triple-click
      > gui_dblclick_fn callback (on_dblclick) in gui_window_t
- [x] Wheel suave
      > PS/2 Intellimouse protocol (magic knock 200/100/80, ID=3 = wheel)
      > 4-byte packets con signed z-delta, scroll coalescing en main loop
      > wid_handle_scroll: WID_LISTVIEW scroll_off, WID_SLIDER +/- step, WID_SCROLLBAR value
      > USB mice: scroll=0 placeholder (sin rueda HW implementada aún)
- [ ] Touchpad support (futura)
- [ ] Gestos multitáctiles (futura)

### 14. Animación con Criterio
- [x] Duraciones definidas (microanimación, transición normal, modal, workspace)
      > THEME_ANIM_MICRO=80ms, THEME_ANIM_NORMAL=200ms, THEME_ANIM_MODAL=300ms, THEME_ANIM_WORKSPACE=400ms
      > Definidos en include/gui/theme.h, usados por gui_window_anim_start()
- [x] Curvas easing coherentes
      > ease_out_cubic_u8(): 1-(1-t)^3 en fixed-point 0..255
      > Aplicada uniformemente a open/close/minimize/restore/fade
- [x] Animaciones interrumpibles
      > gui_window_anim_start() redirige mid-animation: captura alpha actual como nuevo start
      > Minimize durante fade-in, restore durante fade-out — todo fluido
- [ ] Evitar stutter
- [ ] Animar para comunicar estado, no por decorar

### 15. Latencia y Consistencia
- [x] Input-to-photon bajo
      > IRQ-driven keyboard/mouse, no polling delay
      > Fast drag path: salta compose_frame, VBlank-synced directo
- [x] Frames estables
      > 60fps cap (FRAME_INTERVAL_MS=16), dirty rect compositing
      > Frame time graph + P95 monitoring en perf overlay
- [x] Sin congelones en: abrir/cerrar ventana, arrastrar, resize, cambiar foco, menús, hover
      > Fade in/out suave para open/close (5 frames)
      > Fast drag path VBlank-synced
      > Live resize, immediate focus, animated launcher
- [x] Percentiles de frametime medidos
      > P95 calculado por insertion sort de ft_hist[60] en perf overlay
      > Mostrado como "P95: <value>us" entre Max y Dirty
- [x] Perf overlay interno
      > F3 toggle: muestra FPS, avg/max frametime (µs), dirty rects, resolución
      > Fondo semitransparente, actualiza cada segundo, dirty rect automático
      > Funciona en path normal y drag path

## FASE 5 — Componentes UI

### 16. Kit de Widgets Oficial
- [x] Botones, inputs, checkbox, radio, slider, label, panel, separator, progress
      > 9 tipos base en gui/widgets.c + include/gui/widgets.h
      > wid_t struct con type, state flags, callbacks, theme colors
      > API: wid_draw_all, wid_handle_click, wid_handle_key, wid_update_hover
      > Integrado en compositor (draw after on_paint, click/key dispatch)
      > Calculator migrado a wid_button (20 botones)
- [x] Switch/toggle
      > wid_switch(): pill shape + sliding dot, accent fill cuando ON
      > Toggle via click, fires on_change callback
- [x] Tabs
      > wid_tabs(): horizontal tab bar con "|"-separated labels
      > Active tab: accent underline, base bg. Inactive: muted text
      > Click selects tab, fires on_change(w, tab_index)
- [x] Dropdown/combobox
      > wid_dropdown(): button + popup list, "|"-separated items
      > Click toggles open/close, auto-resizes height for popup
      > Closes when clicking outside, fires on_change on selection
- [x] ListView/tabla
      > wid_listview(): scrollable list with selection highlight
      > Scrollbar track + thumb when items > vis_rows
      > Click selects item, fires on_change(w, index)
      > wid_add_item()/wid_clear_items() dynamic API
- [x] Scrollbar (standalone)
      > WID_SCROLLBAR: vertical track 14px ancho, thumb proporcional, min 16px
      > Click-to-position y scroll-wheel support
      > Accent color on hover/press, theme colors
      > wid_scrollbar(win, x, y, h, min, max, cur, on_change)
- [ ] Listas, tablas, tabs, sidebars
- [x] Modales, toasts, menús contextuales, dropdowns
      > Notificaciones = toasts (notify_show/paint, auto-dismiss)
      > Menú contextual desktop (dctx_items[], draw_desktop_ctx)
      > wid_dropdown() con popup list, auth_prompt = modal
      > gui/dialog.c = modal file picker
- [x] File picker, color picker, date picker
      > File picker: gui/dialog.c Open/Save dialog
      > Color picker: dialog_color_picker() HSV gradient + hue bar + hex input
      > Date picker: pending
- [x] Estados: normal, hover, focus, active, disabled
      > 6 state flags (WID_ENABLED, WID_VISIBLE, WID_FOCUSED, WID_HOVERED, WID_PRESSED, WID_CHECKED)
      > Visual feedback en draw: hover highlight, pressed darken, disabled dim
- [ ] Cada componente: error state, keyboard nav completa, accesibilidad
- [x] Migración real a widget kit
      > Settings Fondo page: wid_dropdown (scale mode), wid_switch (blur),
      > wid_button (Apply/Predeterminado) — 4 widgets, visibility toggle
      > Calculator: 20 wid_button widgets

### 17. Icon System
- [ ] Grid base
- [ ] Grosor consistente
- [ ] Metáfora clara
- [ ] Símbolos del sistema unificados
- [ ] Iconos de app adaptables
- [ ] Múltiples tamaños
- [ ] Icon theme oficial

### 18. Sonido e Haptics
- [ ] Sonidos discretos del sistema
- [ ] Feedback visual (volumen, brillo, etc.)
- [ ] OSD limpio
- [ ] No abusar de sonidos

## FASE 6 — Pulido Premium

### 19. Detalles Visuales
- [ ] Sombras creíbles (dirección y opacidad)
- [ ] Separación visual entre capas
- [ ] Contraste excelente
- [ ] Fondos no planos sin intención
- [ ] Transparencias legibles
- [ ] Glass/blur estratégico
- [ ] Esquinas y paddings consistentes
- [ ] Estados vacíos bonitos
- [ ] Placeholders elegantes
- [ ] Skeleton loading (si aplica)

### 20. Coherencia Absoluta
- [ ] Mismo radio en todo
- [ ] Mismos tiempos de animación por categoría
- [ ] Misma jerarquía tipográfica
- [ ] Mismo lenguaje de sombras
- [ ] Mismos espaciados
- [ ] Mismas reglas de foco
- [ ] Mismos patrones de interacción

### 21. Temas
- [x] Tema claro
      > Catppuccin Latte palette: base=0xEFF1F5, text=0x4C4F69, surface0=0xCCD0DA
      > apply_light() en gui/theme.c, activado con theme_set_light()
- [x] Tema oscuro
      > Catppuccin Mocha palette: base=0x1E1E2E, text=0xCDD6F4, surface0=0x313244
      > apply_dark() en gui/theme.c, theme por defecto al iniciar
- [x] Acentos configurables
      > 8 colores: Cyan, Blue, Pink, Green, Yellow, Red, Mauve, Peach
      > theme_set_accent() API, selección visual en Settings > Apariencia
      > Swatch grid con borde blanco en seleccionado
- [ ] Adaptación de wallpaper a accent (opcional)
- [ ] Contraste garantizado
- [ ] Sombras/blur consistentes entre temas

## FASE 7 — Apps del Sistema

### 22. Apps Esenciales
- [x] Explorador de archivos
      > Nexus (gui/apps/filemanager.c): navegación, crear, renombrar, borrar archivos
- [x] Terminal
      > gui/apps/terminal.c: grid de caracteres, scrolling, shell embebido
- [x] Configuración
      > gui/apps/settings.c: 8 secciones (Apariencia, Fondo, Pantalla, Sonido, Red, etc.)
- [x] Visor de imágenes
      > gui/apps/viewer.c: BMP 24-bit y texto plano
- [x] Editor de texto
      > gui/apps/editor.c (Lyth Notes): open/edit/save via VFS, Ctrl shortcuts
- [ ] Navegador básico
- [x] Gestor de tareas/monitor
      > gui/apps/taskman.c: procesos, PID, estado, prioridad, kill/force-close
- [x] Centro de notificaciones
      > notify_paint() en compositor, auto-dismiss 4s, accent bar
- [ ] App de actualizaciones
- [x] Login/lock screen
      > gui/login.c: sky background, reloj grande, avatar, campo password

### 23. Pantallas Clave
- [x] Boot splash
      > Splash PNG embebido (include/gui/splash_png.h), decodificado y escalado en splash.c
- [x] Login
      > gui/login.c: pantalla de login con fondo, reloj, avatar, password
- [x] Desktop inicial
      > Wallpaper + dock + taskbar + launcher, watermark "Lyth OS"
- [x] Alt-tab
      > Overlay visual con thumbnails, cycle + confirm, backdrop dimmed
- [x] Notificaciones
      > notify_paint(), rounded rect, sombra, accent bar, auto-dismiss
- [x] Configuración
      > Settings app con 8 secciones, widgets, live preview
- [x] Apagado/reinicio
      > Power dialog modal con scrim (alpha 120), 3 botones: Cancelar/Reiniciar/Apagar
      > Click fuera = cancelar, launcher Shutdown/Restart abren diálogo
      > open_power_dialog(), draw_power_dialog(), power_dialog_handle_click()
- [x] Errores del sistema
      > dialog_msgbox(type, title, message) en gui/dialog.c
      > MSGBOX_INFO (azul), MSGBOX_WARNING (amarillo), MSGBOX_ERROR (rojo)
      > Word-wrapped text, OK button, Enter/Escape dismiss, animated close
- [ ] Actualizaciones
- [x] File picker
      > gui/dialog.c: Open/Save dialog con navegación directorios, filename entry

## FASE 8 — Rendimiento

### 24. Objetivos Medibles
- [x] Arrastre de ventanas pequeñas sin jank
      > Fast drag path con VBlank sync, DRAG_SHADOW_EXT=4 reducido
      > bg_buffer cache evita recomposición completa durante drag
- [x] Arrastre de ventanas grandes estable
      > Union rect cubre old+new position, shadow extent mínimo durante drag
      > Single shadow layer durante drag (vs 3 capas normal)
- [x] Resize fluido
      > Live resize con gui_window_resize(), dirty rects old+new+shadow
- [x] Launcher instantáneo
      > Launcher cache pre-renderizado, slide animation 6 frames
- [x] Alt-tab instantáneo
      > Alt-Tab overlay aparece instantáneamente, thumbnails pre-escalados
- [x] Notificaciones sin stutter
      > notify_paint() en compose_region, auto-dismiss 4s sin impacto frame
- [ ] Blur sin matar el sistema
- [ ] VRAM razonable
- [ ] CPU idle razonable
- [x] Render escalable a altas resoluciones
      > Hot resolution change, DRAG_SHADOW_EXT reducido, dirty rect compositing
      > Probado hasta 1920x1080 fluido en QEMU y hardware real

### 25. Telemetría Interna
- [x] Overlay debug
      > F3 toggle: FPS, avg/max frametime, dirty rects, resolución
      > paint_perf_overlay() en compose_region paso 6
- [x] Frame time graph
      > Bar graph 60 samples en perf overlay (F3), color-coded:
      > Verde ≤16.7ms, amarillo 16.7-33ms, rojo >33ms
      > Línea de referencia 60fps (16.7ms) translucent green
      > Ring buffer ft_hist[60] actualizado cada frame
- [x] Damage debug
      > F4 toggle: dibuja bordes coloreados (6 colores cíclicos, alpha 180)
      > Muestra cada dirty rect como rectángulo 2px outline
      > Auto-limpieza: se borra en siguiente frame al recomponer
- [x] Redraw debug
      > Frame time graph muestra visualmente cuándo hay redraws pesados
      > Dirty count en perf overlay indica número de regiones por frame
- [ ] GPU upload debug
- [x] Timing por fase
      > Rnd (render_us), Cmp (compose_us), Prs (present_us) en perf overlay F3
      > Color dim verde para distinguir de métricas principales
- [x] Contador de frames caídos
      > dropped_frames: incrementa cuando frame_time > 16667µs
      > Mostrado en línea FPS del perf overlay ("d:N")
- [x] Invalidaciones por ventana
      > redraw_count per-window (cumulative), windows_redrawn per-frame en gui_metrics_t
      > "Win:" line en perf overlay F3 muestra ventanas repintadas por frame
      > compose_frame() phase 1 trackea y contabiliza redraws
- [x] Visualizador de regiones dirty
      > F4 toggle: bordes coloreados (6 colores, alpha 180) sobre cada dirty rect

## FASE 9 — Identidad

### 26. Rasgos Distintivos
- [ ] Decidir 3–5 rasgos propios de Lyth

### 27. Evitar Ser Copia Barata
- [ ] No mezclar estilos sin criterio
- [ ] No usar efectos por moda
- [ ] Bordes, radios y sombras consistentes
- [ ] No sobrecargar de blur
- [ ] Iconos de estilo unificado
- [ ] No dejar pantallas sin diseñar
