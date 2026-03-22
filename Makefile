CROSS_PREFIX ?=
CC ?= $(CROSS_PREFIX)gcc
AS ?= $(CROSS_PREFIX)as
LD ?= $(CROSS_PREFIX)ld
PYTHON ?= python3
GRUB_MKRESCUE ?= grub-mkrescue
GRUB_FILE ?= grub-file
SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || echo 1704067200)
ISO_VOLUME_DATE = $(shell date -u -d "@$(SOURCE_DATE_EPOCH)" +%Y%m%d%H%M%S00)

export SOURCE_DATE_EPOCH

BUILD_DIR = build
DIST_DIR = dist
ISO_DIR = $(BUILD_DIR)/iso
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
ISO_IMAGE = $(DIST_DIR)/lyth.iso
GDB_PORT ?= 1234
QEMU = qemu-system-i386
QEMU_DISPLAY ?= sdl,show-cursor=off
FB_MOUSE_CURSOR ?= 0
AUTOTEST ?= 0
QEMU_FLAGS = -boot d -cdrom $(ISO_IMAGE) -m 128 -no-reboot -no-shutdown -vga std -display $(QEMU_DISPLAY)
DISK_IMG  ?= disk.img
AUTOTEST_ISO_IMAGE = $(DIST_DIR)/lyth-autotest.iso

.PHONY: help compile create-iso create-autotest-iso execute debug gdb-wait gdb-connect clean run disk-create disk-fat16 disk-fat32 harness-serial harness-image repro-check

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
PANIC_OBJ = $(BUILD_DIR)/panic.o
UGDB_OBJ = $(BUILD_DIR)/ugdb.o
INTERRUPTS_ASM_OBJ = $(BUILD_DIR)/interrupts_asm.o
TIMER_OBJ = $(BUILD_DIR)/timer.o
HEAP_OBJ = $(BUILD_DIR)/heap.o
PHYSMEM_OBJ = $(BUILD_DIR)/physmem.o
PAGING_OBJ = $(BUILD_DIR)/paging.o
FS_OBJ = $(BUILD_DIR)/fs.o
VFS_OBJ = $(BUILD_DIR)/vfs.o
RAMFS_OBJ = $(BUILD_DIR)/ramfs.o
DEVFS_OBJ = $(BUILD_DIR)/devfs.o
PIPE_OBJ = $(BUILD_DIR)/pipe.o
SYSCALL_OBJ = $(BUILD_DIR)/syscall.o
FBCONSOLE_OBJ = $(BUILD_DIR)/fbconsole.o
ELF_OBJ = $(BUILD_DIR)/elf.o
USERMODE_OBJ = $(BUILD_DIR)/usermode.o
INIT_OBJ     = $(BUILD_DIR)/init.o
ATA_OBJ = $(BUILD_DIR)/ata.o
BLKDEV_OBJ = $(BUILD_DIR)/blkdev.o
FAT16_OBJ = $(BUILD_DIR)/fat16.o
FAT32_OBJ = $(BUILD_DIR)/fat32.o
FAT_FSCK_OBJ = $(BUILD_DIR)/fat_fsck.o
TTY_VFS_OBJ = $(BUILD_DIR)/tty_vfs.o
SERIAL_OBJ = $(BUILD_DIR)/serial.o
KTEST_OBJ = $(BUILD_DIR)/ktest.o
BOOT_TESTS_OBJ = $(BUILD_DIR)/boot_tests.o
RTC_OBJ        = $(BUILD_DIR)/rtc.o

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-omit-frame-pointer -fno-optimize-sibling-calls \
	-ffile-prefix-map=$(CURDIR)=. \
	-DFB_MOUSE_CURSOR_ENABLED=$(FB_MOUSE_CURSOR) \
	-DLYTH_AUTOTEST_ENABLED=$(AUTOTEST) \
	-Iinclude \
	-Iinclude/kernel \
	-Iinclude/kernel/mem \
	-Iinclude/kernel/task \
	-Iinclude/drivers/console \
	-Iinclude/drivers/input \
	-Iinclude/drivers/serial \
	-Iinclude/userland/shell \
	-Iinclude/fs \
	-Iinclude/lib \
	-Iinclude/drivers/disk \
	-Iinclude/drivers/rtc \
	-Iinclude/kernel/tests
