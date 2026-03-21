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
#include "task.h"
#include "physmem.h"
#include "paging.h"
#include "klog.h"

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
    terminal_print("Lyth OS\n");

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
    if (fb_init(mbi)) {
        terminal_clear();
        klog_write(KLOG_LEVEL_INFO, "video", "Framebuffer activado");
    } else {
        klog_write(KLOG_LEVEL_WARN, "video", "Framebuffer no disponible");
    }

    physmem_init(mbi);
    paging_init(mbi);
    heap_init();
    fs_init();
    task_system_init();
    klog_write(KLOG_LEVEL_INFO, "mem", "Heap, physmem y paging inicializados");
    klog_write(KLOG_LEVEL_INFO, "fs", "FS en memoria listo");
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
    shell_input_init();
    klog_write(KLOG_LEVEL_INFO, "shell", "Shell interactiva lista");

    interrupts_init();
    klog_write(KLOG_LEVEL_INFO, "irq", "IDT/PIC/PIT inicializados");

    while (1) {
        input_event_t event;

        while (input_poll_event(&event)) {
            if (event.device_type == INPUT_DEVICE_MOUSE && fb_active()) {
                mouse_get_state(&mouse_state);
                fb_move_mouse_cursor(mouse_state.x, mouse_state.y);
            }

            shell_input_handle_event(&event);
        }

        terminal_update_cursor();
        __asm__ volatile ("hlt");
    }
}