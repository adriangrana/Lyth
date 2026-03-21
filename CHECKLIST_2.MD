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
- [ ] UTF-8 básico
- [ ] Consolas virtuales

## 3. Interrupciones y tiempo
- [x] IDT / PIC
- [ ] APIC / IOAPIC
- [ ] HPET
- [ ] Timers por proceso

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

## 7. Memoria
- [x] Paging
- [x] Heap
- [x] Espacio por proceso
- [ ] Protección avanzada

## 8. Syscalls
- [x] Dispatcher
- [x] ABI estable
- [x] Validación segura

## 9. Sistema de archivos
- [x] FS en memoria
- [x] FS escribible
- [x] VFS (CRÍTICO)
- [ ] File descriptors
- [ ] Soporte disco

## 10. Procesos
- [x] Modo usuario
- [x] ELF loader
- [ ] fork / exec
- [ ] Señales

## 11. Drivers
- [x] VGA / framebuffer
- [x] Teclado / ratón
- [ ] Disco (ATA)
- [ ] USB
- [ ] Red

## 12. Red
- [ ] TCP/IP
- [ ] Sockets
- [ ] DHCP / DNS

## 13. Seguridad
- [x] Separación kernel/user
- [ ] Permisos tipo UNIX
- [x] Aislamiento procesos

## 14. Debug
- [x] Logs
- [x] GDB remoto
- [x] Tests automatizados

## 15. Interfaz gráfica
- [x] Framebuffer
- [ ] Ventanas
- [ ] GUI básica

## 16. UX
- [x] Shell usable
- [ ] Config persistente
- [x] Tema visual

## 17. Build & mantenimiento
- [x] Makefile
- [x] CI
- [x] Tests
- [x] Documentación

## 18. Componentes avanzados (añadido)
- [ ] IPC (pipes avanzados, shared memory)
- [ ] Gestión de errores (errno)
- [ ] Gestión de recursos (FD limits)
