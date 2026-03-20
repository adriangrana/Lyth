#include <stdint.h>
#include "multiboot.h"
#include "fbconsole.h"
#include "terminal.h"
#include "shell_input.h"
#include "interrupts.h"
#include "keyboard.h"
#include "heap.h"
#include "fs.h"
#include "task.h"
#include "physmem.h"
#include "paging.h"

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

    terminal_init();
    if (fb_init(mbi)) {
        terminal_clear();
    }

    physmem_init(mbi);
    paging_init(mbi);
    heap_init();
    fs_init();
    task_system_init();

    print_framebuffer_info();
    print_memory_info();

    shell_input_init();

    interrupts_init();

    while (1) {
        keyboard_event_t event;

        while (keyboard_poll_event(&event)) {
            shell_input_handle_event(&event);
        }

        terminal_update_cursor();
        __asm__ volatile ("hlt");
    }
}