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
- [ ] Iconografía coherente
- [x] Grosor de bordes
- [ ] Comportamiento hover/focus/pressed/disabled

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
- [ ] Sombras suaves y creíbles
- [ ] Esquinas redondeadas bien recortadas
- [ ] Borde sutil o separación visual clara
- [x] Hit testing preciso
      > hit_close_button() actualizado para botón izquierdo
- [ ] Resize handles cómodos
- [ ] Snap visual bonito
- [ ] Maximize/minimize con animación
- [ ] Raise-to-front instantáneo
- [ ] Drag sin freezes
- [ ] Resize fluido
- [ ] Resize preview o live resize optimizado
- [ ] Focus inmediato
- [ ] No flicker
- [ ] No saltos en drag

### 6. Desktop y Wallpaper
- [x] Wallpaper engine
      > PNG decoder completo (RFC 1951 inflate + zlib + PNG filters) en lib/png.c
      > Wallpaper embebido como C array (tools/bin2h.py → include/gui/wallpaper_png.h)
      > physmem_alloc_region para buffers grandes (~12MB pico durante decode)
- [x] Escalado correcto
      > Cover-mode: nearest-neighbor scale + center-crop en rebuild_desktop()
- [ ] Blur opcional
- [ ] Overlay sutil para legibilidad
- [ ] Wallpapers dinámicos (futura)
- [ ] Desktop vacío elegante
- [ ] Gestión de iconos

#### 6.1. Interacción del escritorio
- [ ] detectar click derecho sobre escritorio
- [ ] distinguir escritorio vs ventana vs panel
- [ ] menú contextual del escritorio
- [ ] acciones del menú
- [ ] cierre del menú al clicar fuera
- [ ] hover/selección por ratón
- [ ] navegación por teclado más adelante

#### 6.2. Configuración del sistema
- [ ] ventana/app de Configuración
- [ ] sección “Pantalla”
- [ ] sección “Fondo de pantalla”
- [ ] sección “Apariencia”
- [ ] persistencia en archivo de configuración
- [ ] aplicar cambios en caliente

### 7. Panel/Barra Superior
- [x] Layout definido
      > Taskbar translúcido (alpha_blend_fill 190/255) sobre wallpaper
      > Secciones: left=app labels, center=dock, right=tray con separadores verticales
- [x] Área de apps
      > Running app labels con rounded rect en sección izquierda del taskbar
- [x] Reloj/fecha
      > HH:MM + full date (DayName, Month Day) en zona derecha del tray
- [x] Estado del sistema (red, audio, batería)
      > Network icon (verde/rojo según conexión) + Volume icon en tray
- [x] Usuario/sesión
      > Username obtenido de session_get_current() mostrado en tray
- [ ] Notificaciones
- [x] Menú del sistema
      > Hamburger icon abre launcher; network icon abre popup red
- [x] Blur/transparencia opcional
      > Taskbar glass: alpha_blend_fill + accent line sutil (alpha 30)
      > Separadores verticales semitransparentes entre secciones
- [ ] Auto-hide opcional
- [ ] Animaciones de hover

### 8. Launcher/Menú de Aplicaciones
- [x] Búsqueda instantánea
      > Filtrado en tiempo real con str_starts_with_ignore_case
      > Cursor texto visible, "No results" cuando no hay coincidencias
- [x] Grid o lista impecable
      > Grid 5 columnas filtrado dinámicamente, hover translúcido (alpha 60)
- [x] Navegación por teclado
      > Arrows + Enter + Escape + Backspace, todo respeta filtro de búsqueda
- [ ] Categorías
- [ ] Recientes
- [ ] Apps fijadas
- [ ] Animación de apertura/cierre
- [x] Tipografía perfecta
      > Título "Lyth OS" en color accent, labels centrados bajo iconos
- [x] Iconos de calidad
      > Rounded rect coloreados con letra centrada, coherentes con dock

### 9. Dock o Task Switcher
- [ ] Previews de ventanas
- [x] Indicadores de app abierta
      > Barra horizontal 6x2px debajo del icono (reemplaza dot 3x3)
      > Dock pill translúcido (alpha 160) sobre wallpaper
- [ ] Drag-reorder
- [ ] Pinned apps
- [ ] Animaciones cuidadas
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
- [ ] Doble click fiable
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
- [ ] Botones, inputs, checkbox, radio, switch, slider
- [ ] Listas, tablas, tabs, sidebars
- [ ] Modales, toasts, menús contextuales, dropdowns
- [ ] File picker, color picker, date picker
- [ ] Cada componente: normal, hover, focus, active, disabled, error, keyboard nav, accesibilidad

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
