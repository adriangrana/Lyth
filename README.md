# Lyth OS

Kernel hobby x86 de 32 bits escrito desde cero en C y ASM, arrancado con GRUB/Multiboot y ejecutable en QEMU.

Lyth OS cubre los subsistemas clásicos de un kernel real: arranque, gestión de memoria, multitarea preemptiva, sistema de archivos con VFS, syscalls, procesos en modo usuario, drivers de disco y periféricos, una shell interactiva completa y un modelo de seguridad multiusuario con permisos tipo UNIX.

> Documentación técnica detallada en [docs/TECHNICAL.md](docs/TECHNICAL.md).

---

## Características

### Arranque y arquitectura
- Arranque Multiboot con GRUB, solicita modo gráfico `1280×1024×32` con fallback a VGA
- Kernel ELF32 enlazado a 1 MB con GDT propia, segmentos kernel/user y TSS
- SMP: detección de CPUs vía MADT, boot de APs con INIT/SIPI, GDT/TSS per-CPU

### Consola y vídeo
- Framebuffer con fuente PSF 8×16, márgenes y scroll propio
- Backend VGA de fallback completamente funcional
- Cursor de ratón software sobre framebuffer; cursor hardware en VGA
- Temas visuales: `default`, `matrix`, `amber`, `ice`

### Entrada
- Driver de teclado PS/2 con layouts `us` / `es`, `AltGr`, `Caps Lock`, `Num Lock` y teclas extendidas
- Driver de ratón PS/2 con cursor overlay en framebuffer
- Capa de eventos de entrada genérica desacoplada de la shell

### Shell interactiva
- Parser propio con comillas, `argv` completo, variables de entorno (`$VAR`)
- Redirección `>`, `>>`, `<` integrada con VFS; pipes encadenados (`cmd1 | cmd2 | cmd3`)
- Utilidades de texto preparadas para pipe: `grep`, `head`, `tail`, `more`/`less`, `wc`
- Ejecución en background (`cmd &`)
- Historial, autocompletado (`Tab`) con barra de sugerencias inline
- Selección, clipboard, `Ctrl+C/L/U/A/X/V`
- Scripts ejecutables desde VFS (`source archivo.sh`)

### Scheduler
- Preemptivo por PIT (100 Hz), prioridades `HIGH` / `NORMAL` / `LOW`
- Estados: ready, running, blocked, zombie, cancelada
- `sleep`, `yield`, bloqueo/despertar por eventos, `fork`, `wait`/`waitpid`
- Hasta 8 tareas simultáneas, cada una con su propio stack de kernel

### Memoria
- Frame allocator físico por bitmap a partir del mapa Multiboot, con conteo de referencias por frame
- Paginación con páginas grandes de 4 MiB, espacio virtual independiente por proceso
- Copy-on-write en `fork`: las páginas de usuario se comparten como solo lectura y se copian bajo demanda al primer write (4 KB por fallo en vez de 4 MB por fork)
- Ventana de memoria compartida por proceso con segmentos SHM respaldados por frames físicos
- Heap del kernel (`kmalloc`/`kfree`), 256 KB

### Syscalls (`int 0x80`)
Más de 40 syscalls: `open/read/write/close`, `fork`, `exec/execv/execve`, `exit`, `wait/waitpid`, `pipe`, `poll`, `select`, `lseek`, `getpid`, `kill`, señales, identidad (`getuid/geteuid/setuid/setgid/getgroups`), `shm_create/shm_attach/shm_detach/shm_unlink`, `vfs_chown`, `getrlimit/setrlimit`, tiempo real y monotónico.

### Sistema de archivos
- VFS con montajes, rutas absolutas/relativas, normalización, `cwd` por proceso
- **ramfs** (montado en `/`): filesystem en RAM completamente escribible
- **FAT16 / FAT32**: lectura y escritura, LFN, montaje automático sobre particiones detectadas
- **devfs** (`/dev`): `null`, `zero`, `tty`, `console`, dispositivos de bloque
- Permisos tipo UNIX de 9 bits con enforcement en todas las operaciones
- Particionado MBR y GPT automático (hasta 32 particiones)

### Procesos y señales
- ELF32 loader, ring 3, `argv`/`envp` completos, `fork` con copy-on-write y herencia de FDs
- Señales completas: entrega, handlers en userland, `SIGKILL` no bloqueable, `SIGCHLD` + `waitpid`
- Shared memory con herencia de mapeos en `fork` y limpieza automática en `exec`/salida
- Message passing con colas MQ kernel-globales y mensajes acotados
- Adopción de huérfanos por `init` (PID 1), recolección de zombies

### Seguridad y multiusuario
- Separación kernel/user (ring 0 / ring 3), UID/GID/EUID/EGID por proceso
- Permisos de acceso validados en cada syscall VFS
- Base de datos de usuarios/grupos con contraseñas y membresía de grupo
- Comandos de administración: `useradd/del/mod`, `groupadd/del`, `gpasswd`, `passwd`, `su`, `login`, `logout`

### Interrupciones y tiempo
- APIC/IOAPIC con detección automática vía ACPI MADT; fallback a PIC 8259A si no hay APIC
- PIT a 100 Hz como fuente de tick del scheduler (IRQ 0 ruteada por IOAPIC o PIC)
- Timers por proceso, RTC CMOS, `sleep` por syscall, reloj monotónico

### Drivers y debug
- ATA PIO (master/slave), RTC CMOS, serie COM1, GDB remoto integrado
- Panic screen con volcado de registros y backtrace
- Buffer de logs `dmesg` con niveles `DEBUG/INFO/WARN/ERROR`

---

## Requisitos

