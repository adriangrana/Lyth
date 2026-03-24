# Lyth OS — Documentación técnica

## Arquitectura general

```
arch/x86/boot/boot64.s  →  kernel_main()  →  subsistemas (GDT, consola, mem, VFS, scheduler)
                                             →  acpi_init() + apic_init()  →  interrupts_init()
                                             →  pci_init() + ahci_init()
                                             →  tarea init (PID 1) → shell interactiva
```

Subsistemas principales y sus archivos:

| Subsistema | Archivos principales |
|---|---|
| Arranque | `arch/x86/boot/boot64.s`, `arch/x86/boot/grub.cfg` |
| Arranque UEFI | `arch/x86/boot/efi/bootx64.efi` (GRUB EFI) |
| Kernel init | `kernel/kernel.c` |
| GDT / TSS | `kernel/gdt.c`, `arch/x86/gdt64.s` |
| IDT / PIC / PIT | `kernel/idt.c`, `kernel/interrupts.c`, `arch/x86/interrupts64.s` |
| Vídeo | `drivers/video/video.c`, `include/drivers/video/video.h` |
| ACPI / APIC | `kernel/acpi.c`, `kernel/apic.c` |
| SMP | `kernel/smp.c`, `arch/x86/boot/ap_trampoline64.s`, `include/kernel/spinlock.h` |
| HPET | `drivers/hpet/hpet.c` |
| PCI | `drivers/net/pci.c` |
| xHCI (USB 3.x) | `drivers/usb/xhci.c`, `include/drivers/usb/xhci.h` |
| USB HID | `drivers/usb/usb_hid.c`, `include/drivers/usb/usb_hid.h`, `include/drivers/usb/usb.h` |
| Red (E1000) | `drivers/net/e1000.c` |
| Stack de red | `net/ethernet.c`, `net/arp.c`, `net/ipv4.c`, `net/icmp.c`, `net/udp.c`, `net/tcp.c`, `net/socket.c`, `net/dhcp.c`, `net/dns.c` |
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
| AHCI/SATA | `drivers/disk/ahci.c` |
| RTC | `drivers/rtc/rtc.c` |
| Serie | `drivers/serial/serial.c` |
| Shell | `userland/shell/shell.c`, `userland/shell/shell_input.c`, `userland/shell/parser.c` |
| GDB remoto | `kernel/ugdb.c` |
| Panic / assert | `kernel/panic.c`, `kernel/klog.c` |

---

## Flujo de arranque

### BIOS (legacy)
1. GRUB carga el kernel con Multiboot desde el El Torito boot catalog (eltorito.img). `boot64.s` solicita modo gráfico `1024×768×32`, activa long mode (paginación de 4 niveles) y salta a `kernel_main()`.

### UEFI
1. OVMF (u otro firmware UEFI) localiza la entrada El Torito EFI en la ISO, monta el `efi.img` FAT y ejecuta `/EFI/BOOT/BOOTX64.EFI` (GRUB EFI).
2. GRUB EFI usa `search --file /boot/kernel.bin` para localizar el volumen de la ISO, carga el kernel con `multiboot` y transfiere el control.
3. GRUB sale de EFI Boot Services, conmuta a modo protegido 32-bit y entra en el entry point del kernel — el mismo código que en BIOS.

