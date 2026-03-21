CC = gcc
AS = as
LD = ld
PYTHON = python3

BUILD_DIR = build
DIST_DIR = dist
ISO_DIR = $(BUILD_DIR)/iso
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
ISO_IMAGE = $(DIST_DIR)/lyth.iso
GDB_PORT ?= 1234
QEMU = qemu-system-i386
QEMU_DISPLAY ?= sdl,show-cursor=off
FB_MOUSE_CURSOR ?= 0
QEMU_FLAGS = -boot d -cdrom $(ISO_IMAGE) -m 128 -no-reboot -no-shutdown -vga std -display $(QEMU_DISPLAY)

.PHONY: help compile create-iso execute debug gdb-wait gdb-connect clean run

BOOT_OBJ = $(BUILD_DIR)/boot.o
GDT_ASM_OBJ = $(BUILD_DIR)/gdt_asm.o
KERNEL_OBJ = $(BUILD_DIR)/kernel.o
GDT_OBJ = $(BUILD_DIR)/gdt.o
TERMINAL_OBJ = $(BUILD_DIR)/terminal.o
CONSOLE_BACKEND_OBJ = $(BUILD_DIR)/console_backend.o
KEYBOARD_OBJ = $(BUILD_DIR)/keyboard.o
INPUT_OBJ = $(BUILD_DIR)/input.o
MOUSE_OBJ = $(BUILD_DIR)/mouse.o
SHELL_INPUT_OBJ = $(BUILD_DIR)/shell_input.o
SHELL_OBJ = $(BUILD_DIR)/shell.o
PARSER_OBJ = $(BUILD_DIR)/parser.o
TASK_OBJ = $(BUILD_DIR)/task.o
STRING_OBJ = $(BUILD_DIR)/string.o
UTF8_OBJ = $(BUILD_DIR)/utf8.o
IDT_OBJ = $(BUILD_DIR)/idt.o
INTERRUPTS_OBJ = $(BUILD_DIR)/interrupts.o
KLOG_OBJ = $(BUILD_DIR)/klog.o
INTERRUPTS_ASM_OBJ = $(BUILD_DIR)/interrupts_asm.o
TIMER_OBJ = $(BUILD_DIR)/timer.o
HEAP_OBJ = $(BUILD_DIR)/heap.o
PHYSMEM_OBJ = $(BUILD_DIR)/physmem.o
PAGING_OBJ = $(BUILD_DIR)/paging.o
FS_OBJ = $(BUILD_DIR)/fs.o
VFS_OBJ = $(BUILD_DIR)/vfs.o
RAMFS_OBJ = $(BUILD_DIR)/ramfs.o
SYSCALL_OBJ = $(BUILD_DIR)/syscall.o
FBCONSOLE_OBJ = $(BUILD_DIR)/fbconsole.o
ELF_OBJ = $(BUILD_DIR)/elf.o
USERMODE_OBJ = $(BUILD_DIR)/usermode.o

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-omit-frame-pointer -fno-optimize-sibling-calls \
	-DFB_MOUSE_CURSOR_ENABLED=$(FB_MOUSE_CURSOR) \
	-Iinclude \
	-Iinclude/kernel \
	-Iinclude/kernel/mem \
	-Iinclude/kernel/task \
	-Iinclude/drivers/console \
	-Iinclude/drivers/input \
	-Iinclude/userland/shell \
	-Iinclude/fs \
	-Iinclude/lib
LDFLAGS = -m elf_i386 -T arch/x86/linker.ld

FONT_PSF = assets/font.psf
FONT_TOOL = tools/psf2h.py
FONT_HEADER = include/font_psf.h
GRUB_CFG = arch/x86/boot/grub.cfg

OBJS = $(BOOT_OBJ) $(GDT_ASM_OBJ) $(KERNEL_OBJ) $(GDT_OBJ) $(TERMINAL_OBJ) $(CONSOLE_BACKEND_OBJ) $(KEYBOARD_OBJ) $(INPUT_OBJ) $(MOUSE_OBJ) $(SHELL_INPUT_OBJ) $(SHELL_OBJ) $(PARSER_OBJ) $(TASK_OBJ) $(STRING_OBJ) $(UTF8_OBJ) $(IDT_OBJ) $(INTERRUPTS_OBJ) $(KLOG_OBJ) $(INTERRUPTS_ASM_OBJ) $(TIMER_OBJ) $(HEAP_OBJ) $(PHYSMEM_OBJ) $(PAGING_OBJ) $(FS_OBJ) $(VFS_OBJ) $(RAMFS_OBJ) $(SYSCALL_OBJ) $(FBCONSOLE_OBJ) $(ELF_OBJ) $(USERMODE_OBJ)

