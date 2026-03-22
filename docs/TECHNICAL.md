# Lyth OS — Documentación técnica

## Arquitectura general

```
arch/x86/boot/boot.s  →  kernel_main()  →  subsistemas (GDT, consola, mem, VFS, scheduler)
                                         →  interrupts_init() (IDT, PIC, PIT, IRQs)
                                         →  tarea init (PID 1) → shell interactiva
```

Subsistemas principales y sus archivos:

| Subsistema | Archivos principales |
|---|---|
| Arranque | `arch/x86/boot/boot.s`, `arch/x86/boot/grub.cfg` |
| Kernel init | `kernel/kernel.c` |
| GDT / TSS | `kernel/gdt.c`, `arch/x86/gdt.s` |
| IDT / PIC / PIT | `kernel/idt.c`, `kernel/interrupts.c`, `arch/x86/interrupts.s` |
| Scheduler | `kernel/task/task.c`, `kernel/task/timer.c` |
| Memoria física | `kernel/mem/physmem.c` |
| Paginación | `kernel/mem/paging.c` |
| Heap del kernel | `kernel/mem/heap.c` |
| Syscalls | `kernel/syscall.c` |
| Señales | `kernel/task/task.c` |
| ELF / user mode | `kernel/elf.c`, `kernel/usermode.c` |
| Usuarios/grupos | `kernel/ugdb.c` |
| VFS | `fs/vfs.c` |
| ramfs | `fs/ramfs.c`, `fs/fs.c` |
| FAT16 | `fs/fat16.c` |
| FAT32 | `fs/fat32.c` |
| devfs | `fs/devfs.c`, `drivers/console/tty_vfs.c` |
| Pipes | `fs/pipe.c` |
| Consola | `drivers/console/terminal.c`, `drivers/console/fbconsole.c`, `drivers/console/console_backend.c` |
| Teclado / ratón | `drivers/input/keyboard.c`, `drivers/input/mouse.c`, `drivers/input/input.c` |
| ATA / blkdev | `drivers/disk/ata.c`, `drivers/disk/blkdev.c` |
| RTC | `drivers/rtc/rtc.c` |
| Serie | `drivers/serial/serial.c` |
| Shell | `userland/shell/shell.c`, `userland/shell/shell_input.c`, `userland/shell/parser.c` |
| GDB remoto | `kernel/ugdb.c` |
| Panic / assert | `kernel/panic.c`, `kernel/klog.c` |

---

## Flujo de arranque

1. GRUB carga el kernel con Multiboot. `boot.s` solicita modo gráfico `1280×1024×32` y pasa a `kernel_main()`.
2. `kernel_main()` inicializa en orden: GDT + TSS, consola (detecta framebuffer o VGA), memoria física (bitmap Multiboot), heap, VFS (monta ramfs en `/`), ugdb (usuarios/grupos), scheduler.
3. `ata_init()` detecta unidades ATA; `blkdev` escanea MBR/GPT; las particiones FAT16/FAT32 se montan automáticamente.
4. `interrupts_init()` instala la IDT, remapea el PIC 8259A, configura el PIT a 100 Hz y habilita IRQs.
5. El scheduler arranca la tarea `init` (PID 1), que inicializa la shell y entra en el bucle de eventos.
6. El loop principal del kernel ejecuta `hlt`; toda la actividad ocurre desde interrupciones y la tarea init.

---

## Consola y vídeo

La consola mantiene un buffer lógico de celdas (carácter + color) independiente del renderer activo.

- **`terminal.c`**: cursor, scroll, overwrite mode, color, escritura de texto.
- **`console_backend.c`**: selecciona el renderer activo (VGA o framebuffer) según lo que GRUB exponga.
- **`fbconsole.c`**: renderiza la fuente PSF bitmap 8×16, aplica padding, gestiona el cursor overlay parpadeante y el cursor de ratón software. Expone primitivas gráficas básicas.