### Común (post-GRUB)
2. `kernel_main()` inicializa en orden: GDT + TSS (64-bit), **IDT temprana** (excepciones + IST1 para double fault), consola (detecta framebuffer 16/24/32 bpp o VGA), mapeo temprano del framebuffer (regiones MMIO >4 GB), video init, memoria física (bitmap Multiboot), heap, VFS (monta ramfs en `/`), ugdb (usuarios/grupos), scheduler.
3. `ata_init()` detecta unidades ATA; `ahci_init()` detecta controladores AHCI/SATA vía PCI; `blkdev` escanea MBR/GPT; las particiones FAT16/FAT32 se montan automáticamente.
4. `acpi_init()` busca el RSDP (EBDA + ROM BIOS), valida el RSDT y parsea la tabla MADT para obtener la dirección del Local APIC, las entradas IOAPIC y los Interrupt Source Overrides. También parsea la FADT para obtener los puertos PM1a/PM1b (shutdown) y el reset register (reboot).
5. `apic_init()` comprueba CPUID, mapea las regiones MMIO (LAPIC en 0xFEE00000, IOAPIC en 0xFEC00000) con `paging_map_mmio()`, deshabilita el PIC 8259A, inicializa el LAPIC (spurious vector 0xFF, TPR a 0) y el IOAPIC (enmascara todas las líneas).
6. `smp_init()` enumera CPUs de la MADT. *(Bootstrap de APs desactivado temporalmente — se mantiene detección y conteo.)*
7. `interrupts_init()` instala gates de IRQ en la IDT (sin re-inicializar para no borrar el handler spurious del APIC), y rutea las IRQs 0, 1, 12 y 14 por el IOAPIC **al LAPIC ID real del BSP** (si APIC está activo) o remapea el PIC 8259A como fallback. Configura el PIT a 100 Hz y habilita IRQs.
8. El scheduler arranca la tarea `init` (PID 1), que inicializa la shell y entra en el bucle de eventos.
9. El loop principal del kernel ejecuta `hlt`; toda la actividad ocurre desde interrupciones y la tarea init.

---

## APIC / IOAPIC

El kernel detecta y usa APIC/IOAPIC cuando el hardware lo soporta, con fallback automático al PIC 8259A.

### Detección (ACPI)
`acpi_init()` busca el RSDP (Root System Description Pointer) en dos regiones de memoria:
- EBDA (Extended BIOS Data Area): puntero en `[0x040E] << 4`
- ROM BIOS: rango `0xE0000`–`0xFFFFF`

A partir del RSDP, valida el RSDT (Root System Description Table) y recorre sus entradas buscando la tabla MADT (`signature = "APIC"`). De la MADT extrae:
- **Dirección base del Local APIC** (normalmente `0xFEE00000`)
- **Entradas IOAPIC**: dirección base (normalmente `0xFEC00000`), ID, GSI base
- **Interrupt Source Overrides (ISO)**: remapeo ISA→GSI con flags de polaridad/trigger

### Inicialización
`apic_init()` verifica CPUID (EDX bit 9) y consume la información MADT:
1. Mapea MMIO del LAPIC y del IOAPIC mediante `paging_map_mmio()` (identity-mapped en el espacio de 4 GB).
2. Deshabilita el PIC 8259A (máscara `0xFF` en ambos chips).
3. Inicializa el LAPIC: spurious interrupt vector `0xFF`, TPR a 0, ESR limpiado.
4. Inicializa el IOAPIC: enmascara todas las líneas de redirección.
5. Construye la tabla ISA→GSI con overrides de la MADT.

### Ruteo de IRQs
`interrupts_init()` comprueba `apic_is_enabled()`:
- **Con APIC**: rutea IRQ 0 (PIT), 1 (teclado), 12 (ratón) y 14 (ATA) mediante `ioapic_route_irq()`, que aplica el mapeo GSI, los flags de polaridad/trigger de la MADT, y dirige la interrupción al **LAPIC ID real del BSP** (obtenido dinámicamente con `apic_get_id()`).
- **Sin APIC**: remapea el PIC 8259A clásico (IRQ 0→vector 32, IRQ 8→vector 40).

### EOI
`send_eoi()` en `interrupts.c` y el handler de ATA usan `apic_eoi()` (write 0 al registro LAPIC EOI en `0xFEE000B0`) si el APIC está activo, o `pic_send_eoi()` como fallback.

---

## SMP (Multicore)

El kernel detecta todos los procesadores lógicos y arranca los APs (Application Processors) a modo protegido con paging.

### Enumeración de CPUs
`acpi_init()` parsea las entradas Local APIC (tipo 0) de la MADT. Cada entrada contiene el ACPI processor ID, el LAPIC ID y un flag de habilitado. Solo se consideran procesadores con el flag enabled.

### Trampoline de arranque
`ap_trampoline64.s` contiene código que transita real→protected→long mode y se copia a la dirección física `0x8000` en runtime. El BSP parchea los campos de datos (kernel GDT descriptor, stack, CR3, entry point) antes de enviar el SIPI.

