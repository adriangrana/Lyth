# Lyth OS - Checklist Refinado

## 1. Arranque y arquitectura base
- [x] GRUB + Multiboot
- [x] Kernel ELF32
- [x] Stack inicial
- [ ] Soporte hardware real
- [ ] UEFI
- [ ] SMP (multicore)

## 2. Consola y salida
- [x] VGA + framebuffer
- [x] Consola desacoplada
- [x] UTF-8 básico
- [x] Consolas virtuales

## 3. Interrupciones y tiempo
- [x] IDT / PIC
- [ ] APIC / IOAPIC
- [ ] HPET
- [x] Timers por proceso
- [x] RTC / reloj del sistema
- [x] sleep por syscall
- [x] Fuente de tiempo monotónica

## 4. Input
- [x] Teclado completo
- [x] Ratón
- [x] Sistema de input unificado

## 5. Shell
- [x] Parser
- [x] Pipes / redirección
- [x] Scripts
- [x] Variables de entorno

## 6. Scheduler
- [x] Preemptivo
- [x] Prioridades
- [x] Bloqueo por eventos
- [x] Idle task
- [x] Cola ready / blocked robusta

## 7. Memoria
- [x] Paging
- [x] Heap
- [x] Espacio por proceso
- [ ] Protección avanzada
- [ ] Kernel heap robusto
- [ ] Copy-on-write
- [ ] mmap / regiones mapeadas
- [x] Guard pages para stacks

## 8. Syscalls
- [x] Dispatcher
- [x] Syscalls básicas
- [x] ABI estable
- [x] Validación de punteros
- [x] open / read / write / close
- [x] Gestión de file descriptors (FD)
- [x] fork
- [x] exec
- [x] exit
- [x] wait (espera a task por id)
- [x] getpid / sleep / kill
- [x] errno por proceso (get_errno syscall)
- [x] getuid / geteuid / getgid / getegid
- [x] setuid / setgid
- [x] getgroups / setgroups
- [x] sigpending / sigprocmask
- [x] pipe (syscall POSIX)
- [x] lseek (seek por file descriptor)
- [x] execve (con argv y envp)

## 9. Sistema de archivos
- [x] FS en memoria
- [x] FS escribible
- [x] VFS (CRÍTICO)
- [x] Resolución de rutas en VFS
- [x] Soporte de rutas relativas
- [x] Normalización de rutas
- [x] Directorio actual por proceso (cwd)
- [x] mount / unmount
- [x] Puntos de montaje
- [x] Integración de FD con VFS
- [x] Tabla global de archivos abiertos
- [x] Flags de apertura y modos de acceso
- [x] Operaciones de directorio (open/read dir)
- [x] rename / unlink / stat
- [x] Caché de bloques
- [x] Caché de vnodes / dentries
- [x] Integración de dispositivos en VFS (/dev)
- [x] FAT16 read-only
- [x] FAT16 write
- [x] FAT32 read-only
- [x] FAT32 write
- [x] Soporte LFN (Long File Names)
- [x] Soporte disco real
- [x] Soporte de particiones MBR
- [x] Soporte de particiones GPT
- [x] Verificación básica de integridad del filesystem
- [x] /dev/null, /dev/zero, /dev/tty, /dev/console
- [x] vfs_chown (cambio de propietario por VFS)
- [x] Estructura inicial Unix en ramfs (/etc/motd, /etc/os-release, /home/user/)

## 10. Procesos
- [x] Modo usuario
- [x] ELF loader
- [x] Estructura de proceso (task_struct)
- [x] PID / gestión de identificadores
- [x] Estados de proceso (ready, running, blocked, zombie)
- [x] Context switch completo
- [x] Creación básica de procesos
- [x] Proceso init / primer proceso userland
- [x] Espacio de direcciones independiente por proceso
- [x] Tabla de file descriptors por proceso
- [x] Herencia de FDs en fork
- [x] fork básico
- [x] exec
- [x] argv / envp completos en exec
- [x] exit
- [x] wait / waitpid (task_wait_id)
- [x] Recolección de procesos zombie
- [x] Señales básicas
- [x] Entrega de señales
- [x] Manejo de señales en userland
- [x] Handlers de señales en userland (salto/retorno seguro y preservación de contexto)
- [x] Máscara / bloqueo de señales (incluye no bloqueables/no capturables como SIGKILL y semántica de pendientes)
- [x] Acciones por defecto de señales e integración SIGCHLD + wait/waitpid (notificación vs recolección)

## 11. Drivers
- [x] VGA / framebuffer
- [x] Teclado / ratón
- [x] ATA PIO
- [x] Capa de dispositivo de bloques
- [x] Disco
- [ ] USB
- [ ] Red
- [x] RTC
- [x] Serie (COM1) para debug

## 12. Red
- [ ] ARP
- [ ] ICMP
- [ ] UDP
- [ ] TCP/IP
- [ ] Sockets
- [ ] DHCP / DNS

## 13. Seguridad
- [x] Separación kernel/user
- [x] Aislamiento procesos
- [x] Permisos tipo UNIX
- [x] Usuarios y grupos
- [x] UID / GID por proceso
- [x] Validación de acceso en syscalls
- [x] euid / egid (identidad efectiva)
- [x] setuid / setgid por proceso
- [x] Contraseñas por usuario (passwd)
- [x] Membresía de grupos (gpasswd)
- [x] Administración de usuarios (useradd, userdel, usermod)
- [x] Administración de grupos (groupadd, groupdel)
- [x] login / logout explícito con autenticación

## 14. Debug
- [x] Logs
- [x] GDB remoto
- [x] Tests automatizados
- [x] Panic screen / crash dump
- [x] Backtrace del kernel
- [x] Assertions del kernel
- [x] Logging por niveles

## 15. Interfaz gráfica
- [x] Framebuffer
- [ ] Ventanas
- [ ] GUI básica
- [ ] Compositor básico
- [ ] Eventos de ventana / input gráfico
- [ ] Widgets básicos

## 16. UX
- [x] Shell usable
- [x] Tema visual
- [x] Config persistente
- [x] Persistencia por usuario
- [x] Utilidades básicas del sistema (cp, mv, grep, ulimit, nice, renice, id, whoami, groups, chown, gfxdemo, elfinfo, alarm, pidof, killall, fg, bg, time, stackbomb, stackok...)
- [x] Ayuda integrada (help)
- [x] who / users (sesiones activas)
- [x] rmdir, find, which, head, tail, more/less, wc, cmp/diff, file, du, df, sync (incluyen uso en pipes de shell donde aplica)

## 17. Build & mantenimiento
- [x] Makefile
- [x] CI
- [x] Tests
- [x] Documentación
- [x] Builds reproducibles
- [x] Versionado del kernel
- [x] Imagen de prueba automatizada
- [x] Toolchain cruzada documentada

## 18. Componentes avanzados
- [x] IPC (pipes avanzados)
- [x] Shared memory
- [x] Message passing
- [x] poll / select
- [x] MQ integradas con poll / select via FD
- [x] MQ con syscalls temporizadas de userland
- [x] Gestión de errores (errno)
- [x] Gestión de recursos (FD limits)