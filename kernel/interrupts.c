#include "interrupts.h"
#include <stdint.h>
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
#include "apic.h"
#include "e1000.h"
#include "xhci.h"
#include "hda.h"

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
extern void irq11_stub(void);
extern void irq12_stub(void);
extern void irq14_stub(void);
extern void xhci_irq_stub(void);
extern void hda_irq_stub(void);
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

static const uintptr_t exception_stubs[32] = {
    (uintptr_t)isr0_stub,  (uintptr_t)isr1_stub,
    (uintptr_t)isr2_stub,  (uintptr_t)isr3_stub,
    (uintptr_t)isr4_stub,  (uintptr_t)isr5_stub,
    (uintptr_t)isr6_stub,  (uintptr_t)isr7_stub,
    (uintptr_t)isr8_stub,  (uintptr_t)isr9_stub,
    (uintptr_t)isr10_stub, (uintptr_t)isr11_stub,
    (uintptr_t)isr12_stub, (uintptr_t)isr13_stub,
    (uintptr_t)isr14_stub, (uintptr_t)isr15_stub,
    (uintptr_t)isr16_stub, (uintptr_t)isr17_stub,
    (uintptr_t)isr18_stub, (uintptr_t)isr19_stub,
    (uintptr_t)isr20_stub, (uintptr_t)isr21_stub,
    (uintptr_t)isr22_stub, (uintptr_t)isr23_stub,
    (uintptr_t)isr24_stub, (uintptr_t)isr25_stub,
    (uintptr_t)isr26_stub, (uintptr_t)isr27_stub,
    (uintptr_t)isr28_stub, (uintptr_t)isr29_stub,
    (uintptr_t)isr30_stub, (uintptr_t)isr31_stub,
};

static void cpu_halt_forever(void) {
    __asm__ volatile ("cli");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static uint64_t read_cr2(void) {
    uint64_t value;
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

static void send_eoi(unsigned char irq) {
    if (apic_is_enabled()) {
        apic_eoi();
    } else {
        pic_send_eoi(irq);
    }
}

uintptr_t timer_interrupt_handler(uintptr_t current_esp) {
    timer_handle_interrupt();
    send_eoi(0);
    return task_schedule_on_timer(current_esp);
}

uintptr_t keyboard_interrupt_handler(uintptr_t current_esp) {
    keyboard_handle_interrupt();
    send_eoi(1);
    return current_esp;
}

uintptr_t mouse_interrupt_handler(uintptr_t current_esp) {
    mouse_handle_interrupt();
    send_eoi(12);
    return current_esp;
}

void e1000_interrupt_handler(void) {
    e1000_irq_handler();
    send_eoi(11);
}

void xhci_interrupt_handler_asm(void) {
    xhci_irq_handler();
    send_eoi(0);  /* APIC EOI — no legacy IRQ mapping for xHCI */
}

void hda_interrupt_handler_asm(void) {
    hda_irq_handler();
    send_eoi(0);  /* APIC EOI — PCI level-triggered */
}

uintptr_t syscall_interrupt_handler(uintptr_t current_esp) {
    panic_frame_t* frame = (panic_frame_t*)current_esp;

    /* 64-bit syscall convention: RAX=number, RDI=arg0, RSI=arg1, RDX=arg2, R10=arg3 */
    if (frame->rax == SYSCALL_FORK) {
        frame->rax = (uint64_t)task_fork_from_frame(current_esp);
    } else if (frame->rax == SYSCALL_EXEC) {
        frame->rax = syscall_exec_interrupt(current_esp,
                                            frame->rdi,
                                            frame->rsi);
    } else if (frame->rax == SYSCALL_EXECV) {
        frame->rax = syscall_execv_interrupt(current_esp,
                                             frame->rdi,
                                             frame->rsi,
                                             frame->rdx,
                                             frame->r10);
    } else if (frame->rax == SYSCALL_EXECVE) {
        frame->rax = syscall_execve_interrupt(current_esp,
                                              frame->rdi,
                                              frame->rsi,
                                              frame->rdx);
    } else {
        frame->rax = syscall_callback(frame->rax,
                                      frame->rdi,
                                      frame->rsi,
                                      frame->rdx,
                                      frame->r10);
    }

    return task_schedule_on_syscall(current_esp);
}

uintptr_t exception_interrupt_handler(uintptr_t current_esp) {
    panic_frame_t* frame = (panic_frame_t*)current_esp;
    unsigned int vector = (unsigned int)(frame->vector & 0xFFU);
    int from_user_mode = (frame->cs & 0x03U) == 0x03U;

    if (from_user_mode && task_current_is_user_mode()) {
        if (vector == 14) {
            uint64_t fault_addr = read_cr2();
            uint64_t error_code = frame->error_code;

            /* COW: write to present user page → attempt resolve */
            if ((error_code & 0x07U) == 0x07U) {
                uint64_t* dir = task_current_page_directory();
                if (dir != 0 && paging_cow_resolve(dir, (uintptr_t)fault_addr)) {
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
            terminal_print_hex((unsigned int)fault_addr);
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

void interrupts_init_early(void) {
    unsigned short code_selector = gdt_kernel_code_selector();

    idt_init();

    for (int vector = 0; vector < 32; vector++) {
        idt_set_gate((unsigned char)vector, exception_stubs[vector], code_selector, 0x8E);
    }

    /* Use IST1 for double fault (vector 8) so it gets a clean stack
       even if the kernel stack is corrupted.  This converts a silent
       triple-fault/reboot into a visible kernel panic. */
    idt_set_gate_ist(8, exception_stubs[8], code_selector, 0x8E, 1);

    idt_load_table();
    /* Interrupts stay disabled (CLI) — only exceptions are caught now. */
}

void interrupts_init(void) {
    unsigned short code_selector = gdt_kernel_code_selector();

    /* Do NOT call idt_init() again — early phase already set exception
     * handlers, and apic_init() has installed the spurious handler at 0xFF.
     * Re-zeroing would wipe those entries and cause triple faults. */

    timer_init(100);

    idt_set_gate(32, (uintptr_t)irq0_stub,  code_selector, 0x8E);
    idt_set_gate(33, (uintptr_t)irq1_stub,  code_selector, 0x8E);
    idt_set_gate(43, (uintptr_t)irq11_stub, code_selector, 0x8E);
    idt_set_gate(44, (uintptr_t)irq12_stub, code_selector, 0x8E);
    idt_set_gate(46, (uintptr_t)irq14_stub, code_selector, 0x8E);
    idt_set_gate(48, (uintptr_t)xhci_irq_stub, code_selector, 0x8E);
    idt_set_gate(49, (uintptr_t)hda_irq_stub, code_selector, 0x8E);
    idt_set_gate(0x80, (uintptr_t)syscall_stub, code_selector, 0xEE);

    if (apic_is_enabled()) {
        /* Route ISA IRQs through IOAPIC */
        ioapic_route_irq(0,  32, 0);   /* PIT timer */
        ioapic_route_irq(1,  33, 0);   /* keyboard */
        ioapic_route_irq_level(11, 43, 0);   /* E1000 NIC — PCI level-triggered */
        ioapic_route_irq(12, 44, mouse_is_enabled() ? 0 : 1);   /* mouse */
        ioapic_route_irq(14, 46, 0);   /* ATA primary */
    } else {
        pic_remap();
    }

    idt_load_table();

    __asm__ volatile ("sti");
}