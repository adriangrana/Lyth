#include "hpet.h"
#include "acpi.h"
#include "paging.h"
#include "klog.h"

/* ── ACPI HPET table (signature "HPET") ────────────────────────── */

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    /* HPET-specific fields */
    uint32_t event_timer_block_id;
    uint8_t  address_space_id;     /* 0 = memory-mapped */
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  reserved;
    uint32_t base_address_lo;
    uint32_t base_address_hi;
    uint8_t  hpet_number;
    uint16_t min_tick;
    uint8_t  page_protection;
} __attribute__((packed)) acpi_hpet_table_t;

/* ── State ──────────────────────────────────────────────────────── */

static volatile uint32_t* hpet_base = 0;
static uint32_t hpet_period = 0;   /* femtoseconds per tick */
static uint32_t hpet_freq   = 0;   /* Hz */
static int      hpet_ready  = 0;

/* 64÷32 → 32 division using x86 div instruction */
static uint32_t div64_32(uint64_t dividend, uint32_t divisor) {
    uint32_t hi = (uint32_t)(dividend >> 32);
    uint32_t lo = (uint32_t)dividend;
    uint32_t q_lo, r;

    if (hi == 0)
        return lo / divisor;

    r = hi % divisor;
    __asm__ volatile ("divl %2"
        : "=a"(q_lo), "=d"(r)
        : "r"(divisor), "a"(lo), "d"(r));
    return q_lo;
}

/* ── MMIO helpers ───────────────────────────────────────────────── */

static uint32_t hpet_read32(uint32_t offset) {
    return *(volatile uint32_t*)((uint8_t*)hpet_base + offset);
}

static void hpet_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)((uint8_t*)hpet_base + offset) = value;
}

/* ── Public API ─────────────────────────────────────────────────── */

int hpet_init(void) {
    const acpi_hpet_table_t* tbl;
    uint32_t phys;
    uint32_t cap;

    tbl = (const acpi_hpet_table_t*)acpi_find_table("HPET");
    if (!tbl) {
        klog_write(KLOG_LEVEL_WARN, "hpet", "tabla ACPI HPET no encontrada");
        return 0;
    }

    if (tbl->address_space_id != 0) {
        klog_write(KLOG_LEVEL_WARN, "hpet", "HPET no es memory-mapped");
        return 0;
    }

    phys = tbl->base_address_lo;
    if (phys == 0) {
        klog_write(KLOG_LEVEL_WARN, "hpet", "HPET base address es 0");
        return 0;
    }

    /* Ensure the 4MB page containing the HPET registers is mapped */
    if (!paging_map_mmio(phys)) {
        klog_write(KLOG_LEVEL_ERROR, "hpet", "no se pudo mapear HPET MMIO");
        return 0;
    }

    hpet_base = (volatile uint32_t*)(uintptr_t)phys;

    /* Read capabilities: bits [63:32] = period in femtoseconds */
    cap = hpet_read32(HPET_REG_CAP + 4);   /* upper 32 bits */
    hpet_period = cap;

    if (hpet_period == 0) {
        klog_write(KLOG_LEVEL_WARN, "hpet", "periodo HPET es 0");
        hpet_base = 0;
        return 0;
    }

    /* frequency = 10^15 / period_fs  —  use div64_32 helper */
    hpet_freq = div64_32(1000000000000000ULL, hpet_period);

    /* Stop counter, reset to 0, then start */
    hpet_write32(HPET_REG_CONFIG,
                 hpet_read32(HPET_REG_CONFIG) & ~HPET_CFG_ENABLE);
    hpet_write32(HPET_REG_COUNTER, 0);
    hpet_write32(HPET_REG_COUNTER + 4, 0);  /* upper 32 bits */
    hpet_write32(HPET_REG_CONFIG,
                 hpet_read32(HPET_REG_CONFIG) | HPET_CFG_ENABLE);

    hpet_ready = 1;

    klog_write(KLOG_LEVEL_INFO, "hpet", "HPET activo");
    return 1;
}

int hpet_is_available(void) {
    return hpet_ready;
}

uint32_t hpet_read_counter(void) {
    return hpet_read32(HPET_REG_COUNTER);
}

uint32_t hpet_period_fs(void) {
    return hpet_period;
}

uint32_t hpet_get_frequency(void) {
    return hpet_freq;
}
