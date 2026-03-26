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
- [ ] Tamaños tipográficos (caption, body, body strong, title, heading)
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
- [ ] Gestión de iconos

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
- [ ] persistencia en archivo de configuración
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
- [ ] Previews de ventanas
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
      > Alt+Tab cicla focus entre ventanas visibles en compositor.c
      > Dirty rects para ventana anterior y siguiente
- [ ] Exposé/overview (futura)

## FASE 3 — Compositor GPU

### 10. Render Backend GPU
- [ ] API de renderer diseñada
- [ ] Backend CPU funcional
- [ ] Backend GPU inicial
- [ ] Surfaces como texturas
- [ ] Quads por ventana/layer
- [ ] Clipping por nodo
- [ ] Alpha por nodo
- [ ] Transform por nodo
- [ ] Scissor/clip regions
- [ ] Render targets intermedios
- [ ] Upload eficiente de surfaces dañadas
- [ ] Sincronización render/present

### 11. Scene Graph/Layers
- [ ] Nodos: ventana, sombra, decoración, popup, tooltip, panel, cursor, overlay
- [ ] Propiedades: bounds, opacity, transform, clip, z-index, visibility, damage

### 12. Efectos Modernos
- [ ] Alpha real por ventana
- [ ] Fade in/out
- [ ] Escalado suave
- [ ] Blur de fondo
- [ ] Sombras gaussianas
- [ ] Esquinas redondeadas recortadas
- [ ] Vibrancy/acrylic-like (opcional)
- [ ] Dim de fondo para modales
- [ ] Overview mode
- [ ] Animaciones minimize/maximize
- [ ] Transiciones de workspace
- [ ] Crossfades entre estados

## FASE 4 — Calidad de Interacción

### 13. Input Impecable
- [ ] Mouse sampling estable
- [ ] Coalescing correcto
- [ ] Sin input lag visible
- [ ] Arrastre subpixel fino
- [ ] Hit testing consistente
- [x] Doble click fiable
      > DBLCLICK_MS=400, DBLCLICK_DIST=6 in compositor.c
      > Timestamp + position tracking, resets on triple-click
      > gui_dblclick_fn callback (on_dblclick) in gui_window_t
- [ ] Wheel suave
- [ ] Touchpad support (futura)
- [ ] Gestos multitáctiles (futura)

### 14. Animación con Criterio
- [ ] Duraciones definidas (microanimación, transición normal, modal, workspace)
- [ ] Curvas easing coherentes
- [ ] Animaciones interrumpibles
- [ ] Evitar stutter
- [ ] Animar para comunicar estado, no por decorar

### 15. Latencia y Consistencia
- [ ] Input-to-photon bajo
- [ ] Frames estables
- [ ] Sin congelones en: abrir/cerrar ventana, arrastrar, resize, cambiar foco, menús, hover
- [ ] Percentiles de frametime medidos
- [ ] Perf overlay interno

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
- [ ] Scrollbar (standalone)
- [ ] Listas, tablas, tabs, sidebars
- [ ] Modales, toasts, menús contextuales, dropdowns
- [ ] File picker, color picker, date picker
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
- [ ] Tema claro
- [ ] Tema oscuro
- [ ] Acentos configurables
- [ ] Adaptación de wallpaper a accent (opcional)
- [ ] Contraste garantizado
- [ ] Sombras/blur consistentes entre temas

## FASE 7 — Apps del Sistema

### 22. Apps Esenciales
- [ ] Explorador de archivos
- [ ] Terminal
- [ ] Configuración
- [ ] Visor de imágenes
- [ ] Editor de texto
- [ ] Navegador básico
- [ ] Gestor de tareas/monitor
- [ ] Centro de notificaciones
- [ ] App de actualizaciones
- [ ] Login/lock screen

### 23. Pantallas Clave
- [x] Boot splash
      > Splash PNG embebido (include/gui/splash_png.h), decodificado y escalado en splash.c
- [ ] Login
- [ ] Desktop inicial
- [ ] Alt-tab
- [ ] Notificaciones
- [ ] Configuración
- [ ] Apagado/reinicio
- [ ] Errores del sistema
- [ ] Actualizaciones
- [ ] File picker

## FASE 8 — Rendimiento

### 24. Objetivos Medibles
- [ ] Arrastre de ventanas pequeñas sin jank
- [ ] Arrastre de ventanas grandes estable
- [ ] Resize fluido
- [ ] Launcher instantáneo
- [ ] Alt-tab instantáneo
- [ ] Notificaciones sin stutter
- [ ] Blur sin matar el sistema
- [ ] VRAM razonable
- [ ] CPU idle razonable
- [ ] Render escalable a altas resoluciones

### 25. Telemetría Interna
- [ ] Overlay debug
- [ ] Frame time graph
- [ ] Damage debug
- [ ] Redraw debug
- [ ] GPU upload debug
- [ ] Timing por fase
- [ ] Contador de frames caídos
- [ ] Invalidaciones por ventana
- [ ] Visualizador de regiones dirty

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