LDFLAGS = -m elf_i386 -T arch/x86/linker.ld --build-id=none

FONT_PSF = assets/font.psf
FONT_TOOL = tools/psf2h.py
FONT_HEADER = include/font_psf.h
GRUB_CFG = arch/x86/boot/grub.cfg

OBJS = $(BOOT_OBJ) $(GDT_ASM_OBJ) $(KERNEL_OBJ) $(GDT_OBJ) $(TERMINAL_OBJ) $(CONSOLE_BACKEND_OBJ) $(KEYBOARD_OBJ) $(INPUT_OBJ) $(MOUSE_OBJ) $(SHELL_INPUT_OBJ) $(SHELL_OBJ) $(PARSER_OBJ) $(TASK_OBJ) $(STRING_OBJ) $(UTF8_OBJ) $(IDT_OBJ) $(INTERRUPTS_OBJ) $(KLOG_OBJ) $(PANIC_OBJ) $(UGDB_OBJ) $(INTERRUPTS_ASM_OBJ) $(TIMER_OBJ) $(HEAP_OBJ) $(PHYSMEM_OBJ) $(PAGING_OBJ) $(FS_OBJ) $(VFS_OBJ) $(RAMFS_OBJ) $(DEVFS_OBJ) $(PIPE_OBJ) $(SYSCALL_OBJ) $(FBCONSOLE_OBJ) $(ELF_OBJ) $(USERMODE_OBJ) $(INIT_OBJ) $(ATA_OBJ) $(BLKDEV_OBJ) $(FAT16_OBJ) $(FAT32_OBJ) $(FAT_FSCK_OBJ) $(TTY_VFS_OBJ) $(SERIAL_OBJ) $(KTEST_OBJ) $(BOOT_TESTS_OBJ) $(RTC_OBJ)

