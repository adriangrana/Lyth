# Lyth OS Checklist

## 1. Arranque y base mínima
- [x] Arrancar con GRUB + Multiboot
- [x] Tener boot.s y salto correcto a kernel_main
- [x] Enlazar kernel ELF32 con linker.ld
- [x] Arrancar en x86 de 32 bits
- [x] Crear stack inicial del kernel
- [x] Bucle principal del kernel
- [x] Build automático con Makefile
- [x] Generar ISO arrancable
- [x] Ejecutar en QEMU
- [ ] Soporte para arranque en hardware real
- [ ] Soporte UEFI
- [ ] Soporte SMP / múltiples CPUs

## 2. Consola y salida básica
- [x] Consola VGA texto básica
- [x] Fallback VGA cuando no hay framebuffer
- [x] Consola framebuffer propia
- [x] Limpiar pantalla
- [x] Scroll de consola
- [x] Escribir caracteres individuales
- [x] Imprimir cadenas
- [x] Cambiar color de texto
- [x] Fuente bitmap funcional
- [x] Fuente 8x16 real vía PSF
- [x] Mostrar diagnóstico de framebuffer al arranque
- [x] Cursor lógico básico
- [x] Cursor parpadeante robusto sin borrar texto subyacente
- [x] Márgenes/padding de consola
- [x] Prompt visual mejorado
- [x] Buffer de pantalla independiente del renderer
- [x] Backend de consola desacoplado
- [ ] Soporte UTF-8 básico
- [ ] Consolas virtuales múltiples

## 3. Interrupciones y temporización
- [x] IDT
- [x] PIC remapeado
- [x] IRQ0 timer
- [x] IRQ1 teclado
- [x] int 0x80 para syscalls
- [x] PIT configurado a 100 Hz
- [x] Contador global de ticks
- [x] Manejadores de excepciones CPU completos
- [x] Page fault handler útil
- [ ] APIC / IOAPIC
- [ ] HPET

## 4. Teclado e input
- [x] Driver de teclado básico
- [x] Lectura de scancodes
- [x] Cola de eventos
- [x] Editor de línea avanzado
- [x] Layouts de teclado
- [x] Soporte para `AltGr`
- [x] Soporte para `Caps Lock`
- [x] Teclas extendidas (`Insert`, `Delete`, `Home`, `End`, `Page Up`, `Page Down`)
- [x] Teclado numérico con `Num Lock`
- [x] Modo overwrite con `Insert`
- [x] Ratón PS/2
- [x] Input genérico

## 5. Shell
- [x] Shell interactiva
- [x] Parser tipo argv
- [x] Historial
- [x] Autocompletado
- [x] Comandos básicos
- [x] Variables de entorno
- [x] Pipes
- [x] Redirección
- [x] Scripts

## 6. Scheduler
- [x] Scheduler cooperativo
- [x] Sleep / yield
- [x] Cancelación de tareas
- [x] Multitarea preemptiva
- [x] Prioridades
- [x] Bloqueo por eventos

## 7. Memoria
- [x] Heap básico
- [x] Gestor de memoria física
- [x] Paginación
- [x] Espacio virtual por proceso

## 8. Syscalls
- [x] Dispatcher
- [x] Syscalls básicas
- [x] ABI estable
- [x] Validación de punteros

## 9. Filesystem
- [x] FS en memoria (RO)
- [x] FS escribible
- [ ] VFS
- [ ] Soporte disco real

## 10. Procesos
- [x] Modo usuario
- [x] ELF loader
- [ ] fork/exec
- [ ] Señales

## 11. Drivers
- [x] VGA
- [x] Framebuffer
- [x] PIT
- [x] Teclado
- [ ] Disco
- [ ] USB
- [ ] Red

## 12. Red
- [ ] TCP/IP stack
- [ ] Sockets
- [ ] DHCP / DNS

## 13. Seguridad
- [x] Separación kernel/user
- [ ] Permisos
- [x] Protección memoria

## 14. Debug
- [x] QEMU debug
- [x] Diagnóstico framebuffer
- [x] Logs avanzados
- [x] GDB remoto

## 15. Interfaz gráfica
- [x] Framebuffer
- [x] Primitivas gráficas
- [ ] Ventanas
- [ ] GUI completa

## 16. UX
- [x] Shell usable
- [x] Cursor mejorado
- [x] Tema visual
- [x] Cursor distinto en modo overwrite
- [ ] Config persistente

## 17. Build & mantenimiento
- [x] Makefile
- [x] Pipeline PSF
- [x] CI
- [x] Tests
- [x] Documentación técnica

## 18. Roadmap siguiente

### Fase 1: estabilidad base
- [x] Cerrar pendientes de documentación técnica
- [ ] Añadir pruebas automatizadas para módulos aislados
- [x] Reforzar validación de punteros en syscalls y loader ELF
- [ ] Reducir regresiones del teclado y la consola con casos de prueba manuales

### Fase 2: experiencia de usuario
- [ ] Config persistente para temas y layout de teclado
- [x] Mejorar autocompletado con rutas y scripts
- [x] Revisar edición de línea para selección, overwrite y pegado
- [ ] Pulir cursor y comportamiento visual en framebuffer/VGA

### Fase 3: sistema operativo más completo
- [ ] VFS sobre el FS en memoria
- [ ] Soporte de disco real
- [ ] Ventanas y GUI básica
- [ ] Red inicial: TCP/IP, sockets y DHCP/DNS