Flujo del trampoline:
1. **Real mode**: `cli`, carga GDT local de 32-bit, activa PE en CR0.
2. **Protected mode (32-bit GDT)**: carga segmentos 32-bit, carga CR3 del BSP, activa PAE + LME + paging.
3. **Compatibility → Long mode**: far jump via selector de código de 64-bit en la GDT local.
4. **Long mode**: recarga la GDT real del kernel (64-bit descriptor completo con `lgdt`), recarga CS con `lretq`, carga stack y salta a `ap_main()` en C.

> **Nota**: La GDT local de 32-bit es necesaria porque la GDT del kernel tiene el bit L=1 en el segmento de código, lo cual causa #GP en `ljmpl` de 32-bit en hardware real (QEMU lo tolera).

### Secuencia INIT/SIPI
Para cada AP detectado:
1. `apic_send_init()` — envía INIT IPI vía ICR (offset `0x300`/`0x310`).
2. Espera 10 ms (port 0x80 I/O delay).
3. `apic_send_sipi()` — envía Startup IPI con vector `0x08` (página `0x8000`).
4. Si no responde en 200 μs, reintenta un segundo SIPI.
5. Espera hasta 100 ms a que el AP señalice `ap_ready`.

> **Estado actual**: el bootstrap de APs está desactivado temporalmente mientras se depura la compatibilidad con arquitecturas híbridas (P-core/E-core en Intel 12th/13th Gen). La detección y conteo de CPUs permanece activa.

### Estado per-CPU
Cada AP recibe:
- **GDT propia** con TSS único (`gdt_init_ap()`): kernel stack independiente, selectors idénticos al BSP.
- **IDT compartida**: la IDT es global, cargada con `idt_load_table()`.
- **LAPIC propio**: `apic_init_ap()` configura SIVR, TPR y ESR.
- **Halt loop**: los APs entran en `hlt` tras la inicialización, sin participar aún en scheduling.

### Spinlocks
`spinlock.h` implementa spinlocks basados en `xchg` (test-and-set):
- `spinlock_acquire()` / `spinlock_release()`: variante básica.
- `spinlock_acquire_irqsave()` / `spinlock_release_irqrestore()`: variante que salva/restaura EFLAGS (deshabilita IRQs locales).

Protecciones actuales:
- **Heap** (`kmalloc`/`kfree`): protegido con spinlock IRQ-safe.
- **Scheduler**: spinlock declarado (`sched_lock`), listo para cuando los APs participen en multitarea.

---

## ACPI FADT (shutdown/reboot)

`acpi_init()` también parsea la tabla FADT (firma `FACP`) del RSDT. De ella extrae:

- **PM1a/PM1b control block**: puertos I/O para gestión de energía.
- **DSDT**: busca el objeto AML `_S5_` para obtener los valores SLP_TYPa/SLP_TYPb necesarios para el estado S5 (soft-off).
- **Reset register** (FADT rev ≥ 2, flag bit 10): dirección I/O y valor para reiniciar el sistema.

### Shutdown (`acpi_shutdown`)
Escribe `(SLP_TYPa << 10) | SLP_EN` en el puerto PM1a control. Si existe PM1b, también lo escribe ahí.

### Reboot (`acpi_reboot`)
Cadena de fallback:
1. **FADT reset register**: escritura al puerto I/O especificado por la FADT.
2. **PS/2 keyboard controller**: escribe `0xFE` al puerto `0x64`.
3. **Triple fault**: carga un IDTR vacío y ejecuta `int3`.

---

## AHCI/SATA

`drivers/disk/ahci.c` implementa un driver AHCI completo para discos SATA.

### Detección
1. `pci_find_class(0x01, 0x06)` localiza el controlador AHCI.
2. Se habilita bus mastering (`pci_enable_bus_mastering`).
3. BAR5 (ABAR) se mapea vía `paging_map_mmio()` para acceso MMIO.
4. Se activa el bit AHCI Enable (`GHC.AE`).

