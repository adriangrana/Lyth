/* ============================================================
 *  hda.c  —  Intel High Definition Audio controller driver
 *
 *  Supports playback via the first output stream on the first
 *  codec found.  Volume and mute control through codec verbs.
 *
 *  Targets QEMU intel-hda + hda-output (ICH6-class controller).
 * ============================================================ */

#include "hda.h"
#include "pci.h"
#include "paging.h"
#include "physmem.h"
#include "klog.h"
#include "timer.h"
#include "string.h"
#include "serial.h"
#include "apic.h"

/* ── I/O helpers ────────────────────────────────────────────────── */

static inline void outb_hda(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* ── MMIO access ────────────────────────────────────────────────── */

static volatile uint8_t *hda_base;

static uint8_t hda_read8(uint32_t off) {
    return *(volatile uint8_t *)(hda_base + off);
}
static uint16_t hda_read16(uint32_t off) {
    return *(volatile uint16_t *)(hda_base + off);
}
static uint32_t hda_read32(uint32_t off) {
    return *(volatile uint32_t *)(hda_base + off);
}
static void hda_write8(uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(hda_base + off) = val;
}
static void hda_write16(uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(hda_base + off) = val;
}
static void hda_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(hda_base + off) = val;
}

/* ── CORB / RIRB ────────────────────────────────────────────────── */

#define CORB_ENTRIES    256
#define RIRB_ENTRIES    256

static volatile uint32_t *corb_buf;
static volatile uint64_t *rirb_buf;
static uint32_t           corb_phys;
static uint32_t           rirb_phys;
static uint16_t           rirb_rp;

/* ── BDL + PCM buffer ───────────────────────────────────────────── */

#define BDL_ENTRIES     2
#define PCM_BUF_SIZE    (48000 * 2 * 2)   /* 1 second stereo 16-bit at 48kHz */

static volatile hda_bdl_entry_t *bdl;
static uint32_t bdl_phys;
static volatile int16_t *pcm_buf;
static uint32_t pcm_phys;

/* ── State ──────────────────────────────────────────────────────── */

static int     hda_found;
static int     hda_volume       = 80;  /* 0–100 */
static int     hda_muted;
static int     hda_playing;
static uint8_t hda_codec_addr;         /* First codec address (usually 0) */
static uint8_t hda_dac_nid;            /* DAC widget node ID */
static uint8_t hda_pin_nid;            /* Output pin widget node ID */
static int     hda_num_output_streams; /* Number of output streams */
static int     hda_num_input_streams;  /* Number of input streams */
static uint8_t hda_irq;               /* PCI IRQ line */

/* Output stream base offset (first output stream = after input streams) */
static uint32_t hda_out_sd_base(void) {
    return HDA_SD_BASE + (uint32_t)hda_num_input_streams * HDA_SD_SIZE;
}

/* ── Codec verb submission ──────────────────────────────────────── */

static uint32_t hda_make_verb(uint8_t codec, uint8_t nid,
                              uint32_t verb, uint32_t payload) {
    return ((uint32_t)codec << 28) | ((uint32_t)nid << 20) |
           (verb << 8) | payload;
}

static uint32_t hda_make_verb12(uint8_t codec, uint8_t nid,
                                uint16_t verb, uint8_t payload) {
    return ((uint32_t)codec << 28) | ((uint32_t)nid << 20) |
           ((uint32_t)verb << 8) | payload;
}

static uint32_t hda_make_verb4(uint8_t codec, uint8_t nid,
                               uint16_t verb, uint16_t payload) {
    return ((uint32_t)codec << 28) | ((uint32_t)nid << 20) |
           ((uint32_t)(verb & 0xF) << 16) | payload;
}

static int corb_send(uint32_t verb) {
    uint16_t wp = hda_read16(HDA_CORBWP) & 0xFF;
    uint16_t next = (wp + 1) % CORB_ENTRIES;
    corb_buf[next] = verb;
    hda_write16(HDA_CORBWP, next);
    return 0;
}

