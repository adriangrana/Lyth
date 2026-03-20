# Lyth OS

Kernel hobby para x86 de 32 bits escrito en C + ASM, arrancado con GRUB mediante Multiboot y empaquetado como ISO para ejecutar en QEMU.

El proyecto ya no está limitado a texto VGA puro: actualmente intenta arrancar en modo gráfico framebuffer y dibuja una consola propia sobre píxeles, con fallback implícito a la ruta clásica si el framebuffer no está disponible.

## Estado actual

- Arranque Multiboot válido con GRUB.
- Bucle principal del kernel con teclado por eventos + scheduler preemptivo básico.
- IDT, PIC remapeado, IRQ de timer y teclado.
- PIT a 100 Hz con contador de ticks y conversión a milisegundos.
- Scheduler preemptivo básico por timer con tareas foreground/background, `sleep`, `yield` y cancelación.
- Shell con parser estilo `argv`, comillas, historial, autocompletado y jobs.
- Heap simple del kernel (`kmalloc`/`kfree`).
- Gestor de memoria física basado en bitmap a partir del mapa Multiboot.
- Paginación inicial con identity mapping usando páginas grandes de 4 MiB.
- Syscalls mínimas sobre `int 0x80`.
- Filesystem en memoria de solo lectura.
- Consola con backends VGA/framebuffer desacoplados, buffer de pantalla propio y fuente PSF 8x16.

## Estructura del repositorio

- `arch/x86/boot`: arranque Multiboot, `grub.cfg` y ASM temprano.
- `arch/x86`: piezas específicas de x86 como stubs de interrupción y linker script.
- `kernel`: init, IDT, interrupciones, syscalls y subsistemas internos.
- `kernel/mem`: heap, memoria física y paginación.
- `kernel/task`: scheduler y PIT.
- `drivers/console`: terminal lógica y backends VGA/framebuffer.
- `drivers/input`: teclado.
- `userland/shell`: shell, editor de línea y parser.
- `fs`: filesystem en memoria.
- `lib`: utilidades base como cadenas.
- `include`: headers organizados con la misma lógica por subsistemas.

## Flujo de arranque

1. `arch/x86/boot/boot.s` define el header Multiboot y solicita un modo gráfico preferido de `1280x1024x32`.
2. GRUB carga el kernel y pasa el puntero a la estructura Multiboot en `EBX`.
3. `_start` crea la pila inicial y llama a `kernel_main()`.
4. `kernel/kernel.c` inicializa terminal, heap, FS, scheduler, entrada de shell y framebuffer.
5. `interrupts_init()` crea la IDT, remapea el PIC, configura el PIT y habilita interrupciones.
6. El loop principal consume eventos de teclado, actualiza la consola y hace `hlt`; el PIT decide cuándo preemptar tareas.

## Vídeo y consola

La parte visual ha cambiado bastante respecto al estado inicial:

- `arch/x86/boot/grub.cfg` pide `gfxmode=1280x1024x32,1024x768x32,auto` y conserva el modo con `gfxpayload=keep`.
- `drivers/console/fbconsole.c` usa el framebuffer expuesto por GRUB si el flag correspondiente de Multiboot está presente.
- La consola actual renderiza una fuente PSF bitmap `8x16` sin blur ni escalado fraccional.
- El framebuffer aplica padding para dejar márgenes visuales alrededor del área de texto.
- `drivers/console/terminal.c` mantiene un buffer de celdas independiente del renderer y delega en un backend activo.
- El cursor en framebuffer es un overlay parpadeante que no borra el texto subyacente; en VGA se usa el cursor hardware.

## Arquitectura por módulos