El framebuffer se activa si el flag `MULTIBOOT_INFO_FRAMEBUFFER_INFO` está presente y el tipo es RGB. Si no, se usa el renderer VGA.

---

## Memoria

### Memoria física
`physmem.c` construye un bitmap de frames (4 KiB cada uno) a partir del mapa de memoria Multiboot. Permite reservar y liberar regiones físicas por alineación de frame.

### Paginación
`paging.c` activa paginación con identity mapping inicial usando páginas grandes de 4 MiB (bit `PSE`). Expone:
- `paging_create_user_dir()`: crea un directorio de páginas para un proceso de usuario.
- `paging_switch(dir)`: cambia `CR3` en el context switch.
- `paging_validate_user_buffer(addr, size)`: valida que un buffer de usuario sea accesible antes de una syscall.

Cada proceso de usuario tiene su propio directorio de páginas. El kernel usa identity mapping supervisor-only global.

La región de usuario usa una page table de 4 KiB y deja una guard page no mapeada justo debajo del stack. Si un proceso toca esa página, provoca `Page fault` y el kernel lo reporta como `stack guard page hit`.

Justo debajo de la guard page existe además una ventana fija de shared memory. Los segmentos SHM se respaldan con frames físicos propios del kernel y se remapean dentro de esa ventana en cada proceso que hace `attach`, de modo que varios procesos ven las mismas páginas físicas sin copiar contenido.

### Heap del kernel
`heap.c` gestiona un array estático de 256 KB con un allocator first-fit. `kmalloc(size)` devuelve un puntero alineado; `kfree(ptr)` libera y coalesce bloques adyacentes libres.

---

## Scheduler y tareas

`task.c` implementa un scheduler preemptivo guiado por el PIT (IRQ0).

### Estructura de una tarea (`task_t`)
- Stack de kernel propio (16 KB).
- Directorio de páginas de usuario (`page_dir_t*`).
- Estado: `TASK_FREE`, `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_BLOCKED`, `TASK_ZOMBIE`, `TASK_DONE`, `TASK_CANCELLED`.
- Prioridad: `HIGH`, `NORMAL`, `LOW`.
- Tabla de FDs propia (`vfs_fd_entry_t[VFS_MAX_FD]`).
- `uid`, `gid`, `euid`, `egid`, grupos suplementarios (hasta `TASK_MAX_SUPP_GROUPS`).
- Señales: máscara, conjunto de señales pendientes, tabla de handlers.
- `parent_id`, `exit_code`.
- Mapeos SHM activos por proceso (hasta 8 segmentos simultáneos).
- Límites de recursos (`fd_limit_soft`, `fd_limit_hard`).

### Ciclo de vida
La IRQ0 llama al scheduler, que elige la tarea de mayor prioridad en estado `READY`. El context switch guarda/restaura registros de kernel en el stack de la tarea saliente/entrante.

`task_fork()` clona la tarea activa: copia el stack de usuario, la tabla de FDs (con `ref_count` en VFS) y el directorio de páginas. No copia el heap del kernel.

Cuando el proceso tiene SHM adjunta, `fork` hereda esos mapeos reinsertando las mismas páginas físicas en el hijo. `exec` y la salida del proceso hacen `detach_all`, decrementan referencias y liberan el segmento cuando ya no quedan adjuntos y además fue marcado con `shm_unlink`.

Los zombies permanecen visibles en `ps` hasta que el padre llama a `wait`/`waitpid` o `init` los adopta.

---

## Syscalls

El dispatcher está en `kernel/syscall.c`, activado por `int 0x80`. Los argumentos llegan en registros (`eax` = número, `ebx/ecx/edx/esi/edi` = args).

Todas las syscalls que reciben punteros de usuario los validan con `paging_validate_user_buffer` o `syscall_validate_user_string` antes de usarlos. El acceso falla con `EFAULT` si el buffer no es válido.

### Tabla de syscalls (selección)