static int rirb_read(uint32_t *response) {
    unsigned int timeout = 1000;
    while (timeout--) {
        uint16_t wp = hda_read16(HDA_RIRBWP) & 0xFF;
        if (wp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) % RIRB_ENTRIES;
            uint64_t entry = rirb_buf[rirb_rp];
            *response = (uint32_t)(entry & 0xFFFFFFFF);
            /* Clear RIRB interrupt status */
            hda_write8(HDA_RIRBSTS, 0x05);
            return 0;
        }
        /* Small delay */
        for (volatile int i = 0; i < 100; i++) { }
    }
    return -1;
}

static int hda_send_verb(uint8_t codec, uint8_t nid,
                         uint32_t verb_id, uint32_t payload,
                         uint32_t *result) {
    uint32_t verb;
    if (verb_id >= 0x700) {
        /* 12-bit verb (SET/GET verbs) */
        verb = hda_make_verb12(codec, nid, (uint16_t)verb_id, (uint8_t)payload);
    } else if ((verb_id & 0xF00) == 0xF00 || (verb_id & 0xF00) == 0x300 ||
               (verb_id & 0xF00) == 0x200) {
        /* 4-bit verb with 16-bit payload */
        verb = hda_make_verb4(codec, nid, (uint16_t)verb_id, (uint16_t)payload);
    } else {
        verb = hda_make_verb(codec, nid, verb_id, payload);
    }
    corb_send(verb);
    if (result)
        return rirb_read(result);
    /* If no result needed, still drain RIRB */
    uint32_t dummy;
    rirb_read(&dummy);
    return 0;
}

static uint32_t hda_get_param(uint8_t codec, uint8_t nid, uint8_t param) {
    uint32_t resp = 0;
    uint32_t verb = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) |
                    ((uint32_t)HDA_VERB_GET_PARAM << 8) | param;
    corb_send(verb);
    rirb_read(&resp);
    return resp;
}

/* ── Controller reset ───────────────────────────────────────────── */

static int hda_reset(void) {
    /* Enter reset */
    hda_write32(HDA_GCTL, 0);
    unsigned int timeout = 1000;
    while ((hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout--)
        for (volatile int i = 0; i < 100; i++) { }

    /* Exit reset */
    hda_write32(HDA_GCTL, HDA_GCTL_CRST);
    timeout = 1000;
    while (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout--)
        for (volatile int i = 0; i < 100; i++) { }

    if (!timeout) return -1;

    /* Wait for codecs to enumerate */
    timeout = 1000;
    while (!(hda_read16(HDA_STATESTS) & 0x01) && timeout--)
        for (volatile int i = 0; i < 200; i++) { }

    return 0;
}

/* ── CORB/RIRB setup ───────────────────────────────────────────── */

static int hda_setup_corb_rirb(void) {
    /* Allocate CORB (256 entries × 4 bytes = 1024 bytes) */
    corb_phys = physmem_alloc_region(4096, 4096);
    if (!corb_phys) return -1;
    paging_map_mmio(corb_phys);
    corb_buf = (volatile uint32_t *)(uintptr_t)corb_phys;
    memset((void *)corb_buf, 0, CORB_ENTRIES * 4);

    /* Allocate RIRB (256 entries × 8 bytes = 2048 bytes) */
    rirb_phys = physmem_alloc_region(4096, 4096);
    if (!rirb_phys) return -1;
    paging_map_mmio(rirb_phys);
    rirb_buf = (volatile uint64_t *)(uintptr_t)rirb_phys;
    memset((void *)rirb_buf, 0, RIRB_ENTRIES * 8);

    /* Stop CORB/RIRB DMA */
    hda_write8(HDA_CORBCTL, 0);
    hda_write8(HDA_RIRBCTL, 0);

    /* Set CORB size to 256 entries */
    hda_write8(HDA_CORBSIZE, 0x02);  /* 256 entries */
    hda_write32(HDA_CORBLBASE, corb_phys);
    hda_write32(HDA_CORBUBASE, 0);

    /* Reset CORB read pointer */
    hda_write16(HDA_CORBRP, (1 << 15));  /* Set reset bit */
    unsigned int timeout = 100;
    while (!(hda_read16(HDA_CORBRP) & (1 << 15)) && timeout--)
        for (volatile int i = 0; i < 100; i++) { }
    hda_write16(HDA_CORBRP, 0);          /* Clear reset bit */
    timeout = 100;
    while ((hda_read16(HDA_CORBRP) & (1 << 15)) && timeout--)
        for (volatile int i = 0; i < 100; i++) { }

    /* Reset CORB write pointer */
    hda_write16(HDA_CORBWP, 0);

    /* Set RIRB size to 256 entries */
    hda_write8(HDA_RIRBSIZE, 0x02);  /* 256 entries */
    hda_write32(HDA_RIRBLBASE, rirb_phys);
    hda_write32(HDA_RIRBUBASE, 0);

    /* Reset RIRB write pointer */
    hda_write16(HDA_RIRBWP, (1 << 15));  /* Reset */

    rirb_rp = 0;

    /* Start CORB/RIRB DMA */
    hda_write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    hda_write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN);

    return 0;
}