### Enumeración de puertos
Para cada bit activo en el registro PI (Ports Implemented):
- Comprueba presencia de dispositivo SATA (SStatus DET=3, IPM=1, signature=`0x00000101`).
- Detiene el puerto, configura estructuras DMA (command list, FIS receive, command table en una página de 4 KiB) y lo reinicia.
- Ejecuta `IDENTIFY DEVICE` para obtener modelo y capacidad (LBA48, capped a 32-bit).

### DMA read/write
- Usa `READ DMA EXT` / `WRITE DMA EXT` (comandos LBA48).
- Una sola PRDT entry por comando, chunks de hasta 128 sectores (64 KB).
- Polling de CI (Command Issue) para completitud; detección de TFES para errores.

### Integración
Cada disco se registra con `blkdev_register()` como `sd0`, `sd1`, etc. El probing de particiones MBR/GPT y el auto-mount FAT16/FAT32 funcionan automáticamente.

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
`physmem.c` construye un bitmap de frames (4 KiB cada uno) a partir del mapa de memoria Multiboot. Cada frame lleva un contador de referencias (`uint8_t`, máximo 255) que permite compartir páginas físicas entre procesos. `physmem_ref_frame` incrementa la cuenta, `physmem_unref_frame` la decrementa y libera el frame automáticamente cuando llega a cero.

### Paginación
`paging.c` activa paginación de 4 niveles (PML4 → PDPT → PD → PT) con identity mapping inicial de 4 GB usando páginas de 2 MiB. Expone:
- `paging_create_user_dir()`: crea un directorio de páginas para un proceso de usuario.
- `paging_switch(dir)`: cambia `CR3` en el context switch.
- `paging_validate_user_buffer(addr, size)`: valida que un buffer de usuario sea accesible antes de una syscall.
- `paging_map_mmio(addr)`: mapea regiones MMIO (APIC, AHCI BAR) en el identity map.

Cada proceso de usuario tiene su propio directorio de páginas. El kernel usa identity mapping supervisor-only global.

La región de usuario usa una page table de 4 KiB y deja una guard page no mapeada justo debajo del stack. Si un proceso toca esa página, provoca `Page fault` y el kernel lo reporta como `stack guard page hit`.

Justo debajo de la guard page existe además una ventana fija de shared memory. Los segmentos SHM se respaldan con frames físicos propios del kernel y se remapean dentro de esa ventana en cada proceso que hace `attach`, de modo que varios procesos ven las mismas páginas físicas sin copiar contenido.

### Copy-on-Write (COW)
`fork` ya no copia los 4 MB del espacio de usuario. En su lugar, `paging_cow_clone_user_directory` comparte todas las páginas del padre con el hijo marcándolas como solo lectura con el bit COW (bit 9 del PTE, disponible para el OS). El refcount de cada frame compartido se incrementa.

Cuando cualquiera de los dos procesos escribe en una página COW, la CPU genera un page fault (vector 14, error code `0x07`: write + user + present). El handler en `exception_interrupt_handler` detecta el bit COW y llama a `paging_cow_resolve`:
- Si el refcount es 1 (último usuario), simplemente marca la página como escribible y elimina el bit COW.
- Si el refcount es > 1, asigna un nuevo frame, copia los 4 KB, decrementa el refcount del frame original y mapea el nuevo frame como escribible.

Esto reduce el coste de `fork` de ~1 millón de escrituras (4 MB) a un recorrido de 1024 PTEs, y el patrón `fork`+`exec` apenas copia páginas.

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

`task_fork()` clona la tarea activa usando copy-on-write: comparte las páginas de usuario como solo lectura, hereda la tabla de FDs (con `ref_count` en VFS) y crea un nuevo directorio de páginas. Las páginas se copian bajo demanda al primer write. No copia el heap del kernel.

Cuando el proceso tiene SHM adjunta, `fork` hereda esos mapeos reinsertando las mismas páginas físicas en el hijo. `exec` y la salida del proceso hacen `detach_all`, decrementan referencias y liberan el segmento cuando ya no quedan adjuntos y además fue marcado con `shm_unlink`.

Los zombies permanecen visibles en `ps` hasta que el padre llama a `wait`/`waitpid` o `init` los adopta.

---

## Syscalls

