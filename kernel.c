#include "terminal.h"
#include "keyboard.h"
#include "shell.h"
#include "idt.h"

void kernel_main(void) {
    terminal_init();
    shell_init();
    idt_init();
    while (1) {
        char c = keyboard_get_char();
        if (c != 0) {
            shell_handle_char(c);
        }
    }
}