#ifndef HDA_H
#define HDA_H

#include <stdint.h>

/* ── Intel HDA PCI identifiers ─────────────────────────────────── */
#define HDA_PCI_CLASS       0x04   /* Multimedia controller */
#define HDA_PCI_SUBCLASS    0x03   /* Audio device */

/* ── HDA controller registers (offsets from BAR0) ──────────────── */
#define HDA_GCAP            0x00   /* Global Capabilities */
#define HDA_VMIN            0x02   /* Minor Version */
#define HDA_VMAJ            0x03   /* Major Version */
#define HDA_OUTPAY          0x04   /* Output Payload Capability */
#define HDA_INPAY           0x06   /* Input Payload Capability */
#define HDA_GCTL            0x08   /* Global Control */
#define HDA_WAKEEN          0x0C   /* Wake Enable */
#define HDA_STATESTS        0x0E   /* State Change Status */
#define HDA_GSTS            0x10   /* Global Status */
#define HDA_INTCTL          0x20   /* Interrupt Control */
#define HDA_INTSTS          0x24   /* Interrupt Status */
#define HDA_WALCLK          0x30   /* Wall Clock Counter */
#define HDA_SSYNC           0x38   /* Stream Synchronization */

/* CORB registers */
#define HDA_CORBLBASE       0x40   /* CORB Lower Base Address */
#define HDA_CORBUBASE       0x44   /* CORB Upper Base Address */
#define HDA_CORBWP          0x48   /* CORB Write Pointer */
#define HDA_CORBRP          0x4A   /* CORB Read Pointer */
#define HDA_CORBCTL         0x4C   /* CORB Control */
#define HDA_CORBSTS         0x4D   /* CORB Status */
#define HDA_CORBSIZE        0x4E   /* CORB Size */

/* RIRB registers */
#define HDA_RIRBLBASE       0x50   /* RIRB Lower Base Address */
#define HDA_RIRBUBASE       0x54   /* RIRB Upper Base Address */
#define HDA_RIRBWP          0x58   /* RIRB Write Pointer */
#define HDA_RINTCNT         0x5A   /* Response Interrupt Count */
#define HDA_RIRBCTL         0x5C   /* RIRB Control */
#define HDA_RIRBSTS         0x5D   /* RIRB Status */
#define HDA_RIRBSIZE        0x5E   /* RIRB Size */

/* Stream Descriptor registers (offset = base + 0x80 + n*0x20) */
#define HDA_SD_BASE         0x80
#define HDA_SD_SIZE         0x20
#define HDA_SD_CTL          0x00   /* Stream Descriptor Control */
#define HDA_SD_STS          0x03   /* Stream Descriptor Status */
#define HDA_SD_LPIB         0x04   /* Link Position in Buffer */
#define HDA_SD_CBL          0x08   /* Cyclic Buffer Length */
#define HDA_SD_LVI          0x0C   /* Last Valid Index */
#define HDA_SD_FIFOW        0x0E   /* FIFO Watermark */
#define HDA_SD_FIFOS        0x10   /* FIFO Size */
#define HDA_SD_FMT          0x12   /* Stream Format */
#define HDA_SD_BDLPL        0x18   /* BDL Pointer Lower */
#define HDA_SD_BDLPU        0x1C   /* BDL Pointer Upper */

/* GCTL bits */
#define HDA_GCTL_CRST       (1 << 0)   /* Controller Reset */

/* INTCTL bits */
#define HDA_INTCTL_GIE      (1U << 31) /* Global Interrupt Enable */
#define HDA_INTCTL_CIE      (1U << 30) /* Controller Interrupt Enable */

/* CORBCTL bits */
#define HDA_CORBCTL_RUN     (1 << 1)   /* CORB DMA Run */

/* RIRBCTL bits */
#define HDA_RIRBCTL_RUN     (1 << 1)   /* RIRB DMA Run */
#define HDA_RIRBCTL_INT     (1 << 0)   /* RIRB Interrupt Enable */

/* SD_CTL bits */
#define HDA_SD_CTL_RUN      (1 << 1)   /* Stream Run */
#define HDA_SD_CTL_IOCE     (1 << 2)   /* Interrupt on Completion Enable */
#define HDA_SD_CTL_STRIPE   (1 << 4)   /* Stripe Control */
#define HDA_SD_CTL_DEIE     (1 << 5)   /* Descriptor Error Interrupt Enable */
#define HDA_SD_CTL_FEIE     (1 << 6)   /* FIFO Error Interrupt Enable */

