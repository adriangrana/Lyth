#include "gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

static struct gdt_entry gdt[6];
static struct gdt_ptr gdtp;
static tss_entry_t tss_entry;
static uint8_t tss_stack[4096];

extern void gdt_flush(uint32_t gdt_ptr_address);
extern void tss_flush(void);

static void gdt_set_gate(int index,
                         uint32_t base,
                         uint32_t limit,
                         uint8_t access,
                         uint8_t granularity) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[index].granularity |= (uint8_t)(granularity & 0xF0);
    gdt[index].access = access;
}

static void write_tss(int index, uint16_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t)(uintptr_t)&tss_entry;
    uint32_t limit = base + sizeof(tss_entry_t);

    gdt_set_gate(index, base, limit, 0x89, 0x40);

    for (uint32_t i = 0; i < sizeof(tss_entry_t); i++) {
        ((uint8_t*)&tss_entry)[i] = 0;
    }

    tss_entry.ss0 = ss0;
    tss_entry.esp0 = esp0;
    tss_entry.cs = GDT_USER_CODE_SELECTOR | 0x03;
    tss_entry.ss = GDT_USER_DATA_SELECTOR | 0x03;
    tss_entry.ds = GDT_USER_DATA_SELECTOR | 0x03;
    tss_entry.es = GDT_USER_DATA_SELECTOR | 0x03;
    tss_entry.fs = GDT_USER_DATA_SELECTOR | 0x03;
    tss_entry.gs = GDT_USER_DATA_SELECTOR | 0x03;
    tss_entry.iomap_base = sizeof(tss_entry_t);
}

void gdt_init(void) {
    gdtp.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtp.base = (uint32_t)(uintptr_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFFU, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFFU, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFFU, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFFU, 0xF2, 0xCF);
    write_tss(5,
              GDT_KERNEL_DATA_SELECTOR,
              (uint32_t)(uintptr_t)(tss_stack + sizeof(tss_stack)));

    gdt_flush((uint32_t)(uintptr_t)&gdtp);
    tss_flush();
}

uint16_t gdt_kernel_code_selector(void) {
    return GDT_KERNEL_CODE_SELECTOR;
}

uint16_t gdt_kernel_data_selector(void) {
    return GDT_KERNEL_DATA_SELECTOR;
}

uint16_t gdt_user_code_selector(void) {
    return GDT_USER_CODE_SELECTOR;
}

uint16_t gdt_user_data_selector(void) {
    return GDT_USER_DATA_SELECTOR;
}

/* ── Per-AP GDT/TSS ─────────────────────────────────────────────── */

#define SMP_MAX_CPUS 16

/* Each AP gets its own GDT copy + TSS (statically allocated). */
static struct gdt_entry  ap_gdts[SMP_MAX_CPUS][6];
static struct gdt_ptr    ap_gdt_ptrs[SMP_MAX_CPUS];
static tss_entry_t       ap_tss[SMP_MAX_CPUS];
static int               ap_gdt_next;    /* index of next free slot */

static void gdt_set_gate_arr(struct gdt_entry* table,
                             int index,
                             uint32_t base,
                             uint32_t limit,
                             uint8_t access,
                             uint8_t granularity) {
    table[index].base_low    = (uint16_t)(base & 0xFFFF);
    table[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    table[index].base_high   = (uint8_t)((base >> 24) & 0xFF);
    table[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    table[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    table[index].granularity |= (uint8_t)(granularity & 0xF0);
    table[index].access      = access;
}

void gdt_init_ap(uint32_t kernel_stack_top) {
    int slot = __sync_fetch_and_add(&ap_gdt_next, 1);
    if (slot >= SMP_MAX_CPUS) return;

    struct gdt_entry* g  = ap_gdts[slot];
    tss_entry_t*      t  = &ap_tss[slot];

    /* Copy BSP GDT entries (NULL, kcode, kdata, ucode, udata) */
    gdt_set_gate_arr(g, 0, 0, 0, 0, 0);
    gdt_set_gate_arr(g, 1, 0, 0xFFFFFFFFU, 0x9A, 0xCF);
    gdt_set_gate_arr(g, 2, 0, 0xFFFFFFFFU, 0x92, 0xCF);
    gdt_set_gate_arr(g, 3, 0, 0xFFFFFFFFU, 0xFA, 0xCF);
    gdt_set_gate_arr(g, 4, 0, 0xFFFFFFFFU, 0xF2, 0xCF);

    /* Unique TSS for this AP */
    uint32_t tss_base  = (uint32_t)(uintptr_t)t;
    uint32_t tss_limit = tss_base + sizeof(tss_entry_t);
    gdt_set_gate_arr(g, 5, tss_base, tss_limit, 0x89, 0x40);

    for (uint32_t i = 0; i < sizeof(tss_entry_t); i++)
        ((uint8_t*)t)[i] = 0;

    t->ss0  = GDT_KERNEL_DATA_SELECTOR;
    t->esp0 = kernel_stack_top;
    t->cs   = GDT_USER_CODE_SELECTOR | 0x03;
    t->ss   = GDT_USER_DATA_SELECTOR | 0x03;
    t->ds   = GDT_USER_DATA_SELECTOR | 0x03;
    t->es   = GDT_USER_DATA_SELECTOR | 0x03;
    t->fs   = GDT_USER_DATA_SELECTOR | 0x03;
    t->gs   = GDT_USER_DATA_SELECTOR | 0x03;
    t->iomap_base = sizeof(tss_entry_t);

    /* Load GDT */
    ap_gdt_ptrs[slot].limit = (uint16_t)(sizeof(ap_gdts[slot]) - 1);
    ap_gdt_ptrs[slot].base  = (uint32_t)(uintptr_t)g;
    gdt_flush((uint32_t)(uintptr_t)&ap_gdt_ptrs[slot]);
    tss_flush();
}