$(FONT_HEADER): $(FONT_PSF) $(FONT_TOOL)
	$(PYTHON) $(FONT_TOOL) $(FONT_PSF) $(FONT_HEADER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

help: ## muestra los targets publicos y una descripcion breve
	@awk 'BEGIN {FS = ":.*## "; print "Targets disponibles:"} /^[a-zA-Z0-9_-]+:.*## / {printf "  %-12s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\nVariables utiles:\n"
	@printf "  %-20s %s\n" "CROSS_PREFIX=..." "prefijo de toolchain cruzada, por ejemplo i686-elf-"
	@printf "  %-20s %s\n" "CC/AS/LD=..." "sobrescribe binarios concretos del compilador/assembler/linker"
	@printf "  %-20s %s\n" "SOURCE_DATE_EPOCH=..." "fija timestamps reproducibles del build/ISO"
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
	$(CC) $(CFLAGS) -c kernel/panic.c -o $(PANIC_OBJ)
	$(CC) $(CFLAGS) -c kernel/ugdb.c -o $(UGDB_OBJ)
	$(CC) $(CFLAGS) -c kernel/task/timer.c -o $(TIMER_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/heap.c -o $(HEAP_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/physmem.c -o $(PHYSMEM_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/paging.c -o $(PAGING_OBJ)
	$(CC) $(CFLAGS) -c fs/fs.c -o $(FS_OBJ)
	$(CC) $(CFLAGS) -c fs/vfs.c -o $(VFS_OBJ)
	$(CC) $(CFLAGS) -c fs/ramfs.c -o $(RAMFS_OBJ)
	$(CC) $(CFLAGS) -c fs/devfs.c -o $(DEVFS_OBJ)
	$(CC) $(CFLAGS) -c fs/pipe.c -o $(PIPE_OBJ)
	$(CC) $(CFLAGS) -c kernel/syscall.c -o $(SYSCALL_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/fbconsole.c -o $(FBCONSOLE_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/tty_vfs.c -o $(TTY_VFS_OBJ)
	$(CC) $(CFLAGS) -c drivers/serial/serial.c -o $(SERIAL_OBJ)
	$(CC) $(CFLAGS) -c kernel/elf.c -o $(ELF_OBJ)
	$(CC) $(CFLAGS) -c kernel/usermode.c -o $(USERMODE_OBJ)
	$(CC) $(CFLAGS) -c kernel/init.c -o $(INIT_OBJ)
	$(CC) $(CFLAGS) -c kernel/ktest.c -o $(KTEST_OBJ)
	$(CC) $(CFLAGS) -c kernel/tests/boot_tests.c -o $(BOOT_TESTS_OBJ)
	$(CC) $(CFLAGS) -c drivers/rtc/rtc.c -o $(RTC_OBJ)
	$(CC) $(CFLAGS) -c drivers/disk/ata.c -o $(ATA_OBJ)
	$(CC) $(CFLAGS) -c drivers/disk/blkdev.c -o $(BLKDEV_OBJ)
	$(CC) $(CFLAGS) -c fs/fat16.c -o $(FAT16_OBJ)
	$(CC) $(CFLAGS) -c fs/fat32.c -o $(FAT32_OBJ)
	$(CC) $(CFLAGS) -c fs/fat_fsck.c -o $(FAT_FSCK_OBJ)
	$(AS) --32 arch/x86/gdt.s -o $(GDT_ASM_OBJ)
	$(AS) --32 arch/x86/interrupts.s -o $(INTERRUPTS_ASM_OBJ)
	$(AS) --32 arch/x86/boot/boot.s -o $(BOOT_OBJ)
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJS)

create-iso: compile $(DIST_DIR) ## genera dist/lyth.iso lista para arrancar con GRUB
	rm -rf $(ISO_DIR) $(ISO_IMAGE)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	find $(ISO_DIR) -exec touch -h -d "@$(SOURCE_DATE_EPOCH)" {} +
	$(GRUB_MKRESCUE) -o $(ISO_IMAGE) $(ISO_DIR) -- \
		-volume_date c $(ISO_VOLUME_DATE) \
		-volume_date m $(ISO_VOLUME_DATE) \
		-volume_date uuid $(ISO_VOLUME_DATE) \
		-alter_date_r m $(ISO_VOLUME_DATE) /

create-autotest-iso: AUTOTEST=1
create-autotest-iso: compile $(DIST_DIR) ## genera dist/lyth-autotest.iso con autotest userland embebido
	rm -rf $(ISO_DIR) $(AUTOTEST_ISO_IMAGE)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	find $(ISO_DIR) -exec touch -h -d "@$(SOURCE_DATE_EPOCH)" {} +
	$(GRUB_MKRESCUE) -o $(AUTOTEST_ISO_IMAGE) $(ISO_DIR) -- \
		-volume_date c $(ISO_VOLUME_DATE) \
		-volume_date m $(ISO_VOLUME_DATE) \
		-volume_date uuid $(ISO_VOLUME_DATE) \
		-alter_date_r m $(ISO_VOLUME_DATE) /

execute: create-iso ## arranca la ISO actual en QEMU con cursor del host oculto
	$(QEMU) $(QEMU_FLAGS) $(shell [ -f "$(DISK_IMG)" ] && echo "-hda $(DISK_IMG)")

debug: create-iso ## arranca QEMU con trazas de interrupciones (-d int)
	$(QEMU) $(QEMU_FLAGS) $(shell [ -f "$(DISK_IMG)" ] && echo "-hda $(DISK_IMG)") -d int

gdb-wait: create-iso ## arranca QEMU congelado y expone GDB remoto en localhost:1234
	$(QEMU) $(QEMU_FLAGS) $(shell [ -f "$(DISK_IMG)" ] && echo "-hda $(DISK_IMG)") -s -S

gdb-connect: ## imprime el comando GDB recomendado para conectarse al kernel
	@echo gdb -ex "target remote localhost:$(GDB_PORT)" -ex "symbol-file $(KERNEL_BIN)"

clean: ## borra build/, dist/ y la cabecera generada de la fuente PSF
	rm -rf $(BUILD_DIR) $(DIST_DIR)
	rm -f $(FONT_HEADER)

test: compile create-iso ## ejecuta comprobaciones basicas de compilacion e ISO
	$(GRUB_FILE) --is-x86-multiboot $(KERNEL_BIN)
	test -s $(ISO_IMAGE)

run: clean compile create-iso execute ## flujo completo: limpia, compila, crea ISO y ejecuta

disk-create: ## crea disk.img (32 MB) para probar el driver ATA; luego usa make execute
	@if [ ! -f "$(DISK_IMG)" ]; then \
		dd if=/dev/zero of="$(DISK_IMG)" bs=1M count=32 2>/dev/null; \
		echo "Imagen creada: $(DISK_IMG) (32 MB)"; \
	else \
		echo "Ya existe: $(DISK_IMG) -- borra manualmente para regenerar"; \
	fi

disk-fat16: ## crea disk.img (32 MB) FAT16 plano con archivos de prueba (requiere dosfstools + mtools)
	@echo "Creando imagen FAT16 (32 MB)..."
	rm -f "$(DISK_IMG)"
	dd if=/dev/zero of="$(DISK_IMG)" bs=1M count=32 2>/dev/null
	mkfs.fat -F 16 -n "LYTH" "$(DISK_IMG)"
	@echo "Copiando archivos de prueba..."
	echo "Hola desde FAT16" | MTOOLS_SKIP_CHECK=1 mcopy -i "$(DISK_IMG)" - ::hola.txt
	MTOOLS_SKIP_CHECK=1 mmd -i "$(DISK_IMG)" ::docs
	echo "Lyth OS FAT16 test" | MTOOLS_SKIP_CHECK=1 mcopy -i "$(DISK_IMG)" - ::docs/info.txt
	@echo "Imagen lista: $(DISK_IMG)"
	@echo "  -> arranca con: make execute"
	@echo "  -> en la shell:  vfs ls /hd0    vfs cat /hd0/hola.txt"

disk-fat32: ## crea disk.img (64 MB) FAT32 con LFN de prueba (requiere dosfstools + mtools)
	@echo "Creando imagen FAT32 (64 MB)..."
	rm -f "$(DISK_IMG)"
	dd if=/dev/zero of="$(DISK_IMG)" bs=1M count=64 2>/dev/null
	mkfs.fat -F 32 -n "LYTH32" "$(DISK_IMG)"
	@echo "Copiando archivos de prueba (con LFN)..."
	echo "Hola desde FAT32" | MTOOLS_SKIP_CHECK=1 mcopy -i "$(DISK_IMG)" - ::"archivo largo de prueba.txt"
	MTOOLS_SKIP_CHECK=1 mmd -i "$(DISK_IMG)" ::documentos
	echo "Lyth OS FAT32 test" | MTOOLS_SKIP_CHECK=1 mcopy -i "$(DISK_IMG)" - ::"documentos/info del sistema.txt"
	@echo "Imagen lista: $(DISK_IMG)"
	@echo "  -> arranca con: make execute"
	@echo "  -> en la shell:  vfs ls /hd0    vfs cat /hd0/archivo largo de prueba.txt"

harness-serial: ## ejecuta mini harness por serie y valida boot tests
	./tools/harness_serial.sh

harness-image: ## ejecuta la imagen de autotest headless y valida boot + shell + stack tests
	bash ./tools/harness_image.sh

repro-check: ## recompila dos veces y comprueba que kernel.bin e ISO sean identicos
	@set -eu; \
	tmpdir=$$(mktemp -d); \
	$(MAKE) clean >/dev/null; \
	$(MAKE) create-iso >/dev/null; \
	cp $(KERNEL_BIN) $$tmpdir/kernel.first; \
	cp $(ISO_IMAGE) $$tmpdir/iso.first; \
	$(MAKE) clean >/dev/null; \
	$(MAKE) create-iso >/dev/null; \
	cmp -s $$tmpdir/kernel.first $(KERNEL_BIN); \
	cmp -s $$tmpdir/iso.first $(ISO_IMAGE); \
	echo "Reproducible outputs OK (SOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH))"