| Syscall | Descripción |
|---|---|
| `WRITE` | Escribe texto en la consola |
| `GET_TICKS` / `SLEEP` / `YIELD` | Tiempo y cesión de CPU |
| `VFS_OPEN/CLOSE/READ/WRITE/SEEK` | Operaciones de archivo |
| `VFS_READDIR/CREATE/DELETE` | Directorios |
| `VFS_CHOWN` | Cambio de propietario |
| `FORK` | Clona el proceso actual |
| `EXEC` / `EXECV` / `EXECVE` | Carga y ejecuta un ELF |
| `EXIT` / `WAIT` / `WAITPID` | Ciclo de vida de proceso |
| `GETPID` / `KILL` | Identificación y señales |
| `SIGNAL` / `KILLSIG` / `SIGPROCMASK` / `SIGPENDING` | Sistema de señales |
| `GETUID/GID/EUID/EGID` | Identidad real |
| `SETUID` / `SETGID` | Cambio de identidad |
| `GETGROUPS` / `SETGROUPS` | Grupos suplementarios |
| `PIPE` | Crea un par de FDs de tubería |
| `POLL` / `SELECT` | Multiplexación de I/O |
| `SHM_CREATE/ATTACH/DETACH/UNLINK` | Segmentos de memoria compartida |
| `MQ_CREATE/SEND/RECV/UNLINK` | Message queues kernel-globales |
| `GET_TIME` / `GET_MONOTONIC_MS` | Tiempo real y monotónico |
| `GETRLIMIT` / `SETRLIMIT` | Límites de recursos (RLIMIT_NOFILE) |
| `ALLOC` / `FREE` | Heap de usuario |

---

## Sistema de archivos (VFS)

La capa VFS (`fs/vfs.c`) actúa como intermediario entre las syscalls y los backends concretos.

### Tabla de montajes
Hasta `VFS_MAX_MOUNTS` (16) puntos de montaje. `vfs_mount(path, root_node)` registra un backend. La resolución de rutas busca el montaje más específico.

### Permisos UNIX
Tabla interna de hasta 256 entradas (ruta → modo 9 bits). Modos por defecto: `0777` para `/`, `0755` para directorios, `0644` para archivos. Los bits de propietario (`uid`/`gid`) se almacenan por separado. El enforcement se realiza en `vfs_open_flags`, `vfs_create`, `vfs_delete` y `vfs_rename`.

### Backends

**ramfs** (`fs/ramfs.c` + `fs/fs.c`): almacenamiento clave-valor en memoria. Soporta `mkdir`, `touch`, `write` (truncado y append), `read`, `readdir`, `delete`, `rename`. Montado en `/` por defecto. Archivos iniciales embebidos en el kernel:

| Ruta | Contenido |
|---|---|
| `/etc/motd` | Mensaje de bienvenida (leído por la shell al arrancar) |
| `/etc/os-release` | Identificación del OS en formato `KEY=Value` estándar, incluida versión y flavor del build |
| `/home/user/demo.sh` | Script de ejemplo para la shell |
| `/home/user/demo` | Binario ELF mínimo de demostración de usermode |

**FAT16** (`fs/fat16.c`): lectura y escritura de archivos y directorios. Montaje automático sobre particiones detectadas como FAT16 en `blkdev`.

**FAT32** (`fs/fat32.c`): lectura y escritura con soporte completo de LFN (Long File Names, hasta 255 caracteres).

**devfs** (`fs/devfs.c`): nodos virtuales para `/dev/null`, `/dev/zero`, `/dev/tty`, `/dev/console` y los dispositivos de bloque detectados (`/dev/sd0`, `/dev/sd0p0`, ...).

**pipes** (`fs/pipe.c`): buffer circular de 4 KB, API read/write con bloqueo voluntario (`task_yield`). Creados por la syscall `PIPE`.

