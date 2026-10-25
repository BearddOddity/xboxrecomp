/*
 * MCPX APU Audio Emulation - Public API
 *
 * Extracted from xemu (LGPL v2+), adapted for standalone use.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MCPXAPUState MCPXAPUState;

/* Initialize the APU emulation.
 * ram_ptr: pointer to the base of Xbox physical RAM (64MB).
 * Returns the APU state, or NULL on failure. */
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);

/* Shut down and free the APU state. */
void mcpx_apu_shutdown(MCPXAPUState *d);

/* MMIO read from APU register space (addr is offset from 0xFE800000). */
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);

/* MMIO write to APU register space (addr is offset from 0xFE800000). */
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

/* Store the DSP "alive" word (0xCCCCCC at 0xFE85A018, which every XDK
 * DirectSound reads back in DirectSoundDoWork) into the MCPX aperture, whose
 * GP/EP memory is plain RAM. Call once when the aperture is created; see
 * RECOMP_APU_DSP_ALIVE in docs/pipeline/06-debugging.md. Returns 1 if stored. */
int mcpx_apu_seed_dsp_memory(void *aperture, size_t size);

/* SAGE (Smooth Audio Gain Engine): an opt-in for a newly recompiled title.
 *
 * RECOMP_SAGE=1 puts the final mix through a soft output limiter
 * (ceiling 0.95 of full scale) instead of the hard clamp that turns a loud
 * scene into harsh distortion, and rounds to 16 bits rather than truncating.
 * Off, the mix is unchanged to the bit. RECOMP_AUDIO_LIMITER=<0.1..1.0> sets
 * the ceiling on its own (0 turns the limiter off, even under SAGE).
 * RECOMP_APU_MIXSTAT=<seconds> prints "[APU-MIX]" lines with the loudest
 * peak and how many samples would have clipped, to see whether it is needed.
 *
 * Output levels (linear, 1.0 = unchanged, the default). The master level
 * scales the whole mix; each voice belongs to one of 8 classes whose level
 * scales it too: class 0 for a one-shot or looping buffer, class 1 for a
 * streamed voice (music and speech decoded on the CPU) unless the title says
 * otherwise. RECOMP_AUDIO_VOL_MASTER and RECOMP_AUDIO_VOL_CLASS0..7 give the
 * starting levels. */
void mcpx_apu_set_master_gain(float gain);
void mcpx_apu_set_class_gain(int cls, float gain);
void mcpx_apu_set_voice_class(int voice, int cls);   /* cls -1: back to default */
void mcpx_apu_set_limiter(float ceiling);            /* 0.1..1.0, or 0 for off */

/* What the mix and the frame thread did since the last reset, for an overlay
 * or a log: the same numbers RECOMP_APU_MIXSTAT prints. over_full_scale is
 * counted before the limiter, so it says whether the title needs SAGE.
 * frames[] counts the thread's frames by kind; APU_FRAME_SILENT_COUNTER_OFF
 * climbing is a title "recovering" a DSP it thinks is dead (see
 * RECOMP_APU_DSP_ALIVE in docs/pipeline/06-debugging.md). */
enum {
    APU_FRAME_FULL,                 /* the whole pipeline ran */
    APU_FRAME_TRAPPED,              /* front end trapped: no frame, as xemu */
    APU_FRAME_SILENT_COUNTER_OFF,   /* lightweight: SECTL sample counter off */
    APU_FRAME_SILENT_HALTED,        /* lightweight: front end halted */
    APU_FRAME_SILENT_OTHER,         /* lightweight: test tone */
    APU_FRAME_KINDS
};
typedef struct MCPXMixStats {
    unsigned long long samples, over_full_scale, limited;
    float peak;                                  /* raw, before the limiter */
    unsigned long long frames[APU_FRAME_KINDS];
} MCPXMixStats;
void mcpx_apu_mix_stats(MCPXMixStats *out, int reset);
void mcpx_apu_note_frame(int kind);              /* the frame thread's own call */

/* A diagnostic switch (RECOMP_APU_TRACE, RECOMP_APU_RING, ...) is on when
 * set, non-empty and not starting with '0'. */
int apu_env_on(const char *name);

/* Play a 440Hz test tone through the APU pipeline to verify audio output.
 * Directly programs a voice without going through DirectSound. */
void mcpx_apu_play_test_tone(MCPXAPUState *d);

/* ============================================================
 * Software mixer - DirectSound buffer bridge
 *
 * DirectSound buffers register here. The APU frame thread
 * mixes active buffers directly into the waveOut output,
 * bypassing the VP hardware voice pipeline.
 * ============================================================ */

#define APU_MIXER_MAX_VOICES 64

typedef struct APUMixerVoice {
    volatile int     active;       /* 1 = playing, 0 = stopped */
    volatile int     looping;      /* 1 = loop, 0 = one-shot */
    const int16_t   *pcm_data;     /* Pointer to 16-bit PCM samples */
    uint32_t         pcm_bytes;    /* Total buffer size in bytes */
    uint32_t         num_channels;  /* 1=mono, 2=stereo */
    uint32_t         sample_rate;   /* e.g. 22050, 44100, 48000 */
    uint64_t         play_offset; /* Source frame position, 16 fractional bits */
    float            volume;       /* 0.0 to 1.0 */
} APUMixerVoice;

/* Allocate a mixer voice slot. Returns slot index or -1 if full. */
int apu_mixer_alloc_voice(void);

/* Free a mixer voice slot. */
void apu_mixer_free_voice(int slot);

/* Get pointer to a mixer voice for configuration. */
APUMixerVoice *apu_mixer_get_voice(int slot);

/* Seek in PCM bytes (rounded down to a whole frame). Returns 0 if invalid. */
int apu_mixer_set_position(int slot, uint32_t byte_offset);

/* Snapshot playback state under the mixer lock; output pointers may be NULL. */
void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping);

/* Start/resume or stop playback without changing the current position. */
void apu_mixer_play(int slot, int looping);
void apu_mixer_stop(int slot);

#ifdef __cplusplus
}
#endif
