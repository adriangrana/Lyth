# Lyth OS - Documentación técnica

## Objetivo

Lyth OS es un kernel hobby para x86 de 32 bits escrito en C y ASM. El proyecto busca servir como base de aprendizaje para entender el arranque con GRUB/Multiboot, la gestión de memoria, interrupciones, multitarea, entrada por teclado/ratón, consola framebuffer y un shell interactivo.

## Arquitectura general

- Boot y entrada temprana en `arch/x86/boot/boot.s`.
- GDT/TSS y segmentos en `kernel/gdt.c` y `arch/x86/gdt.s`.
- IDT, PIC, timer, teclado, ratón y syscall gateway en `kernel/interrupts.c`.
- Consola lógica y renderers en `drivers/console/`.
- Entrada unificada en `drivers/input/`.
- Scheduler y tareas en `kernel/task/`.
- Memoria física y paginación en `kernel/mem/`.
- Shell interactiva en `userland/shell/`.
- Filesystem en memoria en `fs/fs.c`.
- Syscalls en `kernel/syscall.c`.
- Carga de ELF y user mode en `kernel/elf.c` y `kernel/usermode.c`.

## Flujo de arranque

1. GRUB carga el kernel usando Multiboot.
2. `kernel_main()` inicializa GDT, consola, memoria física, heap, FS y scheduler.
3. `interrupts_init()` instala la IDT, remapea el PIC, configura el PIT y habilita IRQs.
4. Se inicializan teclado y ratón PS/2.
5. La shell toma el control y procesa eventos de entrada.
6. El loop principal consume eventos y cede CPU con `hlt` cuando no hay trabajo.

## Consola y vídeo

La consola lógica mantiene un buffer independiente del backend. El renderer activo puede ser VGA o framebuffer.

- `terminal.c` administra cursor, color, overwrite mode y escritura de texto.
- `console_backend.c` adapta la salida a VGA o framebuffer.
- `fbconsole.c` renderiza texto bitmap PSF, cursor de ratón y primitivas gráficas.

El framebuffer se usa si GRUB entrega un modo gráfico compatible; si no, el sistema cae al backend VGA.

## Entrada

La entrada está normalizada en una capa genérica para no acoplar la shell a dispositivos concretos.

- `keyboard.c` traduce scancodes set 1/set 2, layouts `us`/`es`, `AltGr`, `Caps Lock`, `Num Lock` y teclas extendidas.
- `mouse.c` decodifica paquetes PS/2 de 3 bytes y mantiene estado acumulado.
- `input.c` convierte eventos de teclado y ratón en `input_event_t`.

## Memoria

### Memoria física

`physmem.c` construye un bitmap de frames a partir del mapa Multiboot y permite reservar/liberar regiones físicas.

### Paginación

`paging.c` instala identity mapping inicial con páginas grandes y expone helpers para:

- validar buffers de usuario,
- crear directorios de páginas de usuario,
- cambiar `CR3` al cambiar de tarea.

### Heap

`heap.c` gestiona el heap del kernel para asignaciones dinámicas internas.

## Scheduler y tareas

`task.c` implementa un scheduler preemptivo básico guiado por el PIT.

Capacidades principales:

- prioridades `HIGH`, `NORMAL`, `LOW`,
- foreground/background,
- `sleep`, `yield`, bloqueo por eventos,
- soporte de tareas en user mode.

## Syscalls

`syscall.c` expone un dispatcher por `int 0x80` y wrappers para:

- escritura de texto,
- ticks y sleep,
- yield,
- alloc/free,
- consultas del FS,
- exit,
- lectura de nombres de archivo por copia segura a buffer de usuario.

## Shell

`shell.c` y `shell_input.c` gestionan la shell interactiva y el editor de línea.

Incluye:

- historial,
- autocompletado,
- selección,
- clipboard,
- `Insert` como overwrite mode,
- `Delete`, `Home`, `End`, `Page Up`, `Page Down`,
- soporte de variables, pipes y redirecciones.

## Filesystem

`fs.c` implementa un FS en memoria de solo lectura con soporte adicional para entradas escribibles en overlay.

## User mode

`usermode.c` carga imágenes ELF32 i386 desde el FS, prepara una región virtual de usuario y crea tareas ring 3 con su propio directorio de páginas.

## Build y verificación

Targets útiles:

- `make compile`: compila kernel y objetos.
- `make create-iso`: genera la ISO booteable.
- `make test`: compila, genera ISO y valida que el kernel siga siendo multiboot.
- `make run`: flujo completo de limpieza, compilación, ISO y ejecución.

## Estado actual

El proyecto ya tiene:

- arranque Multiboot,
- consola framebuffer/VGA,
- teclado y ratón PS/2,
- shell interactiva,
- scheduler preemptivo,
- memoria física + paginación,
- syscalls,
- FS en memoria,
- ejecución de ELF en user mode,
- logs y diagnóstico visibles.