/* ── Codec discovery ────────────────────────────────────────────── */

static int hda_discover_codec(void) {
    uint16_t statests = hda_read16(HDA_STATESTS);
    hda_codec_addr = 0xFF;

    for (int i = 0; i < 15; i++) {
        if (statests & (1 << i)) {
            hda_codec_addr = (uint8_t)i;
            break;
        }
    }

    if (hda_codec_addr == 0xFF) {
        klog_write(KLOG_LEVEL_WARN, "hda", "No codec found");
        return -1;
    }

    uint32_t vendor = hda_get_param(hda_codec_addr, 0, HDA_PARAM_VENDOR_ID);
    klog_write(KLOG_LEVEL_INFO, "hda", "Codec vendor/device:");
    serial_print("  HDA codec: ");
    serial_print_hex(vendor >> 16);
    serial_print(":");
    serial_print_hex(vendor & 0xFFFF);
    serial_print("\n");

    /* Get root node count to find audio function groups */
    uint32_t node_count = hda_get_param(hda_codec_addr, 0, HDA_PARAM_NODE_COUNT);
    uint8_t start_nid = (uint8_t)((node_count >> 16) & 0xFF);
    uint8_t num_nodes = (uint8_t)(node_count & 0xFF);

    /* Find Audio Function Group */
    uint8_t afg_nid = 0;
    for (uint8_t n = start_nid; n < start_nid + num_nodes; n++) {
        uint32_t fg_type = hda_get_param(hda_codec_addr, n, HDA_PARAM_FN_GROUP_TYPE);
        if ((fg_type & 0xFF) == 0x01) {  /* Audio function group */
            afg_nid = n;
            break;
        }
    }

    if (!afg_nid) {
        klog_write(KLOG_LEVEL_WARN, "hda", "No audio function group");
        return -1;
    }

    /* Power on the AFG */
    hda_send_verb(hda_codec_addr, afg_nid, HDA_VERB_SET_POWER_STATE, 0x00, 0);

    /* Enumerate widgets in the AFG */
    node_count = hda_get_param(hda_codec_addr, afg_nid, HDA_PARAM_NODE_COUNT);
    start_nid = (uint8_t)((node_count >> 16) & 0xFF);
    num_nodes = (uint8_t)(node_count & 0xFF);

    hda_dac_nid = 0;
    hda_pin_nid = 0;

    for (uint8_t n = start_nid; n < start_nid + num_nodes; n++) {
        uint32_t wcap = hda_get_param(hda_codec_addr, n, HDA_PARAM_AUDIO_WIDGET_CAP);
        uint8_t wtype = (uint8_t)((wcap >> 20) & 0xF);

        if (wtype == HDA_WIDGET_AUD_OUT && !hda_dac_nid) {
            hda_dac_nid = n;
            serial_print("  DAC node: ");
            serial_print_hex(n);
            serial_print("\n");
        } else if (wtype == HDA_WIDGET_PIN && !hda_pin_nid) {
            uint32_t pin_cap = hda_get_param(hda_codec_addr, n, HDA_PARAM_PIN_CAP);
            if (pin_cap & (1 << 4)) {  /* Output capable */
                hda_pin_nid = n;
                serial_print("  Pin node: ");
                serial_print_hex(n);
                serial_print("\n");
            }
        }
    }

    if (!hda_dac_nid) {
        klog_write(KLOG_LEVEL_WARN, "hda", "No DAC widget found");
        return -1;
    }

    return 0;
}

