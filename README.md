# Lyth OS

Kernel hobby para x86 de 32 bits escrito en C + ASM, arrancado con GRUB mediante Multiboot y empaquetado como ISO para ejecutar en QEMU.

El proyecto ya no está limitado a texto VGA puro: actualmente intenta arrancar en modo gráfico framebuffer y dibuja una consola propia sobre píxeles, con fallback implícito a la ruta clásica si el framebuffer no está disponible.

## Estado actual

- Arranque Multiboot válido con GRUB.
- Bucle principal del kernel con teclado por eventos + scheduler cooperativo.
- IDT, PIC remapeado, IRQ de timer y teclado.
- PIT a 100 Hz con contador de ticks y conversión a milisegundos.
- Scheduler cooperativo con tareas foreground/background, `sleep`, `yield` y cancelación.
- Shell con parser estilo `argv`, comillas, historial, autocompletado y jobs.
- Heap simple del kernel (`kmalloc`/`kfree`).
- Syscalls mínimas sobre `int 0x80`.
- Filesystem en memoria de solo lectura.
- Consola framebuffer con fuente bitmap 8x8 escalada enteramente a 2x para texto más limpio en fullscreen.

## Flujo de arranque

1. `boot.s` define el header Multiboot y solicita un modo gráfico preferido de `1280x1024x32`.
2. GRUB carga el kernel y pasa el puntero a la estructura Multiboot en `EBX`.
3. `_start` crea la pila inicial y llama a `kernel_main()`.
4. `kernel_main()` inicializa terminal, heap, FS, scheduler, entrada de shell y framebuffer.
5. `interrupts_init()` crea la IDT, remapea el PIC, configura el PIT y habilita interrupciones.
6. El loop principal consume eventos de teclado, ejecuta una tarea lista y hace `hlt` cuando no hay trabajo runnable.

## Vídeo y consola

La parte visual ha cambiado bastante respecto al estado inicial:

- `grub.cfg` pide `gfxmode=1280x1024x32,1024x768x32,auto` y conserva el modo con `gfxpayload=keep`.
- `fbconsole.c` usa el framebuffer expuesto por GRUB si el flag correspondiente de Multiboot está presente.
- La consola actual no usa suavizado: renderiza una fuente bitmap `8x8` con escalado entero `2x`.
- El tamaño efectivo de celda es `16x16`, lo que mejora legibilidad sin introducir blur.
- `terminal.c` abstrae la salida y el scroll, enviando texto al framebuffer cuando está activo.
- El cursor hardware VGA se desactiva de facto en framebuffer y sigue funcionando en fallback VGA.

## Arquitectura por módulos

- `boot.s`: header Multiboot, petición de modo gráfico, stack inicial y salto a `kernel_main()`.
- `linker.ld`: enlaza el kernel ELF32 a partir de `1M`.
- `kernel.c`: secuencia de init y bucle principal del kernel.
- `fbconsole.c`, `fbconsole.h`: acceso al framebuffer, limpieza, scroll y render de caracteres bitmap escalados.
- `terminal.c`, `terminal.h`: API de salida de texto común para VGA/framebuffer.
- `keyboard.c`, `keyboard.h`: lectura de scancodes, cola de eventos y traducción de teclas.
- `idt.c`, `idt.h`: estructuras e instalación de la IDT.
- `interrupts.c`, `interrupts.h`, `interrupts.s`: PIC, IRQ0/IRQ1, `int 0x80` y stubs ASM.
- `timer.c`, `timer.h`: programación del PIT, ticks y uptime.
- `task.c`, `task.h`: scheduler cooperativo y gestión de tareas.
- `shell_input.c`, `shell_input.h`: editor de línea, historial, selección, clipboard y prompt.
- `shell.c`, `shell.h`: comandos, jobs y coordinación con la shell interactiva.
- `parser.c`, `parser.h`: parseo de línea y enteros.
- `heap.c`, `heap.h`: heap del kernel.
- `syscall.c`, `syscall.h`: dispatcher de syscalls y wrappers de invocación.
- `fs.c`, `fs.h`: filesystem estático en memoria.
- `string.c`, `string.h`: helpers de cadenas.
- `include/multiboot.h`: subset de la estructura Multiboot usado por el kernel.
- `include/font8x8_basic.h`: fuente bitmap base usada por la consola framebuffer.

## Scheduler y tareas

El planificador actual es cooperativo, no preemptivo:

- Máximo de 8 tareas (`TASK_MAX_COUNT`).
- Estados principales: libre, lista, corriendo, dormida, finalizada, cancelada.
- Cada tarea ejecuta un `step` por turno.
- `task_sleep()` duerme por ticks del PIT.
- `task_yield()` cede CPU explícitamente.
- `Ctrl+C` marca cancelación para la tarea foreground actual.
- Existe callback para reactivar el prompt cuando termina una tarea foreground.

## Shell disponible

Comandos implementados ahora mismo:

- `help`: lista comandos.
- `clear` / `cls`: limpia la pantalla.
- `about`: muestra versión y resumen del sistema.
- `echo <texto>`: imprime texto.
- `color <white|green|red|blue>`: cambia el color del texto.
- `history`: imprime el historial del editor de línea.
- `count [n] [&]`: demo por pasos cancelable.
- `sleep <ms> [&]`: duerme una tarea en foreground o background.
- `uptime`: muestra ticks y milisegundos desde arranque.
- `ps` / `jobs`: lista tareas activas.
- `kill <id>`: cancela una tarea.
- `task`: muestra la tarea actual y el foreground.
- `mem`: muestra estadísticas del heap.
- `ls`: lista archivos del FS en memoria.
- `cat <NOMBRE>`: lee un archivo del FS.
- `yield`: cede CPU al scheduler.

## Entrada interactiva

La shell tiene más edición de la que parece a simple vista:

- Navegación de historial.
- Movimiento horizontal del cursor.
- Selección con `Shift` + flechas.
- `Tab` para autocompletar comandos.
- `Ctrl+C` para cancelación o interrupción.
- `Ctrl+L` para limpiar pantalla.
- `Ctrl+U` para borrar línea.
- `Ctrl+A` para seleccionar todo.
- `Ctrl+X` para cortar línea o selección.
- `Ctrl+V` y `Shift+Insert` para pegar.
- `Ctrl+C` / `Ctrl+Shift+C` para copiar según contexto.

## Teclado e interrupciones

- IRQ0 -> vector `32`.
- IRQ1 -> vector `33`.
- Syscalls -> vector `0x80`.
- El driver de teclado intenta adaptarse a scancode set 1 y set 2.
- La entrada no escribe directamente en pantalla: genera `keyboard_event_t` y los entrega al editor de línea.

## Syscalls actuales

El dispatcher de `syscall.c` soporta:

- escritura de texto,
- lectura de ticks,
- sleep,
- yield,
- alloc/free,
- consulta de número de archivos,
- nombre de archivo por índice,
- tamaño de archivo,
- lectura de archivo.

## Filesystem en memoria

Archivos actuales del FS:

- `README.TXT`
- `MOTD.TXT`
- `VERSION.TXT`

Es un FS de solo lectura pensado para pruebas internas de shell y syscalls.

## Build y ejecución

Requisitos habituales:

- `gcc` con soporte `-m32`
- `as`, `ld`
- `grub-mkrescue`
- `qemu-system-i386`

Comandos principales:

- `make compile`: compila y enlaza `kernel.bin`.
- `make create-iso`: genera `lyth.iso`.
- `make execute`: arranca la ISO en QEMU.
- `make debug`: arranca QEMU con `-d int`.
- `make run`: limpia, compila, genera ISO y ejecuta.
- `make clean`: borra objetos, binario e ISO.

## Observaciones del estado actual

- El framebuffer actual usa escalado entero 2x, no suavizado subpíxel.
- La resolución preferida es alta para mejorar fullscreen, pero la calidad del texto sigue limitada por la fuente bitmap base.
- El scheduler sigue siendo cooperativo: no hay cambio de contexto real ni multitarea preemptiva.
- El kernel continúa en un único espacio de direcciones, sin paginación ni modo usuario.
- El link final aún avisa de un segmento `RWX`, algo aceptable por ahora pero pendiente de endurecer.

## Archivos clave para retomar el proyecto

Si vuelves a tocar el proyecto, los archivos más importantes ahora son:

- `boot.s`
- `grub.cfg`
- `kernel.c`
- `fbconsole.c`
- `terminal.c`
- `shell_input.c`
- `shell.c`
- `task.c`
- `keyboard.c`
- `interrupts.c`
- `Makefile`

## Próximos pasos razonables

- Usar una fuente 8x16 real en vez de escalar la 8x8.
- Añadir diagnóstico visible de resolución/framebuffer al arranque.
- Implementar multitarea preemptiva.
- Añadir paginación y memoria física.
- Separar más claramente kernel interno y syscall ABI.
- Evolucionar el FS a algo menos estático.
