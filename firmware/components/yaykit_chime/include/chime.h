/* Short synthesized chimes played through the on-board ES8311 + speaker. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    CHIME_FOCUS_DONE, /* bright rising arpeggio: "well done, take a break" */
    CHIME_BREAK_DONE, /* soft falling pair:      "back to it"             */
    CHIME_PREVIEW,    /* single note, confirms sound is on                */
} chime_kind_t;

/* Brings up I2S + codec. Failure is non-fatal: the timer works silently. */
esp_err_t chime_init(void);

/* Queues a chime and returns immediately. Dropped if one is already queued. */
void chime_play(chime_kind_t kind);

/* While muted (recording) chimes are dropped: the speaker would be in the recording. */
void chime_set_muted(bool muted);
/* The owner's volume, 0..100 (0 = no sound). */
void chime_set_volume(int percent);

/* Which GPIO(s) enable the speaker amplifier (-1 = unused). */
void chime_set_pa_pins(int a, int b);

/* Diagnostic: change the priority of the chime task at run time (A/B tests). */
void chime_set_priority(int priority);

/* Current amplifier pins (-1 = unused). */
void chime_get_pa_pins(int *a, int *b);

/* Steady sine tone, blocking. Used for measuring, e.g. microphone loopback. */
void chime_play_tone(int freq_hz, int ms, int volume);

/* Diagnostic: log the ES8311 register file (label shown in the log line). */
void chime_dump_regs(const char *when);

/* Diagnostic: four labelled beeps, each with a different amplifier pin. */
void chime_soundtest(void);

/* Returns true once chime_init() succeeded. */
bool chime_available(void);
