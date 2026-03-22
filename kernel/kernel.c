#include <stdint.h>
#include "multiboot.h"
#include "fbconsole.h"
#include "gdt.h"
#include "terminal.h"
#include "shell_input.h"
#include "interrupts.h"
#include "input.h"
#include "mouse.h"
#include "heap.h"
#include "fs.h"
#include "vfs.h"
#include "ramfs.h"
#include "devfs.h"
#include "task.h"
#include "physmem.h"
#include "paging.h"
#include "klog.h"
#include "ata.h"
#include "blkdev.h"
#include "fat16.h"
#include "fat32.h"
#include "init.h"
#include "tty_vfs.h"
#include "serial.h"
#include "rtc.h"
#include "ktest.h"
#include "boot_tests.h"
#include "version.h"
#include "ugdb.h"
#include "acpi.h"
#include "apic.h"
#include "smp.h"

static void terminal_write_uint(uint32_t value) {
    char buffer[16];
    int index = 0;

    if (value == 0) {
        terminal_print("0");
        return;
    }

    while (value > 0) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        char ch[2];
        ch[0] = buffer[--index];
        ch[1] = '\0';
        terminal_print(ch);
    }
}

static void print_framebuffer_info(void) {
    terminal_print(LYTH_KERNEL_PRETTY_NAME "\n");

    if (!fb_active()) {
        terminal_print("Framebuffer: no disponible\n\n");
        return;
    }

    terminal_print("Framebuffer: ");
    terminal_write_uint(fb_width());
    terminal_print("x");
    terminal_write_uint(fb_height());
    terminal_print("x");
    terminal_write_uint((uint32_t)fb_bpp());
    terminal_print("\n");

    terminal_print("Pitch: ");
    terminal_write_uint(fb_pitch());
    terminal_print("\n");

    terminal_print("Console: ");
    terminal_write_uint((uint32_t)fb_columns());
    terminal_print("x");
    terminal_write_uint((uint32_t)fb_rows());
    terminal_print("\n");
    terminal_print("Font: 8x16 PSF\n\n");
}

static void print_memory_info(void) {
    terminal_print("Phys mem total: ");
    terminal_write_uint(physmem_total_bytes());
    terminal_print(" bytes\n");

    terminal_print("Phys mem libre: ");
    terminal_write_uint(physmem_free_bytes());
    terminal_print(" bytes\n");

    terminal_print("Paging: ");
    terminal_print_line(paging_is_enabled() ? "activo" : "inactivo");

    if (paging_is_enabled()) {
        terminal_print("Identity mapped: ");
        terminal_write_uint(paging_mapped_bytes());
        terminal_print(" bytes\n\n");
    } else {
        terminal_put_char('\n');
    }
}

