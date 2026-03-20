CC = gcc
AS = as
LD = ld
PYTHON = python3

BUILD_DIR = build
DIST_DIR = dist
ISO_DIR = $(BUILD_DIR)/iso
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
ISO_IMAGE = $(DIST_DIR)/lyth.iso

BOOT_OBJ = $(BUILD_DIR)/boot.o
KERNEL_OBJ = $(BUILD_DIR)/kernel.o
TERMINAL_OBJ = $(BUILD_DIR)/terminal.o
CONSOLE_BACKEND_OBJ = $(BUILD_DIR)/console_backend.o
KEYBOARD_OBJ = $(BUILD_DIR)/keyboard.o
SHELL_INPUT_OBJ = $(BUILD_DIR)/shell_input.o
SHELL_OBJ = $(BUILD_DIR)/shell.o
PARSER_OBJ = $(BUILD_DIR)/parser.o
TASK_OBJ = $(BUILD_DIR)/task.o
STRING_OBJ = $(BUILD_DIR)/string.o
IDT_OBJ = $(BUILD_DIR)/idt.o
INTERRUPTS_OBJ = $(BUILD_DIR)/interrupts.o
INTERRUPTS_ASM_OBJ = $(BUILD_DIR)/interrupts_asm.o
TIMER_OBJ = $(BUILD_DIR)/timer.o
HEAP_OBJ = $(BUILD_DIR)/heap.o
PHYSMEM_OBJ = $(BUILD_DIR)/physmem.o
PAGING_OBJ = $(BUILD_DIR)/paging.o
FS_OBJ = $(BUILD_DIR)/fs.o
SYSCALL_OBJ = $(BUILD_DIR)/syscall.o
FBCONSOLE_OBJ = $(BUILD_DIR)/fbconsole.o

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-omit-frame-pointer -fno-optimize-sibling-calls \
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

OBJS = $(BOOT_OBJ) $(KERNEL_OBJ) $(TERMINAL_OBJ) $(CONSOLE_BACKEND_OBJ) $(KEYBOARD_OBJ) $(SHELL_INPUT_OBJ) $(SHELL_OBJ) $(PARSER_OBJ) $(TASK_OBJ) $(STRING_OBJ) $(IDT_OBJ) $(INTERRUPTS_OBJ) $(INTERRUPTS_ASM_OBJ) $(TIMER_OBJ) $(HEAP_OBJ) $(PHYSMEM_OBJ) $(PAGING_OBJ) $(FS_OBJ) $(SYSCALL_OBJ) $(FBCONSOLE_OBJ)

$(FONT_HEADER): $(FONT_PSF) $(FONT_TOOL)
	$(PYTHON) $(FONT_TOOL) $(FONT_PSF) $(FONT_HEADER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

compile: $(FONT_HEADER) $(BUILD_DIR)
	$(CC) $(CFLAGS) -c kernel/kernel.c -o $(KERNEL_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/terminal.c -o $(TERMINAL_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/console_backend.c -o $(CONSOLE_BACKEND_OBJ)
	$(CC) $(CFLAGS) -c drivers/input/keyboard.c -o $(KEYBOARD_OBJ)
	$(CC) $(CFLAGS) -c userland/shell/shell_input.c -o $(SHELL_INPUT_OBJ)
	$(CC) $(CFLAGS) -c userland/shell/shell.c -o $(SHELL_OBJ)
	$(CC) $(CFLAGS) -c userland/shell/parser.c -o $(PARSER_OBJ)
	$(CC) $(CFLAGS) -c kernel/task/task.c -o $(TASK_OBJ)
	$(CC) $(CFLAGS) -c lib/string.c -o $(STRING_OBJ)
	$(CC) $(CFLAGS) -c kernel/idt.c -o $(IDT_OBJ)
	$(CC) $(CFLAGS) -c kernel/interrupts.c -o $(INTERRUPTS_OBJ)
	$(CC) $(CFLAGS) -c kernel/task/timer.c -o $(TIMER_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/heap.c -o $(HEAP_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/physmem.c -o $(PHYSMEM_OBJ)
	$(CC) $(CFLAGS) -c kernel/mem/paging.c -o $(PAGING_OBJ)
	$(CC) $(CFLAGS) -c fs/fs.c -o $(FS_OBJ)
	$(CC) $(CFLAGS) -c kernel/syscall.c -o $(SYSCALL_OBJ)
	$(CC) $(CFLAGS) -c drivers/console/fbconsole.c -o $(FBCONSOLE_OBJ)
	$(AS) --32 arch/x86/interrupts.s -o $(INTERRUPTS_ASM_OBJ)
	$(AS) --32 arch/x86/boot/boot.s -o $(BOOT_OBJ)
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJS)

create-iso: compile $(DIST_DIR)
	rm -rf $(ISO_DIR) $(ISO_IMAGE)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR)

execute:
	qemu-system-i386 -boot d -cdrom $(ISO_IMAGE) -m 128 -no-reboot -no-shutdown

debug:
	qemu-system-i386 -boot d -cdrom $(ISO_IMAGE) -m 128 -no-reboot -no-shutdown -d int

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
	rm -f $(FONT_HEADER)

run: clean compile create-iso execute