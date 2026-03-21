# Lyth OS

Kernel hobby para x86 de 32 bits escrito en C + ASM, arrancado con GRUB mediante Multiboot y empaquetado como ISO para ejecutar en QEMU.

El proyecto ya no está limitado a texto VGA puro: actualmente intenta arrancar en modo gráfico framebuffer y dibuja una consola propia sobre píxeles, con fallback implícito a la ruta clásica si el framebuffer no está disponible.

## Descripción del proyecto

Lyth OS es un sistema operativo educativo y experimental para x86 de 32 bits, pensado para explorar desde el arranque con GRUB hasta la multitarea, la gestión de memoria, la entrada por teclado y ratón, y una shell interactiva propia.

El objetivo del proyecto es tener una base pequeña pero completa sobre la que seguir iterando, con foco en entender cómo se conectan los subsistemas clásicos de un kernel: vídeo, interrupciones, scheduler, syscalls, filesystem y user mode.

## Estado actual

- Arranque Multiboot válido con GRUB.
- Bucle principal del kernel con teclado por eventos + scheduler preemptivo básico.
- IDT, PIC remapeado, IRQ de timer y teclado.
- PIT a 100 Hz con contador de ticks y conversión a milisegundos.
- Scheduler preemptivo básico por timer con tareas foreground/background, `sleep`, `yield` y cancelación.
- Scheduler con prioridades `HIGH`/`NORMAL`/`LOW` y selección preferente de tareas listas.
- Shell con parser estilo `argv`, comillas, historial, autocompletado y jobs.
- Shell con variables de entorno básicas y expansión `$VAR`.
- Shell con `keymap` para alternar entre layouts de teclado básicos `us` y `es`.
- Shell con redirección `>`, `>>`, `<` completamente integrada con VFS.
- Tab completion con barra de sugerencias filtrada en línea (filtra mientras se escribe, se borra al confirmar).
- Entrada de teclado con soporte para `AltGr`, `Caps Lock`, `Insert` como modo overwrite, y teclas extendidas como `Delete`, `Home`, `End`, `Page Up` y `Page Down`.
- Driver PS/2 de ratón con IRQ12 y consulta básica desde la shell.
- Soporte PS/2 de ratón con cursor software del guest para framebuffer.
- Temas visuales básicos de shell (`default`, `matrix`, `amber`, `ice`).
- Heap simple del kernel (`kmalloc`/`kfree`).
- Gestor de memoria física basado en bitmap a partir del mapa Multiboot.
- Paginación inicial con identity mapping usando páginas grandes de 4 MiB.
- GDT propia con segmentos kernel/user y TSS cargada para preparar ring 3.
- Manejadores básicos de excepciones CPU y page fault con diagnóstico visible.
- Syscalls sobre `int 0x80` con soporte completo de operaciones VFS desde user mode.
- Syscalls con validación de acceso completa: punteros de usuario validados, máscara de flags en `open`, bounds en `readdir`, y errno en todos los caminos de error; syscalls FS legacy bloqueadas desde user mode.
- Syscalls de procesos en user mode: `getpid`, `kill`, `wait`, `exec`, `fork`, `execv` y `get_errno`.
- Buffer de logs de kernel con niveles DEBUG/INFO/WARN/ERROR, consultable con `dmesg`.
- Primitivas gráficas básicas de framebuffer accesibles desde la shell.
- ABI de syscall más estable para user mode, evitando devolver punteros internos del kernel.
- Espacio virtual de usuario por tarea mediante directorios de página propios y cambio de `CR3` en el scheduler.
- Primer proceso `init` (PID 1) ejecutando el loop interactivo de shell/eventos.
- Recolección de procesos zombie y reasignación de huérfanos hacia `init`.
- Señales completas: entrega, handlers en userland con salto/retorno seguro y preservación de contexto, máscara/bloqueo por proceso, señales no bloqueables (`SIGKILL`), `SIGCHLD` + `wait`/`waitpid`.
- VFS con tabla de montajes, resolución de rutas absolutas y relativas, normalización, cwd y FDs por tarea.
- VFS con permisos tipo UNIX de 9 bits (rwxrwxrwx) almacenados por ruta, con enforcement en `open`, `create`, `delete` y `rename`.
- VFS con `rename`, `stat`, `chmod` y gestión de flags de apertura y modos de acceso.
- ramfs escribible con soporte de directorios virtuales: `mkdir`, `touch`, redirección `>`/`>>`, borrado `rm`.
- Tabla de file descriptors por tarea integrada con VFS, con herencia en `fork` y limpieza segura por referencia.
- Driver ATA PIO con detección automática de unidades master y slave al arranque.
- Capa de dispositivos de bloque (`blkdev`) con particionado MBR y GPT automático (hasta 32 particiones).
- FAT16 read/write: `read`, `readdir`, `write`, montaje automático sobre particiones detectadas.
- FAT32 read/write con soporte LFN (Long File Names).
- Comandos VFS de primer nivel: `ls`, `cat`, `cd`, `pwd`, `touch`, `rm`, `mkdir`, `rename`, `stat`, `chmod`.
- Consola con backends VGA/framebuffer desacoplados, buffer de pantalla propio y fuente PSF 8x16.