- `arch/x86/boot/boot.s`: header Multiboot, petición de modo gráfico, stack inicial y salto a `kernel_main()`.
- `arch/x86/linker.ld`: enlaza el kernel ELF32 a partir de `1M`.
- `kernel/kernel.c`: secuencia de init y bucle principal del kernel.
- `drivers/console/fbconsole.c`, `include/drivers/console/fbconsole.h`: acceso al framebuffer, padding, scroll y render bitmap.
- `drivers/console/terminal.c`, `include/drivers/console/terminal.h`: buffer lógico de pantalla, cursor y API común de texto.
- `drivers/console/console_backend.c`, `include/drivers/console/console_backend.h`: selección y adaptación del backend VGA/framebuffer.
- `drivers/input/keyboard.c`, `include/drivers/input/keyboard.h`: lectura de scancodes, cola de eventos y traducción de teclas.
- `kernel/idt.c`, `include/kernel/idt.h`: estructuras e instalación de la IDT.
- `kernel/interrupts.c`, `include/kernel/interrupts.h`, `arch/x86/interrupts.s`: PIC, IRQ0/IRQ1, `int 0x80` y stubs ASM.
- `kernel/task/timer.c`, `include/kernel/task/timer.h`: programación del PIT, ticks y uptime.
- `kernel/task/task.c`, `include/kernel/task/task.h`: scheduler y gestión de tareas.
- `kernel/mem/physmem.c`, `include/kernel/mem/physmem.h`: frame allocator físico a partir del mapa de memoria.
- `kernel/mem/paging.c`, `include/kernel/mem/paging.h`: activación de paginación e identity mapping inicial.
- `userland/shell/shell_input.c`, `include/userland/shell/shell_input.h`: editor de línea, historial, selección, clipboard y prompt.
- `userland/shell/shell.c`, `include/userland/shell/shell.h`: comandos, jobs y coordinación con la shell interactiva.
- `userland/shell/parser.c`, `include/userland/shell/parser.h`: parseo de línea y enteros.
- `kernel/mem/heap.c`, `include/kernel/mem/heap.h`: heap del kernel.
- `kernel/syscall.c`, `include/kernel/syscall.h`: dispatcher de syscalls y wrappers de invocación.
- `fs/fs.c`, `include/fs/fs.h`: filesystem estático en memoria.
- `lib/string.c`, `include/lib/string.h`: helpers de cadenas.
- `include/multiboot.h`: subset de la estructura Multiboot usado por el kernel.
- `include/font_psf.h`: fuente PSF generada para la consola framebuffer.

## Scheduler y tareas

- El planificador actual usa preempción básica guiada por el PIT:

- Máximo de 8 tareas (`TASK_MAX_COUNT`).
- Estados principales: libre, lista, corriendo, dormida, finalizada, cancelada.
- Cada tarea corre sobre su propio stack de kernel.
- El PIT puede interrumpir la tarea activa y devolver el control al loop principal.
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

- `make compile`: compila y enlaza el kernel en `build/kernel.bin`.
- `make create-iso`: genera la imagen en `dist/lyth.iso`.
- `make execute`: arranca `dist/lyth.iso` en QEMU.
- `make debug`: arranca QEMU con `-d int`.
- `make run`: limpia, compila, genera ISO y ejecuta.
- `make clean`: borra `build/`, `dist/` y artefactos generados.

## Observaciones del estado actual

- El framebuffer usa fuente PSF 8x16 con márgenes y cursor overlay propio.
- La resolución preferida sigue siendo alta, pero el área útil de consola ahora se calcula respetando padding.
- El scheduler ya hace cambio de contexto básico entre stacks de kernel, pero aún no hay prioridades ni bloqueo por eventos.
- La paginación actual es solo identity mapping global del kernel; aún no hay espacios virtuales por proceso.
- El link final aún avisa de un segmento `RWX`, algo aceptable por ahora pero pendiente de endurecer.

## Archivos clave para retomar el proyecto

Si vuelves a tocar el proyecto, los archivos más importantes ahora son:

- `arch/x86/boot/boot.s`
- `arch/x86/boot/grub.cfg`
- `kernel/kernel.c`
- `drivers/console/fbconsole.c`
- `drivers/console/terminal.c`
- `userland/shell/shell_input.c`
- `userland/shell/shell.c`
- `kernel/task/task.c`
- `drivers/input/keyboard.c`
- `kernel/interrupts.c`
- `Makefile`

## Próximos pasos razonables

- Añadir page fault handler útil y excepciones CPU más completas.
- Introducir prioridades y bloqueo por eventos en el scheduler.
- Preparar espacio virtual por proceso sobre la base de paging actual.
- Separar más claramente kernel interno y syscall ABI.
- Implementar modo usuario y un ELF loader simple.
- Evolucionar el FS a algo menos estático.
