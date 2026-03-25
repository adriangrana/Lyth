#include <stdint.h>
#include "multiboot.h"
#include "fbconsole.h"
#include "gdt.h"
#include "terminal.h"
#include "shell_input.h"
#include "interrupts.h"
#include "input.h"
#include "mouse.h"
#include "keyboard.h"
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
#include "hpet.h"
#include "pci.h"
#include "e1000.h"
#include "socket.h"
#include "ahci.h"
#include "video.h"
#include "xhci.h"
#include "usb_hid.h"
#include "paging.h"
#include "splash.h"
#include "session.h"

/* from lib/string.c — avoid including string.h (size_t conflict) */
extern int str_starts_with(const char* str, const char* prefix);
extern const char* str_after_prefix(const char* str, const char* prefix);

static const char* boot_mode_str = "desconocido";

static void parse_boot_mode(multiboot_info_t* mbi) {
    if (!(mbi->flags & (1U << 2)) || !mbi->cmdline)
        return;
    const char* cmdline = (const char*)(uintptr_t)mbi->cmdline;
    /* buscar boot_mode=uefi o boot_mode=bios en la cmdline */
    for (const char* p = cmdline; *p; p++) {
        if (str_starts_with(p, "boot_mode=")) {
            const char* val = str_after_prefix(p, "boot_mode=");
            if (val) {
                if (str_starts_with(val, "uefi"))
                    boot_mode_str = "UEFI";
                else if (str_starts_with(val, "bios"))
                    boot_mode_str = "BIOS";
            }
        }
        if (str_starts_with(p, "lyth_mode=")) {
            const char* val = str_after_prefix(p, "lyth_mode=");
            if (val) {
                if (str_starts_with(val, "recovery"))
                    init_set_boot_mode(BOOT_MODE_RECOVERY);
                else if (str_starts_with(val, "debug"))
                    init_set_boot_mode(BOOT_MODE_DEBUG);
            }
        }
    }
}

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
    terminal_print("Boot: ");
    terminal_print(boot_mode_str);
    terminal_print("\n");

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
    int splash_boot;  /* 1 = splash active, suppress terminal_print */

    gdt_init();
    interrupts_init_early();  /* IDT + exception handlers — catch faults early */
    klog_clear();
    klog_write(KLOG_LEVEL_INFO, "boot", "GDT inicializada");
    terminal_init();
    serial_init();
    serial_print("[boot] " LYTH_KERNEL_PRETTY_NAME " serial activo\n");
    parse_boot_mode(mbi);
    serial_print("[boot] Boot: ");
    serial_print(boot_mode_str);
    serial_print("\n");

    /* ── Early framebuffer mapping ──────────────────────────────── *
     * On real hardware (especially UEFI) the framebuffer physical
     * address may be above the 4 GB identity map set up in boot64.s.
     * Map it now BEFORE any code tries to write pixels.             */
    serial_print("[boot] MBI flags: ");
    serial_print_hex(mbi->flags);
    serial_print("\n");
    if (mbi->flags & (1u << 12)) {
        uint64_t fb_addr = mbi->framebuffer_addr;
        uint64_t fb_size = (uint64_t)mbi->framebuffer_pitch
                           * mbi->framebuffer_height;
        serial_print("[boot] Framebuffer addr: ");
        serial_print_hex64(fb_addr);
        serial_print(" size: ");
        serial_print_hex64(fb_size);
        serial_print(" ");
        serial_print_uint(mbi->framebuffer_width);
        serial_print("x");
        serial_print_uint(mbi->framebuffer_height);
        serial_print("x");
        serial_print_uint((unsigned int)mbi->framebuffer_bpp);
        serial_print(" type=");
        serial_print_uint((unsigned int)mbi->framebuffer_type);
        serial_print("\n");
        if (fb_addr != 0 && fb_size != 0) {
            paging_map_region_early(fb_addr, fb_size);
            serial_print("[boot] Framebuffer mapped in page tables\n");
        }
    } else {
        serial_print("[boot] WARN: No framebuffer info from bootloader\n");
    }

    serial_print("[boot] Calling video_init...\n");
    if (video_init(mbi)) {
        terminal_clear();
        klog_write(KLOG_LEVEL_INFO, "video", "Driver de video activado");
        serial_print("[boot] Video OK\n");
    } else {
        klog_write(KLOG_LEVEL_WARN, "video", "Driver de video no disponible");
        serial_print("[boot] WARN: video_init failed\n");
    }

    serial_print("[boot] physmem_init...\n");
    physmem_init(mbi);
    serial_print("[boot] paging_init...\n");
    paging_init(mbi);
    heap_init();
    ugdb_init();
    fs_init();
    vfs_init();
    vfs_mount("/", ramfs_create_root());
    vfs_mount("/dev", devfs_create_root());
    /* Create essential directory structure */
    vfs_create("/etc", VFS_FLAG_DIR);
    vfs_create("/home", VFS_FLAG_DIR);
    vfs_create("/tmp", VFS_FLAG_DIR);
    task_system_init();
    klog_write(KLOG_LEVEL_INFO, "mem", "Heap, physmem y paging inicializados");
    klog_write(KLOG_LEVEL_INFO, "fs", "FS en memoria listo");
    klog_write(KLOG_LEVEL_INFO, "vfs", "VFS inicializado, ramfs montado en /");
    klog_write(KLOG_LEVEL_INFO, "devfs", "devfs montado en /dev");
    klog_write(KLOG_LEVEL_INFO, "task", "Scheduler listo");

    /* ── Splash screen ─────────────────────────────────────────────
     * In normal boot mode, show a graphical splash instead of raw
     * terminal output.  Recovery/debug modes keep the text console. */
    if (init_get_boot_mode() == BOOT_MODE_NORMAL && fb_active()) {
        splash_show();
        splash_set_message("Initializing system...");
        splash_set_progress(10);
    } else {
        print_framebuffer_info();
        print_memory_info();
    }
    splash_boot = (init_get_boot_mode() == BOOT_MODE_NORMAL && fb_active());

    keyboard_init();
    mouse_init();
    if (fb_active() && mouse_is_enabled()) {
        mouse_get_state(&mouse_state);
        fb_move_mouse_cursor(mouse_state.x, mouse_state.y);
        klog_write(KLOG_LEVEL_INFO, "mouse", "Ratón PS/2 activado");
    } else {
        klog_write(KLOG_LEVEL_WARN, "mouse", "Ratón PS/2 no disponible");
    }
    splash_set_message("Detecting hardware...");
    splash_set_progress(20);
    init_set_boot_state(BOOT_STATE_SERVICES);
    if (!splash_boot) terminal_print("Init: ACPI...");
    serial_print("[boot] acpi_init...\n");
    acpi_init();
    if (!splash_boot) terminal_print(" HPET...");
    serial_print("[boot] hpet_init...\n");
    hpet_init();
    if (!splash_boot) terminal_print(" APIC...");
    serial_print("[boot] apic_init...\n");
    apic_init();
    if (!splash_boot) terminal_print(" SMP...");
    serial_print("[boot] smp_init...\n");
    smp_init();
    serial_print("[boot] smp_init done\n");
    if (!splash_boot) terminal_print(" IRQ...");
    interrupts_init();
    if (!splash_boot) terminal_print(" RTC...");
    rtc_init();
    if (!splash_boot) terminal_print_line(" OK");
    splash_set_progress(35);
    klog_write(KLOG_LEVEL_INFO, "rtc",  "RTC CMOS inicializado");
    klog_write(KLOG_LEVEL_INFO, "irq",  "IDT/PIC/PIT inicializados");

    splash_set_message("Scanning PCI bus...");
    splash_set_progress(40);
    if (!splash_boot) terminal_print("Init: PCI...");
    serial_print("[boot] pci_init...\n");
    pci_init();
    if (!splash_boot) {
        /* Show PCI count on screen for debug */
        terminal_print("(");
        {
            char tmp[8];
            int cnt = pci_device_count();
            int pos = 0;
            if (cnt == 0) { tmp[pos++] = '0'; }
            else {
                int d = 100;
                int started = 0;
                while (d > 0) {
                    int digit = cnt / d;
                    if (digit || started) { tmp[pos++] = '0' + digit; started = 1; }
                    cnt %= d;
                    d /= 10;
                }
            }
            tmp[pos] = 0;
            terminal_print(tmp);
        }
        terminal_print(")");
        terminal_print(" NET...");
    }
    splash_set_message("Initializing network...");
    splash_set_progress(50);
    serial_print("[boot] net/e1000_init...\n");
    net_init();
    e1000_init();
    if (!splash_boot) terminal_print(" USB...");
    splash_set_message("Initializing USB...");
    splash_set_progress(60);
    serial_print("[boot] xhci_init...\n");
    xhci_init();
    usb_hid_init();
    if (!splash_boot) terminal_print_line(" OK");
    klog_write(KLOG_LEVEL_INFO, "net",  "Stack de red inicializado");

    splash_set_message("Detecting storage...");
    splash_set_progress(70);
    if (!splash_boot) terminal_print("Init: ATA...");
    serial_print("[boot] ata_init...\n");
    ata_init();
    if (ata_is_present(ATA_DRIVE_MASTER))
        klog_write(KLOG_LEVEL_INFO, "ata", "Disco 0 (master) detectado");
    else
        klog_write(KLOG_LEVEL_WARN, "ata", "Disco 0 (master) no detectado");
    if (ata_is_present(ATA_DRIVE_SLAVE))
        klog_write(KLOG_LEVEL_INFO, "ata", "Disco 1 (slave) detectado");

    if (!splash_boot) terminal_print(" BLKDEV...");
    serial_print("[boot] blkdev_init...\n");
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

    if (!splash_boot) terminal_print(" AHCI...");
    serial_print("[boot] ahci_init...\n");
    /* AHCI/SATA controller ---------------------------------------- */
    ahci_init();
    if (!splash_boot) terminal_print_line(" OK");

    splash_set_message("Mounting filesystems...");
    splash_set_progress(80);

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
    if (splash_boot) ktest_set_silent(1);
    boot_tests_run();
    if (splash_boot) ktest_set_silent(0);

    splash_set_message("Starting services...");
    splash_set_progress(95);

    /* Set final boot state so init task knows it can proceed to GUI. */
    if (init_get_boot_mode() == BOOT_MODE_NORMAL && fb_active()) {
        init_set_boot_state(BOOT_STATE_GRAPHICS);
    } else {
        init_set_boot_state(BOOT_STATE_RECOVERY);
    }

    /* Spawn the init task (PID 1) — manages login and session. */
    init_start();
    
    while (1) {
        __asm__ __volatile__("hlt");
    }
}