## Estructura del repositorio

- `arch/x86/boot`: arranque Multiboot, `grub.cfg` y ASM temprano.
- `arch/x86`: piezas específicas de x86 como stubs de interrupción y linker script.
- `arch/x86/gdt.s`: carga de GDT/TSS y recarga de selectores.
- `kernel`: init, IDT, interrupciones, syscalls y subsistemas internos.
- `kernel/gdt.c`: construcción de GDT plana, segmentos user y TSS.
- `kernel/elf.c`: parser ELF32 i386 mínimo para preparar el loader.
- `kernel/mem`: heap, memoria física y paginación.
- `kernel/task`: scheduler y PIT.
- `drivers/console`: terminal lógica y backends VGA/framebuffer.
- `drivers/input`: teclado, ratón PS/2 y capa de input genérico.
- `drivers/disk`: driver ATA PIO y capa de dispositivos de bloque con particionado MBR y GPT (hasta 32 particiones).
- `userland/shell`: shell, editor de línea y parser.
- `fs`: VFS con permisos UNIX, ramfs escribible, FAT16/FAT32 con LFN y almacenamiento en memoria.
- `docs/TECHNICAL.md`: documentación técnica del kernel, subsistemas y flujo de arranque.
- `lib`: utilidades base como cadenas.
- `include`: headers organizados con la misma lógica por subsistemas.

## Flujo de arranque

1. `arch/x86/boot/boot.s` define el header Multiboot y solicita un modo gráfico preferido de `1280x1024x32`.
2. GRUB carga el kernel y pasa el puntero a la estructura Multiboot en `EBX`.
3. `_start` crea la pila inicial y llama a `kernel_main()`.
4. `kernel/kernel.c` inicializa terminal, heap, VFS (ramfs montado en `/`), scheduler, framebuffer y arranca la tarea `init`.
5. El kernel intenta `ata_init()`, registra unidades ATA como dispositivos de bloque, detecta particiones MBR o GPT automáticamente, y monta las particiones FAT16/FAT32 encontradas (p.ej. en `/sd0p0`).
6. `interrupts_init()` crea la IDT, remapea el PIC, configura el PIT y habilita interrupciones.
7. El loop principal del kernel hace `hlt`; la tarea `init` (PID 1) consume eventos y ejecuta la shell interactiva.

## Vídeo y consola

La parte visual ha cambiado bastante respecto al estado inicial:

- `arch/x86/boot/grub.cfg` pide `gfxmode=1280x1024x32,1024x768x32,auto` y conserva el modo con `gfxpayload=keep`.
- `drivers/console/fbconsole.c` usa el framebuffer expuesto por GRUB si el flag correspondiente de Multiboot está presente.
- La consola actual renderiza una fuente PSF bitmap `8x16` sin blur ni escalado fraccional.
- El framebuffer aplica padding para dejar márgenes visuales alrededor del área de texto.
- `drivers/console/terminal.c` mantiene un buffer de celdas independiente del renderer y delega en un backend activo.
- El cursor en framebuffer es un overlay parpadeante que no borra el texto subyacente; en VGA se usa el cursor hardware.
- El ratón también dibuja un cursor overlay software sobre el framebuffer cuando el backend gráfico está activo.
- Para diagnosticar si un cursor extra viene del host o del guest, puedes recompilar temporalmente sin cursor software del guest con `make run FB_MOUSE_CURSOR=0`.

