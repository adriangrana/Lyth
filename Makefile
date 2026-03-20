CC = gcc
AS = as
LD = ld
PYTHON = python3

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-omit-frame-pointer -fno-optimize-sibling-calls -Iinclude
LDFLAGS = -m elf_i386 -T linker.ld

FONT_PSF = assets/font.psf
FONT_TOOL = tools/psf2h.py
FONT_HEADER = include/font_psf.h

OBJS = boot.o kernel.o terminal.o keyboard.o shell_input.o shell.o parser.o task.o string.o idt.o interrupts.o interrupts_asm.o timer.o heap.o fs.o syscall.o fbconsole.o

$(FONT_HEADER): $(FONT_PSF) $(FONT_TOOL)
	$(PYTHON) $(FONT_TOOL) $(FONT_PSF) $(FONT_HEADER)

compile: $(FONT_HEADER)
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
	$(CC) $(CFLAGS) -c terminal.c -o terminal.o
	$(CC) $(CFLAGS) -c keyboard.c -o keyboard.o
	$(CC) $(CFLAGS) -c shell_input.c -o shell_input.o
	$(CC) $(CFLAGS) -c shell.c -o shell.o
	$(CC) $(CFLAGS) -c parser.c -o parser.o
	$(CC) $(CFLAGS) -c task.c -o task.o
	$(CC) $(CFLAGS) -c string.c -o string.o
	$(CC) $(CFLAGS) -c idt.c -o idt.o
	$(CC) $(CFLAGS) -c interrupts.c -o interrupts.o
	$(CC) $(CFLAGS) -c timer.c -o timer.o
	$(CC) $(CFLAGS) -c heap.c -o heap.o
	$(CC) $(CFLAGS) -c fs.c -o fs.o
	$(CC) $(CFLAGS) -c syscall.c -o syscall.o
	$(CC) $(CFLAGS) -c fbconsole.c -o fbconsole.o
	$(AS) --32 interrupts.s -o interrupts_asm.o
	$(AS) --32 boot.s -o boot.o
	$(LD) $(LDFLAGS) -o kernel.bin $(OBJS)

create-iso:
	rm -rf iso lyth.iso
	mkdir -p iso/boot/grub
	cp kernel.bin iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o lyth.iso iso

execute:
	qemu-system-i386 -boot d -cdrom lyth.iso -m 128 -no-reboot -no-shutdown

debug:
	qemu-system-i386 -boot d -cdrom lyth.iso -m 128 -no-reboot -no-shutdown -d int

clean:
	rm -rf iso
	rm -f *.o kernel.bin lyth.iso
	rm -f $(FONT_HEADER)

run: clean compile create-iso execute