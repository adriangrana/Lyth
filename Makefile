CC = gcc
AS = as
LD = ld

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector
LDFLAGS = -m elf_i386 -T linker.ld

OBJS = boot.o kernel.o terminal.o keyboard.o shell.o string.o idt.o idt_load.o

compile:
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
	$(CC) $(CFLAGS) -c terminal.c -o terminal.o
	$(CC) $(CFLAGS) -c keyboard.c -o keyboard.o
	$(CC) $(CFLAGS) -c shell.c -o shell.o
	$(CC) $(CFLAGS) -c string.c -o string.o
	$(CC) $(CFLAGS) -c idt.c -o idt.o
	$(AS) --32 boot.s -o boot.o
	$(AS) --32 idt_load.s -o idt_load.o
	$(LD) $(LDFLAGS) -o kernel.bin $(OBJS)

create-iso:
	rm -rf iso lyth.iso
	mkdir -p iso/boot/grub
	cp kernel.bin iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o lyth.iso iso

execute:
	qemu-system-i386 -boot d -cdrom lyth.iso -m 128 -no-reboot -no-shutdown

clean:
	rm -rf iso
	rm -f *.o kernel.bin lyth.iso

run: clean compile create-iso execute