### API VFS pública
`vfs_open_flags`, `vfs_close`, `vfs_read`, `vfs_write`, `vfs_seek`, `vfs_readdir`, `vfs_create`, `vfs_delete`, `vfs_rename`, `vfs_stat`, `vfs_chmod`, `vfs_chown`, `vfs_get_mode`, `vfs_get_owner`, `vfs_resolve`, `vfs_mount`.

---

## Shell interactiva

La shell implementa un parser propio con soporte para comillas, expansión simple de variables, ejecución en background (`&`), redirección (`<`, `>`, `>>`) y pipes encadenados (`cmd1 | cmd2 | cmd3`).

El pipe de la shell captura la salida textual de cada etapa y la expone a la siguiente como entrada estándar lógica. Las capturas crecen dinámicamente en heap, así que ya no dependen de un buffer fijo pequeño por etapa.

Las utilidades que consumen esa entrada de pipe son:
- `cat`
- `grep`
- `head`
- `tail`
- `more` / `less`
- `wc`

Ejemplos válidos:
- `help | grep mkdir`
- `help | tail -20`
- `help | grep task | tail -5`
- `help | wc`

La implementación sigue siendo una tubería textual interna de shell, no un stream POSIX completo entre procesos separados, pero ya no trunca la salida simplemente por un buffer fijo de 4 KB en cada etapa.

En builds con `AUTOTEST=1`, la ramfs inicial incluye `/etc/bootrc.sh` con una secuencia de validación automática de shell y guard pages. En ese modo, la salida del terminal se espeja también a COM1 para que el harness headless pueda validar el resultado desde el host.

La shell expone `shm` para administración básica de segmentos (`list`, `create`, `unlink`) y `shmdemo` para una prueba end-to-end. `shmdemo` crea un segmento, arranca un writer userland que escribe un byte en la ventana SHM y luego un reader userland que valida el mismo valor desde otro mapeo. El harness AUTOTEST comprueba el mensaje `shmread verificado correctamente` en la salida serie.

La shell expone `mq` para colas de mensajes globales. La implementación actual usa hasta 16 colas simultáneas, con un máximo de 32 mensajes por cola y 256 bytes por mensaje. La cola publica dos eventos internos por identificador, uno de legibilidad y otro de espacio disponible, de modo que tareas del kernel pueden bloquearse de forma real sobre `recv` o `send` y despertar cuando la condición cambia o cuando vence un timeout. Ademas, cada cola puede abrirse como un FD pseudo-VFS con `mq open`, de forma que `read`/`write` y `poll/select` reutilizan la misma semantica de readiness que ya usa el kernel para pipes. En userland existen ahora dos syscalls temporizadas especificas, `mq_send_timed(queue, msg, size, timeout_ms)` y `mq_recv_timed(queue, buf, size, timeout_ms)`, que devuelven `EAGAIN` si `timeout_ms=0` y la cola no esta lista, y `ETIMEDOUT` cuando vence un plazo positivo. `mq send` y `mq recv` siguen siendo no bloqueantes por identificador, mientras que `mq sendwait` y `mq recvwait` ejecutan tareas auxiliares con espera acotada. Para mantener AUTOTEST determinista, `mq demo` valida `poll`, `select` y el timeout de cola llena, terminando con `mq demo ok`.

---

## Procesos y señales

### ELF loader y user mode
`kernel/elf.c` valida la cabecera ELF32 i386. `kernel/usermode.c` mapea los segmentos `PT_LOAD` en la región virtual de usuario, construye la pila con `argv`/`envp` y crea una tarea ring 3.

Para validar las guard pages del stack hay dos pruebas sintéticas expuestas por la shell:
- `stackbomb`: lanza una tarea userland mínima que toca la guard page del stack y debe terminar con `Page fault`.
- `stackok`: lanza una tarea userland mínima que toca memoria válida del stack y sale limpiamente con `exit(0)`.

### Señales
Cada tarea mantiene:
- `signal_mask`: señales bloqueadas (excepto `SIGKILL`, `SIGSTOP`).
- `signal_pending`: señales recibidas y aún no entregadas.
- `signal_handlers[NSIG]`: puntero a handler de usuario o acción por defecto.