El dispatcher está en `kernel/syscall.c`, activado por `syscall` (MSR-based) o `int 0x80`. Los argumentos llegan en registros (`rdi/rsi/rdx/r10/r8/r9`).

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
`kernel/elf.c` valida la cabecera ELF64 x86_64. `kernel/usermode.c` mapea los segmentos `PT_LOAD` en la región virtual de usuario, construye la pila con `argv`/`envp` y crea una tarea ring 3.

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

- **`keyboard.c`**: traduce scancodes set 1/2, layouts `us`/`es`, `AltGr`, `Caps Lock`, `Num Lock`, teclas extendidas (`Delete`, `Home`, `End`, `Page Up/Down`). Publica `keyboard_event_t` en una cola. También recibe eventos de teclados USB vía `keyboard_handle_usb_report()`.
- **`mouse.c`**: decodifica paquetes PS/2 de 3 bytes, mantiene posición acumulada y estado de botones. También recibe eventos de ratones USB vía `mouse_handle_usb_report()`.
- **`input.c`**: convierte eventos de teclado y ratón en `input_event_t` para consumidores desacoplados.

IRQ1 (teclado) → vector 33; IRQ12 (ratón) → vector 44; xHCI → vector 48.

---

## USB (xHCI + HID)

### Arquitectura

```
usb_hid_init() → registra dispositivos y lanza polling
        ↓
usb_hid_poll() ← invocado periódicamente desde el scheduler
        ↓
xhci_check_event() → consume Transfer Events del Event Ring
        ↓
process_kbd_report() / process_mouse_report() → inyecta en input subsystem
```

### xHCI Host Controller (`drivers/usb/xhci.c`)

El driver xHCI implementa el estándar USB 3.x eXtensible Host Controller Interface:

- **Detección PCI**: busca class `0x0C03` prog_if `0x30`. Soporta múltiples controladores (usa el primero).
- **MMIO**: BAR0 de 64 bits mapeado con `paging_map_mmio()`.
- **BIOS/OS Handoff**: implementa el protocolo de handoff del xHCI (bit BIOS_OWNED → OS_OWNED en capabilities extendidas).
- **Command Ring**: anillo de 256 TRBs con ciclo bit y Link TRB.
- **Event Ring**: anillo de 256 TRBs con ERSTe (Event Ring Segment Table Entry), ERDP cacheado e interrupter 0.
- **DCBAA**: Device Context Base Address Array para hasta 64 slots.
- **Scratchpad Buffers**: asignados según HCSPARAMS2.
- **Interrupción**: IRQ PCI ruteada a vector 48 vía IOAPIC (nivel).

#### Enumeración de dispositivos
1. Escanea todos los puertos (`PORTSC`), detecta Connected (CCS).
2. Resetea el puerto (USB2 warm reset, USB3 hot reset), obtiene velocidad.
3. `Enable Slot` → `Address Device` (BSR=0 para dirección directa).
4. `GET_DESCRIPTOR(DEVICE)` → identifica vendor/device, clase.
5. `SET_CONFIGURATION` → habilita el dispositivo.
6. Parsea el Configuration Descriptor completo: interfaces, endpoints, HID descriptors.
7. `Configure Endpoint` con Input Context para cada Interrupt IN endpoint.
8. Para dispositivos HID Boot: `SET_PROTOCOL(BOOT)` + `SET_IDLE(0)`.

#### Soporte de Hubs USB
- Detecta dispositivos con `bDeviceClass == 0x09` (hub).
- `GET_HUB_DESCRIPTOR`: obtiene número de puertos downstream.
- `SET_PORT_FEATURE(PORT_POWER)`: enciende cada puerto del hub.
- `SET_PORT_FEATURE(PORT_RESET)` + `GET_PORT_STATUS`: resetea y obtiene estado de cada puerto conectado.
- `Address Device` con route string calculado recursivamente (4 bits por nivel, hasta 5 niveles según spec).
- `Evaluate Context` para marcar el slot como hub en el xHCI (Hub bit, número de puertos).
- Enumeración recursiva: los hubs detrás de hubs se enumeran correctamente.

