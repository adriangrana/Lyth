CC = gcc
AS = as
LD = ld

CFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector
LDFLAGS = -m elf_i386 -T linker.ld

OBJS = boot.o kernel.o terminal.o keyboard.o shell.o string.o

compile:
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
	$(CC) $(CFLAGS) -c terminal.c -o terminal.o
	$(CC) $(CFLAGS) -c keyboard.c -o keyboard.o
	$(CC) $(CFLAGS) -c shell.c -o shell.o
	$(CC) $(CFLAGS) -c string.c -o string.o
	$(AS) --32 boot.s -o boot.o
	$(LD) $(LDFLAGS) -o kernel.bin $(OBJS)

create-iso:
	rm -rf iso myos.iso
	mkdir -p iso/boot/grub
	cp kernel.bin iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

execute:
	qemu-system-i386 -boot d -cdrom myos.iso -m 128 -no-reboot -no-shutdown

clean:
	rm -rf iso
	rm -f *.o kernel.bin myos.iso

run: clean compile create-iso execute