/* SD_STS bits */
#define HDA_SD_STS_BCIS     (1 << 2)   /* Buffer Completion Interrupt Status */
#define HDA_SD_STS_FIFOE    (1 << 3)   /* FIFO Error */
#define HDA_SD_STS_DESE     (1 << 4)   /* Descriptor Error */

/* ── HDA codec verbs ───────────────────────────────────────────── */
#define HDA_VERB_GET_PARAM          0xF00
#define HDA_VERB_SET_STREAM_FMT     0x200
#define HDA_VERB_GET_STREAM_FMT     0xA00
#define HDA_VERB_SET_AMP_GAIN       0x300
#define HDA_VERB_GET_AMP_GAIN       0xB00
#define HDA_VERB_SET_PIN_WIDGET     0x707
#define HDA_VERB_GET_PIN_WIDGET     0xF07
#define HDA_VERB_SET_CONV_CTRL      0x706
#define HDA_VERB_GET_CONV_CTRL      0xF06
#define HDA_VERB_SET_EAPD           0x70C
#define HDA_VERB_SET_POWER_STATE    0x705
#define HDA_VERB_GET_CONN_LIST      0xF02
#define HDA_VERB_SET_CONN_SEL       0x701

/* Parameter IDs for GET_PARAM */
#define HDA_PARAM_VENDOR_ID         0x00
#define HDA_PARAM_NODE_COUNT        0x04
#define HDA_PARAM_FN_GROUP_TYPE     0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP  0x09
#define HDA_PARAM_PIN_CAP           0x0C
#define HDA_PARAM_CONN_LIST_LEN     0x0E
#define HDA_PARAM_OUT_AMP_CAP       0x12

/* Widget types (from Audio Widget Capabilities) */
#define HDA_WIDGET_AUD_OUT          0x0
#define HDA_WIDGET_AUD_IN           0x1
#define HDA_WIDGET_AUD_MIX          0x2
#define HDA_WIDGET_AUD_SEL          0x3
#define HDA_WIDGET_PIN              0x4
#define HDA_WIDGET_POWER            0x5
#define HDA_WIDGET_VOL_KNOB         0x6

/* Pin Widget Control bits */
#define HDA_PIN_OUT_EN              (1 << 6)
#define HDA_PIN_HP_EN               (1 << 7)

/* Amp Gain/Mute bits */
#define HDA_AMP_MUTE                (1 << 7)
#define HDA_AMP_LEFT                (1 << 13)
#define HDA_AMP_RIGHT               (1 << 12)
#define HDA_AMP_OUTPUT              (1 << 15)
#define HDA_AMP_INPUT               (1 << 14)

/* ── BDL entry ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t addr_lo;        /* Buffer physical address (lower 32b) */
    uint32_t addr_hi;        /* Buffer physical address (upper 32b) */
    uint32_t length;         /* Buffer length in bytes */
    uint32_t ioc;            /* Interrupt on completion (bit 0) */
} __attribute__((packed)) hda_bdl_entry_t;

/* ── Audio format ──────────────────────────────────────────────── */
#define HDA_FMT_48KHZ_16BIT_STEREO  0x0011  /* 48kHz, 16-bit, 2ch */

/* ── Public API ────────────────────────────────────────────────── */

/* Initialise HDA controller. Returns 0 on success, -1 on failure. */
int  hda_init(void);

/* Returns 1 if HDA controller was detected and initialised */
int  hda_is_present(void);

/* Set master volume (0–100). */
void hda_set_volume(int level);

/* Get current master volume (0–100). */
int  hda_get_volume(void);

/* Mute/unmute output. */
void hda_set_mute(int muted);
int  hda_get_mute(void);

/* Play a PCM buffer (16-bit signed LE, stereo, 48kHz).
 * data   — pointer to PCM samples
 * frames — number of stereo sample pairs (each pair = 4 bytes)
 * Returns 0 on success. */
int  hda_play_pcm(const int16_t *data, unsigned int frames);

/* Stop current playback. */
void hda_stop(void);

/* Returns 1 if currently playing audio */
int  hda_is_playing(void);

/* Called from IRQ handler */
void hda_irq_handler(void);

#endif /* HDA_H */