## Arquitectura por módulos

- `arch/x86/boot/boot.s`: header Multiboot, petición de modo gráfico, stack inicial y salto a `kernel_main()`.
- `arch/x86/linker.ld`: enlaza el kernel ELF32 a partir de `1M`.
- `kernel/kernel.c`: secuencia de init y bucle principal del kernel.
- `drivers/console/fbconsole.c`, `include/drivers/console/fbconsole.h`: acceso al framebuffer, padding, scroll, render bitmap, cursor de ratón y primitivas gráficas.
- `drivers/console/terminal.c`, `include/drivers/console/terminal.h`: buffer lógico de pantalla, cursor y API común de texto.
- `drivers/console/console_backend.c`, `include/drivers/console/console_backend.h`: selección y adaptación del backend VGA/framebuffer.
- `drivers/input/keyboard.c`, `include/drivers/input/keyboard.h`: lectura de scancodes, cola de eventos y traducción de teclas.
- `drivers/input/input.c`, `include/drivers/input/input.h`: abstracción de eventos de entrada para desacoplar consumidores del dispositivo concreto.
- `drivers/input/mouse.c`, `include/drivers/input/mouse.h`: driver PS/2, cola de paquetes y estado acumulado.
- `drivers/disk/ata.c`: driver ATA PIO 28-bit LBA para leer sectores de disco.
- `drivers/disk/blkdev.c`: capa de dispositivos de bloque con soporte de particiones MBR y GPT (`blkdev_probe_gpt`), nombres de partición multi-dígito, hasta 32 particiones por disco.
- `kernel/idt.c`, `include/kernel/idt.h`: estructuras e instalación de la IDT.
- `kernel/interrupts.c`, `include/kernel/interrupts.h`, `arch/x86/interrupts.s`: PIC, IRQ0/IRQ1, `int 0x80` y stubs ASM.
- `kernel/gdt.c`, `include/kernel/gdt.h`, `arch/x86/gdt.s`: GDT propia, segmentos y TSS.
- `kernel/task/timer.c`, `include/kernel/task/timer.h`: programación del PIT, ticks y uptime.
- `kernel/task/task.c`, `include/kernel/task/task.h`: scheduler, gestión de tareas y tabla de FDs por tarea.
- `kernel/mem/physmem.c`, `include/kernel/mem/physmem.h`: frame allocator físico a partir del mapa de memoria.
- `kernel/mem/paging.c`, `include/kernel/mem/paging.h`: activación de paginación e identity mapping inicial.
- `kernel/usermode.c`, `include/kernel/usermode.h`: carga ELF en memoria de usuario y creación de tareas ring 3.
- `userland/shell/shell_input.c`, `include/userland/shell/shell_input.h`: editor de línea, historial, selección, clipboard, prompt y barra de sugerencias de tab completion.
- `userland/shell/shell.c`, `include/userland/shell/shell.h`: comandos, jobs, redirección VFS (`>`/`>>`/`<`), pipes y coordinación con la shell interactiva.
- `userland/shell/parser.c`, `include/userland/shell/parser.h`: parseo de línea y enteros.
- `kernel/mem/heap.c`, `include/kernel/mem/heap.h`: heap del kernel.
- `kernel/syscall.c`, `include/kernel/syscall.h`: dispatcher de syscalls, wrappers de invocación y syscalls VFS completas.
- `kernel/elf.c`, `include/kernel/elf.h`: validación básica de imágenes ELF32 i386.
- `fs/vfs.c`: capa VFS con tabla de montajes, resolución de rutas, FDs por tarea, operaciones de directorio, tabla de permisos UNIX por ruta (256 entradas), enforcement en `open`/`create`/`delete`/`rename`, y APIs `vfs_chmod`/`vfs_get_mode`.
- `fs/ramfs.c`: filesystem en RAM escribible con soporte de directorios virtuales.
- `fs/fat16.c`: driver FAT16 read/write (read, readdir, write, mount sobre blkdev).
- `fs/fat32.c`: driver FAT32 read/write con soporte LFN completo.
- `fs/fs.c`, `include/fs/fs.h`: almacenamiento clave-valor en memoria usado por ramfs.
- `lib/string.c`, `include/lib/string.h`: helpers de cadenas.
- `include/multiboot.h`: subset de la estructura Multiboot usado por el kernel.
- `include/font_psf.h`: fuente PSF generada para la consola framebuffer.