void kernel_main(unsigned long mbi_ptr) {
    /* mbi_ptr: Multiboot info pointer passed in EBX by GRUB */
    multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mbi_ptr;
    mouse_state_t mouse_state;

    gdt_init();
    klog_clear();
    klog_write(KLOG_LEVEL_INFO, "boot", "GDT inicializada");
    terminal_init();
        serial_init();
        serial_print("[boot] " LYTH_KERNEL_PRETTY_NAME " serial activo\n");
    if (fb_init(mbi)) {
        terminal_clear();
        klog_write(KLOG_LEVEL_INFO, "video", "Framebuffer activado");
    } else {
        klog_write(KLOG_LEVEL_WARN, "video", "Framebuffer no disponible");
    }

    physmem_init(mbi);
    paging_init(mbi);
    heap_init();
    ugdb_init();
    fs_init();
    vfs_init();
    vfs_mount("/", ramfs_create_root());
    vfs_mount("/dev", devfs_create_root());
    task_system_init();
    klog_write(KLOG_LEVEL_INFO, "mem", "Heap, physmem y paging inicializados");
    klog_write(KLOG_LEVEL_INFO, "fs", "FS en memoria listo");
    klog_write(KLOG_LEVEL_INFO, "vfs", "VFS inicializado, ramfs montado en /");
    klog_write(KLOG_LEVEL_INFO, "devfs", "devfs montado en /dev");
    klog_write(KLOG_LEVEL_INFO, "task", "Scheduler listo");

    print_framebuffer_info();
    print_memory_info();

    mouse_init();
    if (fb_active() && mouse_is_enabled()) {
        mouse_get_state(&mouse_state);
        fb_move_mouse_cursor(mouse_state.x, mouse_state.y);
        klog_write(KLOG_LEVEL_INFO, "mouse", "Ratón PS/2 activado");
    } else {
        klog_write(KLOG_LEVEL_WARN, "mouse", "Ratón PS/2 no disponible");
    }
    acpi_init();
    apic_init();
    smp_init();
    interrupts_init();
    rtc_init();
    klog_write(KLOG_LEVEL_INFO, "rtc",  "RTC CMOS inicializado");
    klog_write(KLOG_LEVEL_INFO, "irq",  "IDT/PIC/PIT inicializados");

    ata_init();
    if (ata_is_present(ATA_DRIVE_MASTER))
        klog_write(KLOG_LEVEL_INFO, "ata", "Disco 0 (master) detectado");
    else
        klog_write(KLOG_LEVEL_WARN, "ata", "Disco 0 (master) no detectado");
    if (ata_is_present(ATA_DRIVE_SLAVE))
        klog_write(KLOG_LEVEL_INFO, "ata", "Disco 1 (slave) detectado");

    blkdev_init();
    {
        int idx;
        idx = blkdev_register_ata(ATA_DRIVE_MASTER);
        if (idx >= 0) {
            klog_write(KLOG_LEVEL_INFO, "blkdev", "hd0 registrado");
            if (blkdev_probe_partitions(idx) > 0)
                klog_write(KLOG_LEVEL_INFO, "blkdev", "Particiones hd0 detectadas");
        }
        idx = blkdev_register_ata(ATA_DRIVE_SLAVE);
        if (idx >= 0) {
            klog_write(KLOG_LEVEL_INFO, "blkdev", "hd1 registrado");
            if (blkdev_probe_partitions(idx) > 0)
                klog_write(KLOG_LEVEL_INFO, "blkdev", "Particiones hd1 detectadas");
        }
        klog_write(KLOG_LEVEL_INFO, "blkdev", "Capa de bloques lista");
    }

    /* Auto-mount FAT16 / FAT32 partitions ------------------------- */
    {
        int di;
        int fat_count = 0;

        for (di = 0; di < BLKDEV_MAX; di++) {
            blkdev_t     dev;
            vfs_node_t*  fat_root = 0;
            char         mnt[VFS_PATH_MAX];
            const char*  fs_label = "fat16";
            unsigned int k;

            if (blkdev_get(di, &dev) < 0) continue;

            fat_root = fat16_mount(di);
            if (!fat_root) {
                fat_root = fat32_mount(di);
                fs_label = "fat32";
            }
            if (!fat_root) continue;

            /* Mount at /<devname> (e.g. /hd0p1) */
            mnt[0] = '/';
            for (k = 0; dev.name[k] && k < VFS_PATH_MAX - 2U; k++)
                mnt[k + 1] = dev.name[k];
            mnt[k + 1] = '\0';

            if (vfs_mount(mnt, fat_root) == 0) {
                klog_write(KLOG_LEVEL_INFO, fs_label, "FAT montado");
                fat_count++;
            }
        }

        if (fat_count == 0)
            klog_write(KLOG_LEVEL_INFO, "fat", "Sin particiones FAT detectadas");
    }

    /* Set up the TTY VFS node and register it so all new tasks get
       fd 0/1/2 pre-filled with stdin/stdout/stderr. */
    tty_vfs_init();
     vfs_set_tty_node(devfs_tty_node());
     klog_write(KLOG_LEVEL_INFO, "tty", "TTY VFS node listo (/dev/tty, fd 0/1/2)");

    /* Mini harness de arranque (salida en pantalla + serial COM1). */
    boot_tests_run();

    /* Spawn the init task (PID 1) — owns the shell and event loop. */
    init_start();

    while (1) {
        __asm__ __volatile__("hlt");
    }
}