// The station list and what is said about it. Plain C, no ESP-IDF: host tested (test/run.sh).
#pragma once

#include <stddef.h>

#define FM_MIN_10 875   // the dial's ends, in 0.1 MHz
#define FM_MAX_10 1080

typedef struct {
    const char *city;  // 北京
    const char *name;  // 北京交通广播
    int mhz10;         // 1039 = FM 103.9
    const char *url;   // an HTTP MP3 stream
} station_t;

int stations_count(void);
/** The station at i, wrapped into the list (any int). */
const station_t *station_at(int i);
/** i moved by delta, wrapped: the next (+1) or previous (-1) station. */
int station_step(int i, int delta);
/** 「103.9」 */
void station_freq_text(const station_t *st, char *out, size_t size);
/** Where a frequency sits on the dial, 0..1000 (clamped to FM_MIN_10..FM_MAX_10). */
int dial_permille(int mhz10);
/** The sentence Yay says when it is back: 「听了北京交通广播 25 分钟」, under a minute 「听了一会儿北京交通广播」. */
void listen_sentence(const char *name, int seconds, char *out, size_t size);