## Scheduler y tareas

- El planificador actual usa preempción básica guiada por el PIT.

- Máximo de 8 tareas (`TASK_MAX_COUNT`).
- Estados principales: libre, lista, corriendo, dormida, bloqueada por evento, zombie, finalizada y cancelada.
- Cada tarea corre sobre su propio stack de kernel.
- Las tareas pueden ejecutarse en foreground o background.
- El scheduler prioriza tareas `HIGH`, `NORMAL` y `LOW`.
- El PIT puede interrumpir la tarea activa y devolver el control al loop principal.
- `task_sleep()` duerme por ticks del PIT.
- `task_yield()` cede CPU explícitamente.
- `task_wait_event()` y `task_signal_event()` permiten bloqueo/despertar por eventos simples.
- `task_wait_id()` permite esperar por PID concreto sin race básica de espera.
- `Ctrl+C` marca cancelación para la tarea foreground actual.
- Existe callback para reactivar el prompt cuando termina una tarea foreground.
- Las tareas de user mode usan directorio de páginas propio y heap de usuario separado.
- Cada tarea mantiene `parent_id`; los huérfanos se reparentan a `init`.
- Los zombies se muestran en `ps` y son recolectados por `init`.

## Shell disponible

Comandos implementados ahora mismo:

- `help`: lista comandos.
- `clear` / `cls`: limpia la pantalla.
- `about`: muestra versión y resumen del sistema.
- `echo <texto>`: imprime texto.
- `env`, `set`, `unset`: gestionan variables de entorno básicas.
- `keymap [us|es]`: muestra o cambia el layout activo del teclado.
- `mouse`: muestra estado actual del ratón PS/2.
- `source <NOMBRE>`: ejecuta scripts de comandos desde el FS en memoria.
- `color <white|green|red|blue>`: cambia el color del texto.
- `theme [default|matrix|amber|ice]`: cambia el estilo visual del prompt y selección.
- `dmesg [clear]`: muestra o limpia el buffer de logs del kernel.
- `gfxdemo`: dibuja primitivas gráficas básicas en el framebuffer.
- `history`: imprime el historial del editor de línea.
- `count [n] [&]`: demo por pasos cancelable.
- `sleep <ms> [&]`: duerme una tarea en foreground o background.
- `uptime`: muestra tiempo de arranque formateado (H:m:s) y ticks.
- `date`: muestra la fecha/hora actual leída del RTC CMOS, el epoch Unix aproximado y el contador monotónico en microsegundos.
- `ps` / `jobs`: lista tareas activas.
- `getpid`: muestra el PID de la tarea actual.
- `nice <id> <high|normal|low>`: cambia prioridad de una tarea.
- `kill <id>`: cancela una tarea.
- `task`: muestra la tarea actual y el foreground.
- `mem`: muestra estadísticas del heap.
- `wait <id> [&]`: bloquea una tarea esperando un evento.
- `signal <id>`: despierta tareas bloqueadas en ese evento.
- `ls [ruta]`: lista archivos y directorios del VFS.
- `cat <ruta>`: muestra el contenido de un archivo VFS.
- `cd [ruta]`: cambia el directorio de trabajo actual.
- `pwd`: muestra el directorio de trabajo actual.
- `touch <ruta>`: crea un archivo vacío en el VFS.
- `rm <ruta>`: borra un archivo del VFS.
- `mkdir <ruta>`: crea un directorio en el VFS.
- `rename <origen> <destino>`: renombra o mueve un archivo/directorio en el VFS.
- `stat <ruta>`: muestra metadatos del nodo VFS, tamaño y modo en octal.
- `chmod <modo_octal> <ruta>`: cambia los permisos UNIX de un nodo VFS.
- `vfs [ls|cat|touch|rm|stat|chmod] [ruta]`: operaciones VFS de bajo nivel con diagnóstico.
- `disk [read <lba> [dev]] [mount <dev> <ruta>] [fsck <dev>] [gpt <dev>]`: operaciones de disco, montaje manual e inspección de tabla GPT.
- `elfinfo <NOMBRE>`: inspecciona una imagen ELF32 i386 del FS.
- `exec <NOMBRE> [args...] [&]`: carga y ejecuta un ELF en user mode pasando `argv`.
- `yield`: cede CPU al scheduler.