$(FONT_HEADER): $(FONT_PSF) $(FONT_TOOL)
	$(PYTHON) $(FONT_TOOL) $(FONT_PSF) $(FONT_HEADER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

help: ## muestra los targets publicos y una descripcion breve
	@awk 'BEGIN {FS = ":.*## "; print "Targets disponibles:"} /^[a-zA-Z0-9_-]+:.*## / {printf "  %-12s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\nVariables utiles:\n"
	@printf "  %-20s %s\n" "QEMU_DISPLAY=..." "sobrescribe el backend/opciones de video de QEMU"
	@printf "  %-20s %s\n" "FB_MOUSE_CURSOR=0|1" "activa o desactiva el cursor software del guest"

compile: $(FONT_HEADER) $(BUILD_DIR) ## compila y enlaza el kernel en build/kernel.bin
	$(CC) $(CFLAGS) -c kernel/kernel.c -o $(KERNEL_OBJ)
	$(CC) $(CFLAGS) -c kernel/gdt.c -o $(GDT_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/terminal.c -o $(TERMINAL_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/console_backend.c -o $(CONSOLE_BACKEND_OBJ)
	$(CC) $(CFLAGS) -c drivers/input/keyboard.c -o $(KEYBOARD_OBJ)
	$(CC) $(CFLAGS) -c drivers/input/input.c -o $(INPUT_OBJ)
	$(CC) $(CFLAGS) -c drivers/input/mouse.c -o $(MOUSE_OBJ)
	$(CC) $(CFLAGS) -c userland/shell/shell_input.c -o $(SHELL_INPUT_OBJ)
	$(CC) $(CFLAGS) -c userland/shell/shell.c -o $(SHELL_OBJ)
	$(CC) $(CFLAGS) -c userland/shell/parser.c -o $(PARSER_OBJ)
	$(CC) $(CFLAGS) -c kernel/task/task.c -o $(TASK_OBJ)
	$(CC) $(CFLAGS) -c lib/string.c -o $(STRING_OBJ)
	$(CC) $(CFLAGS) -c lib/utf8.c -o $(UTF8_OBJ)
	$(CC) $(CFLAGS) -c kernel/idt.c -o $(IDT_OBJ)
	$(CC) $(CFLAGS) -c kernel/interrupts.c -o $(INTERRUPTS_OBJ)
	$(CC) $(CFLAGS) -c kernel/klog.c -o $(KLOG_OBJ)
	$(CC) $(CFLAGS) -c kernel/task/timer.c -o $(TIMER_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/heap.c -o $(HEAP_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/physmem.c -o $(PHYSMEM_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/paging.c -o $(PAGING_OBJ)
	$(CC) $(CFLAGS) -c fs/fs.c -o $(FS_OBJ)
	$(CC) $(CFLAGS) -c fs/vfs.c -o $(VFS_OBJ)
	$(CC) $(CFLAGS) -c fs/ramfs.c -o $(RAMFS_OBJ)
	$(CC) $(CFLAGS) -c kernel/syscall.c -o $(SYSCALL_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/fbconsole.c -o $(FBCONSOLE_OBJ)
	$(CC) $(CFLAGS) -c kernel/elf.c -o $(ELF_OBJ)
	$(CC) $(CFLAGS) -c kernel/usermode.c -o $(USERMODE_OBJ)
	$(AS) --32 arch/x86/gdt.s -o $(GDT_ASM_OBJ)
	$(AS) --32 arch/x86/interrupts.s -o $(INTERRUPTS_ASM_OBJ)
	$(AS) --32 arch/x86/boot/boot.s -o $(BOOT_OBJ)
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJS)

create-iso: compile $(DIST_DIR) ## genera dist/lyth.iso lista para arrancar con GRUB
	rm -rf $(ISO_DIR) $(ISO_IMAGE)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR)

execute: create-iso ## arranca la ISO actual en QEMU con cursor del host oculto
	$(QEMU) $(QEMU_FLAGS)

debug: create-iso ## arranca QEMU con trazas de interrupciones (-d int)
	$(QEMU) $(QEMU_FLAGS) -d int

gdb-wait: create-iso ## arranca QEMU congelado y expone GDB remoto en localhost:1234
	$(QEMU) $(QEMU_FLAGS) -s -S

gdb-connect: ## imprime el comando GDB recomendado para conectarse al kernel
	@echo gdb -ex "target remote localhost:$(GDB_PORT)" -ex "symbol-file $(KERNEL_BIN)"

clean: ## borra build/, dist/ y la cabecera generada de la fuente PSF
	rm -rf $(BUILD_DIR) $(DIST_DIR)
	rm -f $(FONT_HEADER)

test: compile create-iso ## ejecuta comprobaciones basicas de compilacion e ISO
	grub-file --is-x86-multiboot $(KERNEL_BIN)
	test -s $(ISO_IMAGE)

run: clean compile create-iso execute ## flujo completo: limpia, compila, crea ISO y ejecuta