/* ── Configure output path ──────────────────────────────────────── */

static void hda_setup_output(void) {
    /* Set stream format on DAC: 48kHz, 16-bit, stereo */
    hda_send_verb(hda_codec_addr, hda_dac_nid,
                  HDA_VERB_SET_STREAM_FMT, HDA_FMT_48KHZ_16BIT_STEREO, 0);

    /* Set converter control: stream tag=1, channel=0 */
    hda_send_verb(hda_codec_addr, hda_dac_nid,
                  HDA_VERB_SET_CONV_CTRL, 0x10, 0);  /* stream=1, chan=0 */

    /* Power on DAC */
    hda_send_verb(hda_codec_addr, hda_dac_nid,
                  HDA_VERB_SET_POWER_STATE, 0x00, 0);

    /* Configure output pin if found */
    if (hda_pin_nid) {
        /* Enable output + headphone */
        hda_send_verb(hda_codec_addr, hda_pin_nid,
                      HDA_VERB_SET_PIN_WIDGET, HDA_PIN_OUT_EN | HDA_PIN_HP_EN, 0);

        /* Power on pin */
        hda_send_verb(hda_codec_addr, hda_pin_nid,
                      HDA_VERB_SET_POWER_STATE, 0x00, 0);

        /* EAPD enable if supported */
        hda_send_verb(hda_codec_addr, hda_pin_nid,
                      HDA_VERB_SET_EAPD, 0x02, 0);
    }

    /* Set initial volume on DAC */
    hda_set_volume(hda_volume);
}

/* ── BDL + output stream setup ──────────────────────────────────── */

static int hda_setup_stream(void) {
    /* Allocate BDL (aligned to 128 bytes) */
    bdl_phys = physmem_alloc_region(4096, 4096);
    if (!bdl_phys) return -1;
    paging_map_mmio(bdl_phys);
    bdl = (volatile hda_bdl_entry_t *)(uintptr_t)bdl_phys;
    memset((void *)bdl, 0, 4096);

    /* Allocate PCM buffer (page-aligned) */
    uint32_t pcm_size = PCM_BUF_SIZE;
    uint32_t alloc_pages = (pcm_size + 4095) & ~4095U;
    pcm_phys = physmem_alloc_region(alloc_pages, 4096);
    if (!pcm_phys) return -1;
    paging_map_mmio(pcm_phys);
    pcm_buf = (volatile int16_t *)(uintptr_t)pcm_phys;
    memset((void *)pcm_buf, 0, pcm_size);

    /* Set up 2 BDL entries (double buffer) */
    uint32_t half = pcm_size / 2;
    bdl[0].addr_lo = pcm_phys;
    bdl[0].addr_hi = 0;
    bdl[0].length  = half;
    bdl[0].ioc     = 1;

    bdl[1].addr_lo = pcm_phys + half;
    bdl[1].addr_hi = 0;
    bdl[1].length  = half;
    bdl[1].ioc     = 1;

    /* Configure output stream descriptor */
    uint32_t sd = hda_out_sd_base();

    /* Reset stream */
    hda_write8(sd + HDA_SD_CTL, 0);
    unsigned int timeout = 100;
    while ((hda_read8(sd + HDA_SD_CTL) & HDA_SD_CTL_RUN) && timeout--)
        for (volatile int i = 0; i < 100; i++) { }

    /* Set stream tag and channel (stream tag = 1) */
    uint8_t ctl_upper = (1 << 4);  /* stream tag = 1 in bits 7:4 of CTL[23:16] */
    hda_write8(sd + HDA_SD_CTL + 2, ctl_upper);

    /* Set format: 48kHz, 16-bit, stereo */
    hda_write16(sd + HDA_SD_FMT, HDA_FMT_48KHZ_16BIT_STEREO);

    /* Set BDL pointer */
    hda_write32(sd + HDA_SD_BDLPL, bdl_phys);
    hda_write32(sd + HDA_SD_BDLPU, 0);

    /* Set cyclic buffer length */
    hda_write32(sd + HDA_SD_CBL, pcm_size);

    /* Set last valid index (2 BDL entries → LVI = 1) */
    hda_write16(sd + HDA_SD_LVI, BDL_ENTRIES - 1);

    return 0;
}