## Entrada interactiva

La shell tiene más edición de la que parece a simple vista:

- Navegación de historial.
- Movimiento horizontal del cursor.
- Selección con `Shift` + flechas.
- `Tab` para autocompletar comandos y rutas del VFS relativas al cwd actual.
- Barra de sugerencias inline bajo el prompt: muestra coincidencias en amarillo y filtra mientras se escribe; desaparece al confirmar o cancelar.
- `Ctrl+C` para cancelación o interrupción.
- `Ctrl+L` para limpiar pantalla.
- `Ctrl+U` para borrar línea.
- `Ctrl+A` para seleccionar todo.
- `Ctrl+X` para cortar línea o selección.
- `Ctrl+V` y `Shift+Insert` para pegar.
- `Ctrl+C` / `Ctrl+Shift+C` para copiar según contexto.
- `Insert` alterna entre modo insertar y overwrite, y el cursor cambia para reflejarlo.
- `Caps Lock` alterna mayúsculas/minúsculas en letras ASCII y caracteres españoles como `ñ`/`Ñ`.

## Teclado e interrupciones

- IRQ0 -> vector `32`.
- IRQ1 -> vector `33`.
- IRQ12 -> vector `44`.
- Syscalls -> vector `0x80`.
- El driver de teclado intenta adaptarse a scancode set 1 y set 2.
- El driver de teclado soporta layouts ASCII básicos `us` y `es`.
- El driver de ratón PS/2 decodifica paquetes de 3 bytes y publica eventos a través de `input`.
- La shell consume eventos desde una capa `input` genérica en vez de depender directamente del teclado.
- El driver de teclado traduce `AltGr`, `Caps Lock`, teclado numérico con `Num Lock`, y las teclas extendidas para que la shell reciba eventos reales de edición y navegación.
- El arranque recomendado con `make execute` usa por defecto `-display sdl,show-cursor=off` para ocultar el cursor del host y dejar visible solo el cursor software del guest.
- Si prefieres GTK, puedes sobreescribirlo al vuelo con `make run QEMU_DISPLAY='gtk,show-cursor=off,grab-on-hover=on'`.
- La entrada no escribe directamente en pantalla: genera `keyboard_event_t` y los entrega al editor de línea.

## Syscalls actuales

El dispatcher de `syscall.c` soporta:

- escritura de texto (`SYSCALL_WRITE`),
- lectura de ticks (`SYSCALL_GET_TICKS`),
- sleep (`SYSCALL_SLEEP`),
- yield (`SYSCALL_YIELD`),
- alloc/free (`SYSCALL_ALLOC`, `SYSCALL_FREE`),
- exit (`SYSCALL_EXIT`),
- syscalls FS legacy (`SYSCALL_FS_COUNT`, `SYSCALL_FS_NAME_AT`, `SYSCALL_FS_NAME_COPY`, `SYSCALL_FS_SIZE`, `SYSCALL_FS_READ`): accesibles solo desde kernel mode; retornan `EPERM` si las invoca un proceso de usuario,
- operaciones VFS completas: `SYSCALL_VFS_OPEN`, `SYSCALL_VFS_CLOSE`, `SYSCALL_VFS_READ`, `SYSCALL_VFS_WRITE`, `SYSCALL_VFS_SEEK`, `SYSCALL_VFS_READDIR`, `SYSCALL_VFS_CREATE`, `SYSCALL_VFS_DELETE`,
- y syscalls de procesos: `SYSCALL_GETPID`, `SYSCALL_KILL`, `SYSCALL_WAIT`, `SYSCALL_EXEC`, `SYSCALL_GET_ERRNO`, `SYSCALL_FORK`, `SYSCALL_EXECV`, `SYSCALL_EXECVE`, `SYSCALL_SIGNAL`, `SYSCALL_WAITPID`, `SYSCALL_SIGPROCMASK`.

