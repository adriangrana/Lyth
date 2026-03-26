#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* ── Audio system state ────────────────────────────────────────── */
#define AUDIO_STATE_NONE    0   /* No audio hardware */
#define AUDIO_STATE_READY   1   /* Hardware ready, idle */
#define AUDIO_STATE_PLAYING 2   /* Currently playing */

/* ── System sound identifiers ──────────────────────────────────── */
#define SND_CLICK       0   /* Button click / UI interaction */
#define SND_NOTIFY      1   /* Notification sound */
#define SND_ERROR       2   /* Error beep */
#define SND_STARTUP     3   /* Boot chime */
#define SND_LOGOUT      4   /* Logout chime */
#define SND_COUNT       5

/* ── Public API ────────────────────────────────────────────────── */

/* Initialise the audio subsystem (probes HDA). Returns 0 success. */
int  audio_init(void);

/* Returns current audio state (AUDIO_STATE_*) */
int  audio_get_state(void);

/* Master volume control (0–100) */
void audio_set_volume(int level);
int  audio_get_volume(void);

/* Mute toggle */
void audio_set_mute(int muted);
int  audio_get_mute(void);

/* Play a system sound by ID (non-blocking, fire-and-forget) */
void audio_play_sound(int sound_id);

/* Play raw PCM (16-bit signed LE, stereo, 48kHz). Blocking. */
int  audio_play_pcm(const int16_t *data, unsigned int frames);

/* Stop whatever is playing */
void audio_stop(void);

/* Returns human-readable backend name ("Intel HDA" or "None") */
const char *audio_backend_name(void);

#endif /* AUDIO_H */