#### Comandos soportados
| Comando | TRB Type |
|---|---|
| No Op | NOOP_CMD |
| Enable Slot | ENABLE_SLOT |
| Address Device | ADDRESS_DEVICE |
| Configure Endpoint | CONFIGURE_EP |
| Evaluate Context | EVALUATE_CONTEXT |
| Reset Endpoint | RESET_EP |
| Set TR Dequeue Pointer | SET_TR_DEQUEUE |

### USB HID (`drivers/usb/usb_hid.c`)

Driver de HID Boot Protocol para teclados y ratones USB:

- **Registro de dispositivos**: escanea los dispositivos xHCI enumerados, registra hasta 8 HID devices (teclados + ratones).
- **Polling asíncrono**: `usb_hid_poll()` se invoca ~100 veces/segundo, consume Transfer Events del Event Ring sin bloquear.
- **Boot Protocol**: reportes de 8 bytes para teclado (modifier + 6 keys), 3 bytes para ratón (buttons + dx + dy).
- **Recuperación de errores**: STALL → Reset Endpoint + Set TR Dequeue Pointer. BABBLE → misma recuperación con buffers de tamaño correcto.
- **Reset diferido**: las recuperaciones de endpoint se difieren fuera del bucle de eventos para no robar eventos de otros dispositivos.
- **Integración con input**: convierte reportes HID en `keyboard_event_t` y `mouse_event_t` para la capa de eventos genérica.

---

## HPET (High Precision Event Timer)

`drivers/hpet/hpet.c` implementa el timer HPET como fuente de temporización de alta resolución.

- **Detección**: busca la tabla ACPI con firma `HPET`. Extrae la dirección base MMIO.
- **Configuración**: lee el periodo del contador (femtosegundos por tick), habilita el contador global.
- **API**: `hpet_read_counter()` devuelve el valor actual del contador; `hpet_delay_us()` espera microsegundos con precisión de nanosegundos.
- **Uso**: empleado para delays precisos en la enumeración USB (port reset, power-on delays) donde el PIT (resolución de 10 ms) es demasiado grueso.

---

## GUI (Compositor)

### Arquitectura

```
gui/compositor.c  →  Backbuffer RGB32 (screen_w × screen_h × 4)
                      ↓
                   Dirty rect list (max 64 rects)
                      ↓
compose_frame()   →  Phase 1: re-render windows with needs_redraw
                  →  Phase 2: sort windows by z_order
                  →  Phase 3: composite each dirty region
                      ↓
present_dirty()   →  memcpy dirty scanlines to framebuffer (32bpp fast path)
```

### Per-window surfaces (`gui/window.c`)
- Cada ventana tiene su propia `gui_surface_t` (pixel buffer respaldado por `physmem_alloc_region`).
- El contenido solo se re-renderiza cuando `needs_redraw == 1` (ej: cambio de texto, nuevo dato).
- **Mover una ventana** no marca `needs_redraw`: solo cambia posición e invalida rects viejos/nuevos.
- Hasta 32 ventanas simultáneas con z-ordering por inserción sort.
- Widgets: botones, labels, callbacks de click; hasta 48 widgets por ventana.

### Dirty rects (`gui/compositor.c`)
- **Dirty list**: hasta 64 rectángulos pendientes por frame.
- Se unen rects que se solapan para evitar fragmentación excesiva.
- Si overflow → marca pantalla completa (fallback seguro).
- Solo las ventanas que intersectan un dirty rect se componen; el resto se saltan.

### Cursor software (`gui/cursor.c`)
- Sprite de 12×18 píxeles (outline + fill).
- Save-under: antes de dibujar, guarda los píxeles de debajo; al borrar, los restaura.
- Solo invalida su bounding rect (no la pantalla entera).

### Escritorio (`gui/desktop.c`)
- Superficie cacheada con gradiente de fondo + taskbar inferior.
- Taskbar: botón "Lyth", lista de ventanas abiertas, reloj RTC actualizado cada segundo.
- Menú de inicio con 5 entradas (Terminal, Task Manager, System Info, Network, Settings).
- Tema Catppuccin Mocha (`#1E1E2E` bg, `#CDD6F4` text, `#3B82F6` accent).