| Herramienta | Uso |
|---|---|
| `gcc` (soporte `-m32`) | Compilar el kernel |
| `binutils` (`as`, `ld`) | Ensamblar y enlazar |
| `grub-mkrescue` + `xorriso` | Generar la ISO booteable |
| `qemu-system-i386` | Ejecutar la ISO |
| `mtools` *(opcional)* | Crear imágenes de disco FAT para probar el driver ATA |

---

## Toolchain

Hay dos formas soportadas de compilar:

1. Toolchain host actual
- Es la que usa la CI hoy.
- Requiere `gcc` con soporte `-m32`, `binutils`, `grub-mkrescue`, `xorriso` y `qemu-system-i386`.
- Flujo directo:

```bash
make run
```

2. Toolchain cruzada recomendada
- Recomendable si quieres aislar el build de librerías y peculiaridades del host.
- El Makefile acepta un prefijo cruzado vía `CROSS_PREFIX`.
- Ejemplo con toolchain `i686-elf-`:

```bash
make compile CROSS_PREFIX=i686-elf-
make create-iso CROSS_PREFIX=i686-elf-
make run CROSS_PREFIX=i686-elf-
```

También puedes sobrescribir binarios concretos:

```bash
make compile CC=i686-elf-gcc AS=i686-elf-as LD=i686-elf-ld
```

Notas:
- La CI sigue usando la toolchain host con `gcc -m32`.
- `grub-mkrescue` y `grub-file` siguen siendo herramientas del host, aunque el compilador sea cruzado.

---

## Builds reproducibles

El build exporta `SOURCE_DATE_EPOCH` por defecto usando la fecha del último commit disponible, con fallback fijo si no hay metadatos de git. Con eso:

- `kernel.bin` evita variaciones de `build-id` del linker
- la ISO usa timestamps estables al pasar por `grub-mkrescue`/`xorriso`
- el árbol temporal `build/iso` se normaliza al mismo timestamp antes de empaquetar
- las fechas de volumen ISO (creación/modificación/uuid) se fijan explícitamente
- los mtimes Rock Ridge de toda la imagen se reescriben al mismo instante reproducible
- las rutas del workspace no se filtran al binario vía `-ffile-prefix-map`

Comprobación automática:

```bash
make repro-check
```

Si necesitas fijar explícitamente el timestamp reproducible:

```bash
make repro-check SOURCE_DATE_EPOCH=1700000000
```

---

## Compilar y ejecutar

```bash
# Flujo completo: limpia, compila, genera ISO y arranca en QEMU
make run

# Pasos individuales
make compile        # solo compilar
make create-iso     # generar dist/lyth.iso
make execute        # arrancar la ISO en QEMU

# Con trazas de interrupciones
make debug

# Depuración remota con GDB
make gdb-wait       # Terminal 1: QEMU congelado, GDB stub en localhost:1234
make gdb-connect    # Terminal 2: imprime el comando gdb a usar

# Imagen de autotest headless
make create-autotest-iso
make harness-image
```

### Disco FAT para probar el driver ATA

```bash
make disk-fat16     # crea disk.img con partición FAT16
make disk-fat32     # crea disk.img con partición FAT32
make execute        # el kernel monta automáticamente la partición al arrancar
```

### Opciones de display QEMU

```bash
make run                                                   # SDL (por defecto)
make run QEMU_DISPLAY='gtk,show-cursor=off'                # GTK
make run FB_MOUSE_CURSOR=0                                 # sin cursor software del guest
```

---

## Estructura del repositorio

```
arch/x86/           arranque, GDT/TSS, stubs de interrupciones, linker script
kernel/             núcleo: init, IDT, interrupciones, syscalls, scheduler, memoria, señales, ugdb
  mem/              heap, paginación, memoria física
  task/             scheduler y timer (PIT)
drivers/
  console/          terminal lógica, backends VGA y framebuffer, fuente PSF
  input/            teclado PS/2, ratón PS/2, eventos genéricos
  disk/             driver ATA PIO, capa blkdev (MBR/GPT)
  rtc/              reloj en tiempo real (CMOS)
  serial/           salida de debug por COM1
fs/                 VFS, ramfs, FAT16, FAT32, devfs, pipes
userland/shell/     shell interactiva, editor de línea, parser
lib/                helpers de cadenas y UTF-8
include/            headers organizados por subsistema
docs/               documentación técnica detallada
tools/              scripts auxiliares
```

---

## Comandos de la shell

| Categoría | Comandos |
|---|---|
| Sistema | `help`, `clear`, `about`, `uptime`, `date`, `dmesg`, `mem` |
| Procesos | `ps`, `kill`, `nice`, `task`, `wait`, `signal`, `sleep`, `yield`, `getpid`, `exec` |
| Archivos | `ls`, `cat`, `cd`, `pwd`, `touch`, `rm`, `mkdir`, `cp`, `mv`, `rename`, `stat`, `chmod`, `chown`, `grep`, `head`, `tail`, `more`, `less`, `wc`, `find`, `which`, `rmdir`, `cmp`, `diff`, `file`, `du`, `df`, `sync` |
| Discos | `disk` (read / mount / fsck / gpt) |
| Shell | `echo`, `env`, `set`, `unset`, `source`, `history`, `repeat`, `shm`, `shmdemo`, `mq` |
| Entrada | `keymap`, `mouse` |
| Visual | `color`, `theme`, `gfxdemo` |
| Usuarios | `whoami`, `id`, `groups`, `su`, `login`, `logout`, `who`, `users` |
| Administración | `useradd`, `userdel`, `usermod`, `groupadd`, `groupdel`, `gpasswd`, `passwd`, `ulimit` |
| Debug | `elfinfo`, `vfs`, `disk` |
