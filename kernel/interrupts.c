#include "interrupts.h"
#include "idt.h"
#include "gdt.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "syscall.h"
#include "task.h"
#include "paging.h"
#include "terminal.h"
#include "panic.h"

static const char* exception_names[32] = {
    "Division by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "BOUND range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 floating-point",
    "Alignment check",
    "Machine check",
    "SIMD floating-point",
    "Virtualization",
    "Control protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor injection",
    "VMM communication",
    "Security",
    "Reserved"
};

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void irq14_stub(void);
extern void syscall_stub(void);
extern void isr0_stub(void);
extern void isr1_stub(void);
extern void isr2_stub(void);
extern void isr3_stub(void);
extern void isr4_stub(void);
extern void isr5_stub(void);
extern void isr6_stub(void);
extern void isr7_stub(void);
extern void isr8_stub(void);
extern void isr9_stub(void);
extern void isr10_stub(void);
extern void isr11_stub(void);
extern void isr12_stub(void);
extern void isr13_stub(void);
extern void isr14_stub(void);
extern void isr15_stub(void);
extern void isr16_stub(void);
extern void isr17_stub(void);
extern void isr18_stub(void);
extern void isr19_stub(void);
extern void isr20_stub(void);
extern void isr21_stub(void);
extern void isr22_stub(void);
extern void isr23_stub(void);
extern void isr24_stub(void);
extern void isr25_stub(void);
extern void isr26_stub(void);
extern void isr27_stub(void);
extern void isr28_stub(void);
extern void isr29_stub(void);
extern void isr30_stub(void);
extern void isr31_stub(void);

static const unsigned int exception_stubs[32] = {
    (unsigned int)isr0_stub,  (unsigned int)isr1_stub,
    (unsigned int)isr2_stub,  (unsigned int)isr3_stub,
    (unsigned int)isr4_stub,  (unsigned int)isr5_stub,
    (unsigned int)isr6_stub,  (unsigned int)isr7_stub,
    (unsigned int)isr8_stub,  (unsigned int)isr9_stub,
    (unsigned int)isr10_stub, (unsigned int)isr11_stub,
    (unsigned int)isr12_stub, (unsigned int)isr13_stub,
    (unsigned int)isr14_stub, (unsigned int)isr15_stub,
    (unsigned int)isr16_stub, (unsigned int)isr17_stub,
    (unsigned int)isr18_stub, (unsigned int)isr19_stub,
    (unsigned int)isr20_stub, (unsigned int)isr21_stub,
    (unsigned int)isr22_stub, (unsigned int)isr23_stub,
    (unsigned int)isr24_stub, (unsigned int)isr25_stub,
    (unsigned int)isr26_stub, (unsigned int)isr27_stub,
    (unsigned int)isr28_stub, (unsigned int)isr29_stub,
    (unsigned int)isr30_stub, (unsigned int)isr31_stub,
};

