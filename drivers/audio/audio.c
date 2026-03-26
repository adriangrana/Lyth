/* ============================================================
 *  audio.c  —  Audio subsystem / system sounds
 *
 *  Wraps the HDA driver and provides a high-level API for
 *  system sounds (click, notify, error, startup, logout).
 *
 *  Tone generation: simple sine-approximation via parabolic
 *  wave (no floating point, no SSE — pure integer math).
 * ============================================================ */

#include "audio.h"
#include "hda.h"
#include "klog.h"
#include "string.h"
#include "physmem.h"
#include "paging.h"

/* ── State ──────────────────────────────────────────────────────── */

static int audio_state = AUDIO_STATE_NONE;

/* ── Integer tone generation ────────────────────────────────────── */

/*
 * Attempt a sine-like waveform using a parabolic approximation.
 * Input: phase in range [0, period) representing 0..2π
 * Output: amplitude in range [-amplitude, +amplitude]
 */
static int16_t tone_sample(int phase, int period, int amplitude) {
    /* Normalise phase to [0, period) */
    phase = phase % period;
    if (phase < 0) phase += period;

    /* Map to [-half, +half] */
    int half = period / 2;
    int x;
    if (phase < half) {
        /* First half: 0 → peak → 0 */
        x = phase * 2 - half;  /* range [-half, half) */
    } else {
        /* Second half: 0 → trough → 0 */
        x = (phase - half) * 2 - half;
        x = -x;               /* flip for negative half */
    }

    /* Parabolic approx: y = amplitude * (1 - (2x/period)^2) */
    /* But simpler: triangle wave scaled → good enough for beeps */
    int val = (x * amplitude * 2) / period;

    /* Clamp */
    if (val > 32767) val = 32767;
    if (val < -32767) val = -32767;
    return (int16_t)val;
}

/* Generate a tone into a PCM buffer (stereo 16-bit, 48kHz).
 * Returns number of stereo frames written. */
static unsigned int generate_tone(int16_t *buf, unsigned int max_frames,
                                  int freq_hz, int duration_ms,
                                  int amplitude) {
    unsigned int sample_rate = 48000;
    unsigned int frames = (sample_rate * (unsigned int)duration_ms) / 1000;
    if (frames > max_frames) frames = max_frames;

    int period = (int)sample_rate / freq_hz;
    if (period < 1) period = 1;

    /* Fade-out envelope: last 20% fades to zero */
    unsigned int fade_start = frames - frames / 5;

    for (unsigned int i = 0; i < frames; i++) {
        int16_t s = tone_sample((int)i, period, amplitude);

        /* Apply fade-out */
        if (i > fade_start) {
            unsigned int remaining = frames - i;
            unsigned int fade_len = frames - fade_start;
            s = (int16_t)((int)s * (int)remaining / (int)fade_len);
        }

        buf[i * 2]     = s;  /* Left */
        buf[i * 2 + 1] = s;  /* Right */
    }

    return frames;
}

/* Generate a two-tone sequence */
static unsigned int generate_two_tone(int16_t *buf, unsigned int max_frames,
                                      int freq1, int dur1_ms,
                                      int freq2, int dur2_ms,
                                      int amplitude) {
    unsigned int f1 = generate_tone(buf, max_frames, freq1, dur1_ms, amplitude);
    unsigned int f2 = generate_tone(buf + f1 * 2, max_frames - f1,
                                    freq2, dur2_ms, amplitude);
    return f1 + f2;
}

/* ── System sound definitions ───────────────────────────────────── */

/* Temporary buffer for generated tones (allocated once) */
static int16_t *snd_buf;
static uint32_t snd_buf_phys;
#define SND_BUF_FRAMES  48000   /* 1 second max */

static int ensure_snd_buf(void) {
    if (snd_buf) return 0;
    uint32_t size = SND_BUF_FRAMES * 4;  /* stereo 16-bit */
    uint32_t alloc = (size + 4095) & ~4095U;
    snd_buf_phys = physmem_alloc_region(alloc, 4096);
    if (!snd_buf_phys) return -1;
    paging_map_mmio(snd_buf_phys);
    snd_buf = (int16_t *)(uintptr_t)snd_buf_phys;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────── */

int audio_init(void) {
    if (hda_init() == 0) {
        audio_state = AUDIO_STATE_READY;
        klog_write(KLOG_LEVEL_INFO, "audio", "Subsystem ready (Intel HDA)");
        return 0;
    }

    klog_write(KLOG_LEVEL_INFO, "audio", "No audio hardware detected");
    audio_state = AUDIO_STATE_NONE;
    return -1;
}

int audio_get_state(void) {
    if (audio_state == AUDIO_STATE_READY && hda_is_playing())
        return AUDIO_STATE_PLAYING;
    return audio_state;
}

void audio_set_volume(int level) {
    hda_set_volume(level);
}

int audio_get_volume(void) {
    return hda_get_volume();
}

void audio_set_mute(int muted) {
    hda_set_mute(muted);
}

int audio_get_mute(void) {
    return hda_get_mute();
}

void audio_play_sound(int sound_id) {
    if (audio_state == AUDIO_STATE_NONE) return;
    if (ensure_snd_buf() < 0) return;

    memset(snd_buf, 0, SND_BUF_FRAMES * 4);
    unsigned int frames = 0;

    switch (sound_id) {
    case SND_CLICK:
        /* Short, subtle click: high freq, very short */
        frames = generate_tone(snd_buf, SND_BUF_FRAMES, 1200, 30, 8000);
        break;

    case SND_NOTIFY:
        /* Pleasant two-tone chime: C5→E5 */
        frames = generate_two_tone(snd_buf, SND_BUF_FRAMES,
                                   523, 120, 659, 150, 12000);
        break;

    case SND_ERROR:
        /* Low, attention-getting double beep */
        frames = generate_two_tone(snd_buf, SND_BUF_FRAMES,
                                   300, 100, 200, 150, 14000);
        break;

    case SND_STARTUP:
        /* Rising three-note chime: C4→E4→G4 */
        frames = generate_two_tone(snd_buf, SND_BUF_FRAMES,
                                   262, 200, 330, 200, 10000);
        {
            unsigned int f3 = generate_tone(snd_buf + frames * 2,
                                            SND_BUF_FRAMES - frames,
                                            392, 300, 10000);
            frames += f3;
        }
        break;

    case SND_LOGOUT:
        /* Descending two-tone: G4→C4 */
        frames = generate_two_tone(snd_buf, SND_BUF_FRAMES,
                                   392, 200, 262, 300, 10000);
        break;

    default:
        return;
    }

    if (frames > 0)
        hda_play_pcm(snd_buf, frames);
}

int audio_play_pcm(const int16_t *data, unsigned int frames) {
    if (audio_state == AUDIO_STATE_NONE) return -1;
    return hda_play_pcm(data, frames);
}

void audio_stop(void) {
    hda_stop();
}

const char *audio_backend_name(void) {
    if (hda_is_present())
        return "Intel HDA";
    return "None";
}
