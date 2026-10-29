// Pure C parts of yaykit_speak (host tested with test/run.sh): the playback ring, fades, level and the two cue
// tones. No ESP-IDF; the device wraps the ring in a critical section.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Mono int16 ring, one producer and one consumer. cap must be a power of two. */
typedef struct {
    int16_t *buf;
    size_t cap;
    size_t head, tail;  // free-running counts written / read
} yk_ring_t;

void yk_ring_init(yk_ring_t *r, int16_t *buf, size_t cap);
size_t yk_ring_avail(const yk_ring_t *r);
size_t yk_ring_space(const yk_ring_t *r);
/** Writes as much as fits; returns the samples written. */
size_t yk_ring_write(yk_ring_t *r, const int16_t *x, size_t n);
/** Reads up to n; returns the samples read. */
size_t yk_ring_read(yk_ring_t *r, int16_t *out, size_t n);
void yk_ring_clear(yk_ring_t *r);

/** Linear gain ramp across x, from g0 to g1 (0..1). */
void yk_fade(int16_t *x, size_t n, float g0, float g1);

/** Loudness of a chunk, 0..100: 0 below -60 dBFS RMS, 100 at -6 dBFS and above (for the mouth and the rim). */
uint8_t yk_level16(const int16_t *x, size_t n);

typedef enum { YK_CHIME_START = 1, YK_CHIME_END = 2 } yk_chime_t;

/** The short cue before the microphone opens (rising) or after it closes (falling): two soft notes, about
 *  140 ms, at `rate`. Returns the samples written (at most max). */
size_t yk_chime_synth(yk_chime_t kind, int rate, int16_t *out, size_t max);

/** Volume step 0..10 to the codec's output volume (0..100, roughly dB-scaled; 0 = muted). */
int yk_volume_to_codec(int level);

/** May yk_speak_reclaim() open and close the codec now? Only with it closed (open, the speaker's format is already
 *  set), nothing to play, and the microphones not recording: opening the speaker then would put the shared I2S's
 *  clock (its transmit side, full duplex) back in the speaker's format under the recording. */
bool yk_speak_may_reclaim(bool codec_open, bool playing, bool mic_open);
