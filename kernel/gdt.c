#include "gdt.h"
#include <stdint.h>
#include <stddef.h>

/* ── 64-bit GDT ──────────────────────────────────────────────────── *
 * Layout (each normal entry = 8 bytes, TSS = 16 bytes):
 *   0x00  NULL
 *   0x08  Kernel Code  (64-bit, DPL 0, L=1 D=0)
 *   0x10  Kernel Data  (64-bit, DPL 0)
 *   0x18  User Code    (64-bit, DPL 3, L=1 D=0)
 *   0x20  User Data    (64-bit, DPL 3)
 *   0x28  TSS low      (16-byte system descriptor)
 *   0x30  TSS high
 * ──────────────────────────────────────────────────────────────────── */

#define GDT_ENTRY_COUNT 7   /* 5 normal + 2 for TSS */

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 64-bit TSS (Task State Segment) — 104 bytes */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

static uint64_t gdt[GDT_ENTRY_COUNT];
static struct gdt_ptr gdtp;
static tss64_t tss;
static uint8_t tss_stack[8192] __attribute__((aligned(16)));

extern void gdt_flush(uintptr_t gdt_ptr_address);
extern void tss_flush(void);

static void write_tss_descriptor(uint64_t* table, int index, uint64_t base, uint32_t limit) {
    uint64_t lo = 0;
    lo |= (uint64_t)(limit & 0xFFFFU);
    lo |= (uint64_t)(base & 0xFFFFU) << 16;
    lo |= (uint64_t)((base >> 16) & 0xFFU) << 32;
    lo |= (uint64_t)0x89ULL << 40;                   /* type = 64-bit TSS available */
    lo |= (uint64_t)((limit >> 16) & 0xFU) << 48;
    lo |= (uint64_t)((base >> 24) & 0xFFU) << 56;
    table[index] = lo;

    uint64_t hi = (base >> 32) & 0xFFFFFFFFULL;
    table[index + 1] = hi;
}

static void tss_init(uint64_t rsp0) {
    for (size_t i = 0; i < sizeof(tss); i++)
        ((uint8_t*)&tss)[i] = 0;

    tss.rsp0 = rsp0;
    tss.iomap_base = sizeof(tss64_t);
}

void gdt_init(void) {
    /* NULL */
    gdt[0] = 0;
    /* Kernel code: base=0, limit=0xFFFFF, L=1 D=0 P=1 DPL=0 type=0xA (exec/read) */
    gdt[1] = 0x00AF9A000000FFFFULL;
    /* Kernel data: base=0, limit=0xFFFFF, G=1 DB=1 P=1 DPL=0 type=0x2 (read/write) */
    gdt[2] = 0x00CF92000000FFFFULL;
    /* User code: L=1 D=0 P=1 DPL=3 type=0xA */
    gdt[3] = 0x00AFFA000000FFFFULL;
    /* User data: G=1 DB=1 P=1 DPL=3 type=0x2 */
    gdt[4] = 0x00CFF2000000FFFFULL;

    /* TSS */
    tss_init((uint64_t)(uintptr_t)(tss_stack + sizeof(tss_stack)));
    write_tss_descriptor(gdt, 5, (uint64_t)(uintptr_t)&tss, sizeof(tss64_t) - 1);

    gdtp.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtp.base = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uintptr_t)&gdtp);
    tss_flush();
}

void gdt_set_tss_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

uint16_t gdt_kernel_code_selector(void) { return GDT_KERNEL_CODE_SELECTOR; }
uint16_t gdt_kernel_data_selector(void) { return GDT_KERNEL_DATA_SELECTOR; }
uint16_t gdt_user_code_selector(void)   { return GDT_USER_CODE_SELECTOR; }
uint16_t gdt_user_data_selector(void)   { return GDT_USER_DATA_SELECTOR; }

/* ── Per-AP GDT/TSS ─────────────────────────────────────────────── */

#define SMP_MAX_CPUS 16

static uint64_t       ap_gdts[SMP_MAX_CPUS][GDT_ENTRY_COUNT];
static struct gdt_ptr ap_gdt_ptrs[SMP_MAX_CPUS];
static tss64_t        ap_tss[SMP_MAX_CPUS];
static int            ap_gdt_next;

void gdt_init_ap(uintptr_t kernel_stack_top) {
    int slot = __sync_fetch_and_add(&ap_gdt_next, 1);
    if (slot >= SMP_MAX_CPUS) return;

    uint64_t* g = ap_gdts[slot];
    tss64_t*  t = &ap_tss[slot];

    g[0] = 0;
    g[1] = 0x00AF9A000000FFFFULL;
    g[2] = 0x00CF92000000FFFFULL;
    g[3] = 0x00AFFA000000FFFFULL;
    g[4] = 0x00CFF2000000FFFFULL;

    for (size_t i = 0; i < sizeof(tss64_t); i++)
        ((uint8_t*)t)[i] = 0;

    t->rsp0 = (uint64_t)kernel_stack_top;
    t->iomap_base = sizeof(tss64_t);

    write_tss_descriptor(g, 5, (uint64_t)(uintptr_t)t, sizeof(tss64_t) - 1);

    ap_gdt_ptrs[slot].limit = (uint16_t)(sizeof(ap_gdts[slot]) - 1);
    ap_gdt_ptrs[slot].base  = (uint64_t)(uintptr_t)g;

    gdt_flush((uintptr_t)&ap_gdt_ptrs[slot]);
    tss_flush();
}