### Apps integradas (`gui/apps/`)
- **Terminal** (`terminal.c`): grid 80×24, scrollback 256 líneas, comandos built-in (help, clear, uptime, mem, ps, uname, echo).
- **Task Manager** (`taskman.c`): lista de procesos vía `task_list()`, barra de memoria.
- **System Info** (`sysinfo.c`): OS, arquitectura, display, memoria, uptime, tareas.
- **Network Config** (`netcfg.c`): interfaces vía `netif_get()`, MAC/IP/mask/GW/DNS.
- **Settings** (`settings.c`): métricas de rendimiento: FPS, frame time, compose/present µs, dirty rects, píxeles copiados.

### Optimización de drag
- Coalescing de eventos de mouse: se acumula el delta y solo se procesa la posición final por frame.
- Invalidación separada de old rect + new rect (sin unión bruta).
- `needs_redraw` nunca se activa durante drag → la superficie se reutiliza tal cual.
- Frame pacing desactivado durante drag para máxima responsividad.

### Presentación (`drivers/console/fbconsole.c`)
- `fb_present_rgb32()`: copia backbuffer al framebuffer hardware. Fast path con `memcpy` por fila para BGR32; conversión por pixel para otros formatos.
- En modo dirty: solo copia scanlines dentro de dirty rects (32bpp direct memcpy).

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
| `make create-iso` | Genera `dist/lyth.iso` híbrida BIOS + UEFI |
| `make create-autotest-iso` | Genera `dist/lyth-autotest.iso` con `AUTOTEST=1` y script de prueba embebido |
| `make execute` | Arranca la ISO en QEMU (BIOS, SDL, cursor host oculto) |
| `make execute-uefi` | Arranca la ISO en QEMU con firmware OVMF (UEFI) |
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
- Flujo actualmente validado en CI: toolchain host con `gcc -m64`, `as`, `ld`, `grub-mkrescue` y `grub-file` del sistema.
- Flujo recomendado para desarrollo aislado: toolchain cruzada ELF x86_64, por ejemplo `x86_64-elf-gcc`, `x86_64-elf-as`, `x86_64-elf-ld`.
- El Makefile admite ambas rutas:
    - host: `make run`
    - cruzada por prefijo: `make run CROSS_PREFIX=x86_64-elf-`
    - cruzada por binario explícito: `make compile CC=x86_64-elf-gcc AS=x86_64-elf-as LD=x86_64-elf-ld`
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

## Red (Networking)

### Arquitectura de la pila de red

```
Aplicación (shell: ping, nc, dhcp, nslookup, arp, ifconfig, netstat)
        │
   net/socket.c  ─── capa BSD-like (net_socket / net_send / net_recv …)
        │
   ┌────┴────┐
net/udp.c  net/tcp.c  ─── transporte
        │
   net/ipv4.c  ─── red (routing, checksum, fragmentación básica)
        │
   net/arp.c   ─── resolución MAC
        │
   net/ethernet.c  ─── enlace (Ethernet II)
        │
   drivers/net/e1000.c  ─── driver NIC
        │
   drivers/net/pci.c  ─── enumeración PCI
```

### PCI
- Acceso por puertos I/O (0xCF8 / 0xCFC).
- Escaneo de hasta 256 buses × 32 slots × 8 funciones.
- `pci_find_device(vendor, device)` busca en la tabla enumerada.
- `pci_enable_bus_mastering()` habilita DMA en el dispositivo.

### Driver E1000 (Intel 82540EM)
- Dispositivo PCI 0x8086:0x100E (adaptador por defecto de QEMU `-netdev user`).
- Acceso MMIO: BAR0 se mapea con `paging_map_mmio()`.
- RX: 32 descriptores DMA de 2048 bytes cada uno, en anillo circular.
- TX: 8 descriptores DMA en anillo circular.
- IRQ11 → vector 43 vía IOAPIC. El handler lee ICR y procesa descriptores RX pendientes.
- MAC se obtiene primero del EEPROM; si falla, se lee de RAL/RAH.
- `e1000_send(data, len)` copia al buffer DMA y avanza el tail del anillo TX.
- `e1000_irq_handler()` itera RX descriptores completados, entrega a `netif_rx()`.