static void cpu_halt_forever(void) {
    __asm__ volatile ("cli");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static unsigned int read_cr2(void) {
    unsigned int value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    outb(0x21, mouse_is_enabled() ? 0xF8 : 0xFC);
    outb(0xA1, mouse_is_enabled() ? 0xEF : 0xFF);
}

static void pic_send_eoi(unsigned char irq) {
    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

unsigned int timer_interrupt_handler(unsigned int current_esp) {
    timer_handle_interrupt();
    pic_send_eoi(0);
    return task_schedule_on_timer(current_esp);
}

unsigned int keyboard_interrupt_handler(unsigned int current_esp) {
    keyboard_handle_interrupt();
    pic_send_eoi(1);
    return current_esp;
}

unsigned int mouse_interrupt_handler(unsigned int current_esp) {
    mouse_handle_interrupt();
    pic_send_eoi(12);
    return current_esp;
}

unsigned int syscall_interrupt_handler(unsigned int current_esp) {
    panic_frame_t* frame = (panic_frame_t*)current_esp;

    if (frame->eax == SYSCALL_FORK) {
        /* fork needs access to the raw frame to clone the register state,
           so it bypasses the normal syscall_callback path. */
        frame->eax = (unsigned int)task_fork_from_frame(current_esp);
    } else if (frame->eax == SYSCALL_EXEC) {
        frame->eax = syscall_exec_interrupt(current_esp,
                                            frame->ebx,
                                            frame->ecx);
    } else if (frame->eax == SYSCALL_EXECV) {
        frame->eax = syscall_execv_interrupt(current_esp,
                                             frame->ebx,
                                             frame->ecx,
                                             frame->edx,
                                             frame->esi);
    } else if (frame->eax == SYSCALL_EXECVE) {
        frame->eax = syscall_execve_interrupt(current_esp,
                                              frame->ebx,
                                              frame->ecx,
                                              frame->edx);
    } else {
        frame->eax = syscall_callback(frame->eax,
                                      frame->ebx,
                                      frame->ecx,
                                      frame->edx,
                                      frame->esi);
    }

    return task_schedule_on_syscall(current_esp);
}

unsigned int exception_interrupt_handler(unsigned int current_esp) {
    panic_frame_t* frame = (panic_frame_t*)current_esp;
    unsigned int vector = frame->vector & 0xFFU;
    int from_user_mode = (frame->cs & 0x03U) == 0x03U;

    if (from_user_mode && task_current_is_user_mode()) {
        if (vector == 14) {
            unsigned int fault_addr = read_cr2();
            unsigned int error_code = frame->error_code;

            /* COW: write to present user page → attempt resolve */
            if ((error_code & 0x07U) == 0x07U) {
                uint32_t* dir = task_current_page_directory();
                if (dir != 0 && paging_cow_resolve(dir, fault_addr)) {
                    return current_esp;
                }
            }

            terminal_set_color(0x0C);
            terminal_print_line("");
            terminal_print("[user fault] ");
            terminal_print(task_current_name() != 0 ? task_current_name() : "<unknown>");
            terminal_print(": ");
            terminal_print_line(exception_names[14]);
            terminal_print("CR2: ");
            terminal_print_hex(fault_addr);
            terminal_put_char('\n');
            if (fault_addr >= PAGING_USER_STACK_GUARD_BASE &&
                fault_addr < PAGING_USER_STACK_BOTTOM) {
                terminal_print_line("[user fault] stack guard page hit");
            }
            terminal_set_color(0x0F);
        } else {
            terminal_set_color(0x0C);
            terminal_print_line("");
            terminal_print("[user fault] ");
            terminal_print(task_current_name() != 0 ? task_current_name() : "<unknown>");
            terminal_print(": ");
            terminal_print_line(vector < 32 ? exception_names[vector] : "Unknown");
            terminal_set_color(0x0F);
        }

        task_exit(128 + (int)vector);
        return task_schedule_on_syscall(current_esp);
    }

    panic_show(vector < 32 ? exception_names[vector] : "Unknown exception",
               frame,
               (vector == 14U) ? read_cr2() : 0U);
    return current_esp;
}

void interrupts_init(void) {
    unsigned short code_selector = gdt_kernel_code_selector();

    idt_init();

    for (int vector = 0; vector < 32; vector++) {
        idt_set_gate((unsigned char)vector, exception_stubs[vector], code_selector, 0x8E);
    }

    pic_remap();
    timer_init(100);

    idt_set_gate(32, (unsigned int)irq0_stub,  code_selector, 0x8E);
    idt_set_gate(33, (unsigned int)irq1_stub,  code_selector, 0x8E);
    idt_set_gate(44, (unsigned int)irq12_stub, code_selector, 0x8E);
    idt_set_gate(46, (unsigned int)irq14_stub, code_selector, 0x8E);
    idt_set_gate(0x80, (unsigned int)syscall_stub, code_selector, 0xEE);

    idt_load_table();

    __asm__ volatile ("sti");
}