Todas las syscalls VFS validan punteros de usuario, comprueban la máscara de flags permitidos en `open`, verifican los bounds de `name_max` en `readdir`, y propagan `errno` (`EFAULT`, `EINVAL`, `EACCES`, `EBADF`) en todos los caminos de error.

## Sistema de archivos

Lyth OS cuenta con una capa VFS sobre la que se montan diferentes backends:

- **ramfs** (montado en `/`): filesystem en RAM completamente escribible. Soporta directorios virtuales (`mkdir`), creación y borrado de archivos (`touch`, `rm`), escritura con truncado y modo append (`>`, `>>`), y lectura (`cat`, `<`).
- **FAT16** (montado automáticamente en `/sd0p0`, `/sd0p1`, etc.): driver con lectura y escritura. Se detecta y monta al arranque si hay partición FAT16 en disco (requiere imagen `disk.img` adjunta a QEMU).
- **FAT32** (montado automáticamente): driver con lectura y escritura, soporte completo de LFN (Long File Names) para nombres de hasta 255 caracteres.

Particionado:

- **MBR**: detección automática de hasta 4 particiones primarias con tipo FAT16/FAT32.
- **GPT**: detección automática mediante MBR protectora (`0xEE`), lectura de hasta 32 entradas de partición desde la tabla LBA1; el comando `disk gpt <dev>` permite inspeccionar la tabla GPT de cualquier disco.

Permisos tipo UNIX:

- Tabla de permisos por ruta (256 entradas), modos de 9 bits `rwxrwxrwx`.
- Modos por defecto: `/` → 0777, directorios → 0755, archivos → 0644.
- Enforcement activo en `open` (lectura/escritura/ejecución), `create`, `delete` y `rename` (comprobación del directorio padre).
- API: `vfs_chmod(path, mode)`, `vfs_get_mode(path, &mode)`.

Operaciones VFS disponibles:

- `vfs_open`, `vfs_close`, `vfs_read`, `vfs_write`, `vfs_seek` (SET/CUR/END).
- `vfs_readdir`: iteración de entradas de directorio por índice.
- `vfs_create`, `vfs_delete`, `vfs_rename`: creación, borrado y renombrado de archivos y directorios.
- `vfs_stat`: metadatos (tamaño, tipo, modo).
- `vfs_chmod`, `vfs_get_mode`: gestión de permisos UNIX.
- `vfs_resolve`: resolución de rutas absolutas y relativas, normalización de `.` y `..`.
- `vfs_mount`: registro de puntos de montaje.
- Tabla de FDs por tarea con init/close automático en el scheduler.

Syscalls VFS disponibles desde user mode: `SYSCALL_VFS_OPEN`, `SYSCALL_VFS_CLOSE`, `SYSCALL_VFS_READ`, `SYSCALL_VFS_WRITE`, `SYSCALL_VFS_SEEK`, `SYSCALL_VFS_READDIR`, `SYSCALL_VFS_CREATE`, `SYSCALL_VFS_DELETE`.

La shell mantiene un directorio de trabajo (`cwd`) y resuelve rutas relativas, `.` y `..` mediante `shell_resolve_path`.

Archivos estáticos precargados en ramfs al arranque:

- `README.TXT`, `MOTD.TXT`, `VERSION.TXT`, `DEMO.SH`, `DEMO.ELF`

## Build y ejecución

Requisitos habituales:

- `gcc` con soporte `-m32`
- `as`, `ld`
- `grub-mkrescue`
- `qemu-system-i386`

Comandos principales:

- `make help`: lista los targets públicos del `Makefile` y qué hace cada uno.
- `make compile`: compila todos los `.c` y `.s`, y enlaza el kernel final en `build/kernel.bin` sin crear todavía la ISO.
- `make create-iso`: recompila si hace falta, copia `kernel.bin` y `grub.cfg` al árbol temporal y genera `dist/lyth.iso` con `grub-mkrescue`.
- `make execute`: crea o actualiza la ISO y la arranca en QEMU con el display definido en `QEMU_DISPLAY`; por defecto usa SDL con el cursor del host oculto.
- `make debug`: igual que `execute`, pero añade `-d int` para que QEMU saque trazas de interrupciones y excepciones en consola.
- `make gdb-wait`: crea la ISO, arranca QEMU parado desde el primer instante y abre el stub remoto de GDB en `localhost:1234`.
- `make gdb-connect`: no ejecuta nada por sí mismo; solo imprime el comando de `gdb` recomendado para conectarte al kernel ya arrancado con `make gdb-wait`.
- `make clean`: borra `build/`, `dist/` y la cabecera generada de la fuente PSF para forzar una reconstrucción limpia.
- `make run`: flujo completo recomendado para uso diario; ejecuta `clean`, luego `compile`, después `create-iso` y finalmente `execute`.

Backend de vídeo de QEMU:

- `QEMU_DISPLAY=sdl,show-cursor=off` es ahora el valor por defecto porque suele comportarse mejor cuando el guest dibuja su propio cursor software.
- Si en tu entorno SDL no va bien, puedes cambiar solo esa parte sin tocar el `Makefile`, por ejemplo: `make run QEMU_DISPLAY='gtk,show-cursor=off,grab-on-hover=on'`.
- Para una prueba definitiva del cursor doble, usa `make run FB_MOUSE_CURSOR=0`; si sigues viendo un cursor, ese es el del host/QEMU, no el del kernel.

Depuración remota rápida:

- Terminal 1: `make gdb-wait`
- Terminal 2: `gdb -ex "target remote localhost:1234" -ex "symbol-file build/kernel.bin"`

## Observaciones del estado actual

- El framebuffer usa fuente PSF 8x16 con márgenes y cursor overlay propio.
- La resolución preferida sigue siendo alta, pero el área útil de consola ahora se calcula respetando padding.
- El scheduler ya hace cambio de contexto entre stacks de kernel, con prioridades y bloqueo por eventos.
- El kernel mantiene identity mapping global supervisor-only para sí mismo, mientras que cada tarea de usuario dispone de una ventana virtual propia sobre un directorio de páginas dedicado.
- Ya se ejecutan binarios ELF en ring 3 desde el FS en memoria, incluyendo `argv` inicial en `exec`/`execv`.
- `fork` clona memoria de usuario y tabla de FDs (con conteo de referencias en VFS).
- Los procesos terminados quedan en estado zombie hasta ser recolectados por su padre o por `init`.
- El kernel enlaza ahora segmentos separados de código/solo lectura y datos/escritura, evitando el `RWX` global del binario final.
- La redirección `>` y `>>` escribe directamente en el VFS; `<` lee desde el VFS. El modo append usa `vfs_seek(VFS_SEEK_END)` antes de escribir.
- El driver ATA PIO intenta detectar unidades al arranque; si hay disco con partición FAT16, se monta automáticamente.

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
- `fs/vfs.c`
- `fs/ramfs.c`
- `fs/fat16.c`
- `drivers/disk/ata.c`
- `drivers/disk/blkdev.c`
- `Makefile`

## Próximos pasos razonables

- Implementar usuarios y grupos (UID/GID por proceso, `getuid`/`setuid`).
- Añadir guard pages para stacks de tarea (protección de desbordamiento de pila).
- Implementar `mmap` / regiones mapeadas y copy-on-write.
- Panic screen con backtrace del kernel y volcado de registros.
- Backtrace del kernel con resolución de símbolos desde la tabla ELF.
- Soporte de consolas virtuales (`/dev/tty0`, `/dev/tty1`, ...).
- Implementar `poll`/`select` para multiplexación de I/O.
- IPC avanzado: pipes con buffer circular, memoria compartida.
- Límites de recursos por proceso (FD limits).
- Soporte APIC/IOAPIC para reemplazar el PIC 8259.
- Idle task explícita para el scheduler cuando no hay trabajo pendiente.