/* ── Public API ─────────────────────────────────────────────────── */

int hda_init(void) {
    hda_found = 0;

    /* Find HDA controller on PCI bus */
    const pci_device_t *dev = pci_find_class(HDA_PCI_CLASS, HDA_PCI_SUBCLASS);
    if (!dev) {
        klog_write(KLOG_LEVEL_INFO, "hda", "No HDA controller found");
        return -1;
    }

    klog_write(KLOG_LEVEL_INFO, "hda", "HDA controller found");
    serial_print("  HDA PCI: ");
    serial_print_hex(dev->vendor_id);
    serial_print(":");
    serial_print_hex(dev->device_id);
    serial_print(" IRQ=");
    serial_print_int(dev->irq_line);
    serial_print("\n");

    /* Enable bus mastering for DMA */
    pci_enable_bus_mastering(dev);

    /* Map BAR0 (MMIO registers) */
    uint32_t bar0 = dev->bar[0] & ~0xFU;
    if (!bar0) {
        klog_write(KLOG_LEVEL_WARN, "hda", "BAR0 is zero");
        return -1;
    }
    paging_map_mmio(bar0);
    hda_base = (volatile uint8_t *)(uintptr_t)bar0;
    hda_irq = dev->irq_line;

    /* Read capabilities */
    uint16_t gcap = hda_read16(HDA_GCAP);
    hda_num_output_streams = (gcap >> 12) & 0xF;
    hda_num_input_streams  = (gcap >> 8) & 0xF;

    serial_print("  HDA GCAP: ");
    serial_print_hex(gcap);
    serial_print(" out_streams=");
    serial_print_int(hda_num_output_streams);
    serial_print(" in_streams=");
    serial_print_int(hda_num_input_streams);
    serial_print("\n");

    if (hda_num_output_streams == 0) {
        klog_write(KLOG_LEVEL_WARN, "hda", "No output streams available");
        return -1;
    }

    /* Reset controller */
    if (hda_reset() < 0) {
        klog_write(KLOG_LEVEL_WARN, "hda", "Controller reset failed");
        return -1;
    }

    /* Setup command/response rings */
    if (hda_setup_corb_rirb() < 0) {
        klog_write(KLOG_LEVEL_WARN, "hda", "CORB/RIRB setup failed");
        return -1;
    }

    /* Discover and configure codec */
    if (hda_discover_codec() < 0) {
        klog_write(KLOG_LEVEL_WARN, "hda", "Codec discovery failed");
        return -1;
    }

    /* Setup output path in codec */
    hda_setup_output();

    /* Setup output stream and BDL */
    if (hda_setup_stream() < 0) {
        klog_write(KLOG_LEVEL_WARN, "hda", "Stream setup failed");
        return -1;
    }

    /* Enable interrupts */
    uint32_t intctl = HDA_INTCTL_GIE | HDA_INTCTL_CIE;
    /* Enable interrupt for first output stream */
    intctl |= (1U << hda_num_input_streams);
    hda_write32(HDA_INTCTL, intctl);

    /* Route HDA PCI IRQ to vector 49 via IOAPIC (level-triggered) */
    if (apic_is_enabled() && hda_irq < 24) {
        ioapic_route_irq_level(hda_irq, 49, 0);
        serial_print("  HDA IRQ ");
        serial_print_int(hda_irq);
        serial_print(" -> vector 49 (IOAPIC)\n");
    }

    hda_found = 1;
    klog_write(KLOG_LEVEL_INFO, "hda", "Driver ready");
    return 0;
}