La entrega ocurre justo antes de retornar al proceso. El kernel construye un frame de señal en la pila de usuario (guarda contexto), salta al handler, y al retornar el proceso ejecuta un trampoline que restaura el contexto original.

`SIGCHLD` se envía al padre cuando un hijo termina. `wait`/`waitpid` recolecta el zombie y devuelve el exit code.

---

## Usuarios y grupos (ugdb)

`kernel/ugdb.c` implementa una base de datos estática de usuarios y grupos.

### Estructuras

```c
typedef struct {
    int          used;
    unsigned int uid;
    unsigned int gid;      /* grupo primario */
    char         name[16];
    char         password[16];  /* vacío = sin contraseña */
} ugdb_user_t;

typedef struct {
    int          used;
    unsigned int gid;
    char         name[16];
    unsigned int member_uids[UGDB_MAX_USERS];
    unsigned int member_count;
} ugdb_group_t;
```

### Capacidades
- Hasta `UGDB_MAX_USERS` (8) usuarios y `UGDB_MAX_GROUPS` (8) grupos.
- Built-ins: `root` (uid=0, gid=0) y `user` (uid=1, gid=1).
- `ugdb_next_uid()` / `ugdb_next_gid()`: IDs >= 1000 para cuentas nuevas.
- `ugdb_check_password(uid, pw)`: contraseña vacía siempre concede acceso.
- `ugdb_group_add/remove_member`, `ugdb_group_is_member`, `ugdb_get_user_groups`.

### Integración con el scheduler
Cada tarea mantiene `uid`, `gid`, `euid`, `egid` y un array de grupos suplementarios. `task_force_identity(uid, gid)` cambia los cuatro valores a la vez. Las syscalls `SETUID`/`SETGID` siguen la semántica POSIX: solo root puede establecer un UID arbitrario; un proceso no-root solo puede volver a su UID real.

### Comandos de administración (shell)
| Comando | Acción |
|---|---|
| `passwd [usuario]` | Cambia contraseña; non-root solo la suya con verificación |
| `useradd <nombre> [-u uid] [-g gid]` | Crea usuario y su home |
| `userdel [-r] <usuario>` | Elimina usuario; `-r` borra también el home |
| `usermod -n|-g|-p <valor> <usuario>` | Modifica nombre, grupo o contraseña |
| `groupadd <nombre> [-g gid]` | Crea grupo |
| `groupdel <nombre>` | Elimina grupo (wheel protegido) |
| `gpasswd -a\|-d <usuario> <grupo>` | Añade o quita miembro de grupo |
| `su <usuario>` | Cambia identidad; pide contraseña si el destino la tiene |
| `login <usuario>` | Inicia sesión explícita con contraseña |
| `logout` | Cierra sesión y restaura root |
| `who` / `users` | Muestra la sesión activa actual |

---

## Entrada

- **`keyboard.c`**: traduce scancodes set 1/2, layouts `us`/`es`, `AltGr`, `Caps Lock`, `Num Lock`, teclas extendidas (`Delete`, `Home`, `End`, `Page Up/Down`). Publica `keyboard_event_t` en una cola.
- **`mouse.c`**: decodifica paquetes PS/2 de 3 bytes, mantiene posición acumulada y estado de botones.
- **`input.c`**: convierte eventos de teclado y ratón en `input_event_t` para consumidores desacoplados.

IRQ1 (teclado) → vector 33; IRQ12 (ratón) → vector 44.

---

## Drivers de disco

### ATA PIO (`drivers/disk/ata.c`)
Detecta hasta 2 unidades (master/slave) en el controlador primario al arranque. Lee sectores de 512 bytes en modo PIO 28-bit LBA.