### Ethernet (`net/ethernet.c`)
- `eth_rx()`: desmultiplexa por EtherType → ARP (0x0806) o IPv4 (0x0800).
- `eth_send()`: construye trama Ethernet II (dst MAC + src MAC + EtherType + payload).

### ARP (`net/arp.c`)
- Caché estática de 32 entradas con política LRU para evicción.
- `arp_rx()`: procesa REQUEST (responde si la IP destino es la local) y REPLY (actualiza caché).
- `arp_resolve()`: busca en caché; si no hay entrada, envía ARP request broadcast.
- Detección de broadcast: si la IP destino coincide con la broadcast de la subred, devuelve FF:FF:FF:FF:FF:FF.

### IPv4 (`net/ipv4.c`)
- Tabla de rutas estática (8 entradas), longest-prefix match.
- `ip4_send()`: calcula checksum, resuelve MAC vía ARP, encapsula en Ethernet.
- `ip4_rx()`: valida versión/IHL/checksum, despacha a ICMP (proto 1), UDP (proto 17), TCP (proto 6).
- `ip4_checksum()`: ones' complement estándar RFC 1071.
- `ip4_to_str()`: formatea dirección a cadena legible.

### ICMP (`net/icmp.c`)
- Echo Reply automático (tipo 0) ante Echo Request (tipo 8).
- `icmp_send_echo()`: construye paquete ICMP Echo Request con id/seq configurables.
- Callback registrable para respuestas (usado por el comando `ping`).

### UDP (`net/udp.c`)
- 16 sockets estáticos con binding por puerto.
- `udp_send()`: construye datagrama UDP, envía por `ip4_send()`.
- `udp_rx()`: despacha a socket local por puerto destino; puertos especiales (DHCP 68, DNS client) se entregan directamente.
- `udp_recvfrom()`: bloquea (spin) hasta que llega un paquete o se alcanza timeout.

### TCP (`net/tcp.c`)
- 16 sockets estáticos con máquina de estados completa (RFC 793):
  CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT,
  CLOSED → LISTEN → SYN_RECEIVED → ESTABLISHED → CLOSE_WAIT → LAST_ACK.
- MSS = 1460, window = 8192 bytes.
- Checksum con pseudo-header (src IP, dst IP, proto, TCP length).
- `tcp_connect()`: three-way handshake activo.
- `tcp_listen()` / `tcp_accept()`: servidor pasivo.
- `tcp_send()` / `tcp_recv()`: transferencia sobre conexión establecida.
- `tcp_close()`: cierre ordenado con FIN.
- `tcp_tick()`: limpieza de sockets en TIME_WAIT (llamado periódicamente).

### Sockets (`net/socket.c`)
- Capa BSD-like unificada: `net_socket()`, `net_bind()`, `net_connect()`, `net_listen()`, `net_accept()`, `net_send()`, `net_recv()`, `net_sendto()`, `net_recvfrom()`, `net_close()`.
- 32 sockets kernel; cada uno referencia un socket UDP o TCP interno.
- `net_init()`: inicializa todos los subsistemas (ARP, IPv4, UDP, TCP, DHCP, DNS).

### DHCP (`net/dhcp.c`)
- Discover → Offer → Request → Ack (RFC 2131).
- Configura automáticamente IP, máscara, gateway y servidor DNS de la interfaz.
- Inserta rutas (red local + default gateway) en la tabla IPv4.

### DNS (`net/dns.c`)
- Stub resolver: envía query tipo A al servidor DNS configurado (puerto 53).
- Caché estática de 16 entradas.
- Codificación de nombres DNS (labels con longitud prefijada).
- Timeout de 3 segundos.

### Comandos de shell

| Comando | Descripción |
|---|---|
| `ping <ip>` | Envía ICMP Echo Request y espera Reply |
| `ifconfig` | Muestra interfaces de red registradas |
| `netstat` | Lista sockets UDP y TCP abiertos |
| `arp` | Muestra la tabla de caché ARP |
| `dhcp` | Ejecuta DHCP en la interfaz por defecto |
| `nslookup <host>` | Resuelve nombre de dominio vía DNS |
| `nc <ip> <port> <msg>` | Envía mensaje UDP a destino |