int hda_is_present(void) {
    return hda_found;
}

void hda_set_volume(int level) {
    if (level < 0)   level = 0;
    if (level > 100)  level = 100;
    hda_volume = level;

    if (!hda_found || !hda_dac_nid) return;

    /* HDA amp gain is typically 0–127 (7 bits). Scale 0–100 → 0–127. */
    uint8_t gain = (uint8_t)((level * 127) / 100);
    uint16_t payload = HDA_AMP_OUTPUT | HDA_AMP_LEFT | HDA_AMP_RIGHT | gain;
    if (hda_muted)
        payload |= HDA_AMP_MUTE;

    uint32_t verb = ((uint32_t)hda_codec_addr << 28) |
                    ((uint32_t)hda_dac_nid << 20) |
                    ((uint32_t)0x3 << 16) | payload;
    corb_send(verb);
    uint32_t dummy;
    rirb_read(&dummy);
}

int hda_get_volume(void) {
    return hda_volume;
}

void hda_set_mute(int muted) {
    hda_muted = muted ? 1 : 0;
    /* Re-apply volume to update mute bit */
    hda_set_volume(hda_volume);
}

int hda_get_mute(void) {
    return hda_muted;
}

int hda_play_pcm(const int16_t *data, unsigned int frames) {
    if (!hda_found) return -1;

    /* Cap to buffer size */
    unsigned int max_frames = PCM_BUF_SIZE / 4;  /* 4 bytes per stereo frame */
    if (frames > max_frames)
        frames = max_frames;

    /* Copy PCM data to DMA buffer */
    unsigned int bytes = frames * 4;
    memcpy((void *)pcm_buf, (const void *)data, bytes);

    /* Pad remainder with silence */
    if (bytes < PCM_BUF_SIZE)
        memset((void *)((uint8_t *)pcm_buf + bytes), 0, PCM_BUF_SIZE - bytes);

    /* Update BDL buffer lengths to match actual data */
    uint32_t half = bytes / 2;
    if (half == 0) half = 4;
    bdl[0].length = half;
    bdl[1].length = bytes - half;

    /* Update CBL */
    uint32_t sd = hda_out_sd_base();
    hda_write32(sd + HDA_SD_CBL, bytes);

    /* Enable IOC + stream run */
    uint8_t ctl = hda_read8(sd + HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    hda_write8(sd + HDA_SD_CTL, ctl);

    hda_playing = 1;
    return 0;
}

void hda_stop(void) {
    if (!hda_found) return;

    uint32_t sd = hda_out_sd_base();
    uint8_t ctl = hda_read8(sd + HDA_SD_CTL);
    ctl &= ~(uint8_t)HDA_SD_CTL_RUN;
    hda_write8(sd + HDA_SD_CTL, ctl);

    hda_playing = 0;
}

int hda_is_playing(void) {
    return hda_playing;
}

void hda_irq_handler(void) {
    if (!hda_found) return;

    uint32_t intsts = hda_read32(HDA_INTSTS);

    /* Check output stream interrupt */
    if (intsts & (1U << hda_num_input_streams)) {
        uint32_t sd = hda_out_sd_base();
        uint8_t sts = hda_read8(sd + HDA_SD_STS);

        if (sts & HDA_SD_STS_BCIS) {
            /* Buffer completion — stop if not looping */
            hda_write8(sd + HDA_SD_STS, HDA_SD_STS_BCIS);  /* W1C */
            hda_playing = 0;

            /* Stop the stream */
            uint8_t ctl = hda_read8(sd + HDA_SD_CTL);
            ctl &= ~(uint8_t)HDA_SD_CTL_RUN;
            hda_write8(sd + HDA_SD_CTL, ctl);
        }
        if (sts & HDA_SD_STS_FIFOE) {
            hda_write8(sd + HDA_SD_STS, HDA_SD_STS_FIFOE);
        }
        if (sts & HDA_SD_STS_DESE) {
            hda_write8(sd + HDA_SD_STS, HDA_SD_STS_DESE);
        }
    }
}