### blkdev (`drivers/disk/blkdev.c`)
Capa de abstracción sobre ATA. Al registrar una unidad:
1. Lee el MBR (sector 0). Si la partición 0 tiene tipo `0xEE`, intenta GPT.
2. **MBR**: reconoce hasta 4 entradas primarias con tipos FAT16 (`0x04`, `0x06`, `0x0E`) y FAT32 (`0x0B`, `0x0C`).
3. **GPT**: lee la cabecera en LBA1, itera hasta 32 entradas de partición, identifica GUIDs FAT.
4. Registra las particiones como `blkdev_t` con nombre `sd0`, `sd0p0`, `sd0p1`, etc.
5. Intenta montar cada partición FAT16/FAT32 automáticamente.

---

## Build

### Targets del Makefile

| Target | Descripción |
|---|---|
| `make compile` | Compila todos los `.c`/`.s` y enlaza `build/kernel.bin` |
| `make create-iso` | Genera `dist/lyth.iso` con `grub-mkrescue` |
| `make create-autotest-iso` | Genera `dist/lyth-autotest.iso` con `AUTOTEST=1` y script de prueba embebido |
| `make execute` | Arranca la ISO en QEMU (SDL, cursor host oculto) |
| `make run` | `clean` + `compile` + `create-iso` + `execute` |
| `make debug` | Como `execute` pero con `-d int` en QEMU |
| `make gdb-wait` | Arranca QEMU congelado con GDB stub en `localhost:1234` |
| `make gdb-connect` | Imprime el comando `gdb` para conectarse |
| `make disk-fat16` | Crea `disk.img` (32 MB) con partición FAT16 |
| `make disk-fat32` | Crea `disk.img` con partición FAT32 y LFN |
| `make harness-image` | Arranca la imagen de autotest headless y valida boot + shell + stack tests |
| `make repro-check` | Recompila dos veces y comprueba que `build/kernel.bin` y `dist/lyth.iso` son idénticos |
| `make clean` | Borra `build/` y `dist/` |
| `make test` | Compila, genera ISO y valida multiboot con `grub-file` |

### Toolchain host y cruzada
- Flujo actualmente validado en CI: toolchain host con `gcc -m32`, `as`, `ld`, `grub-mkrescue` y `grub-file` del sistema.
- Flujo recomendado para desarrollo aislado: toolchain cruzada ELF i386, por ejemplo `i686-elf-gcc`, `i686-elf-as`, `i686-elf-ld`.
- El Makefile admite ambas rutas:
    - host: `make run`
    - cruzada por prefijo: `make run CROSS_PREFIX=i686-elf-`
    - cruzada por binario explícito: `make compile CC=i686-elf-gcc AS=i686-elf-as LD=i686-elf-ld`
- Las utilidades de GRUB y QEMU siguen siendo herramientas del host; no van detrás de `CROSS_PREFIX`.

### Reproducibilidad
- `SOURCE_DATE_EPOCH` se exporta desde el Makefile; por defecto toma la fecha del último commit y cae a un valor fijo si no hay metadata de git.
- `ld` se invoca con `--build-id=none` para evitar variaciones no funcionales en `kernel.bin`.
- `gcc` usa `-ffile-prefix-map=$(CURDIR)=.` para no incrustar rutas absolutas del workspace.
- antes de generar la ISO, los mtimes de `build/iso` se fijan al mismo `SOURCE_DATE_EPOCH`.
- `grub-mkrescue` recibe fechas de volumen ISO explícitas (`c`, `m`, `uuid`) derivadas del mismo instante reproducible.
- `xorriso` reescribe recursivamente la fecha de modificación Rock Ridge de toda la imagen con `-alter_date_r m ... /`.
- `make repro-check` recompila dos veces desde limpio y exige identidad byte a byte de `build/kernel.bin` y `dist/lyth.iso`.

### Flags de compilación relevantes
- `-m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector`
- `-fno-omit-frame-pointer -fno-optimize-sibling-calls` (para backtraces fiables)
- `FB_MOUSE_CURSOR=0` deshabilita el cursor software del guest (diagnóstico)
