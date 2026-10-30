// The player: two tasks. One reads the station's MP3 stream over HTTP into a PSRAM buffer of about 12 seconds;
// the other decodes it (esp_audio_codec), turns it into mono at the speaker's rate (pcm_conv) and plays it through
// yaykit_speak. The buffer rides out network stalls; when it runs dry the player waits until it holds a few seconds
// again instead of stuttering. Calls from any task; they only set what is wanted and the tasks follow.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RADIO_IDLE,        // stopped
    RADIO_WAIT_NET,    // wants to play, no network yet
    RADIO_CONNECTING,  // opening the stream, filling the buffer before the first sound
    RADIO_PLAYING,
    RADIO_BUFFERING,   // the buffer ran dry while playing: filling it again
    RADIO_RETRYING,    // the stream failed; trying again shortly
} radio_state_t;

/** After yk_speak_init_cfg(). Creates the tasks. */
esp_err_t radio_init(void);
/** Plays the station at this index (stations.h); another station stops the current one first. */
void radio_play(int station);
void radio_stop(void);
radio_state_t radio_state(void);
const char *radio_state_name(radio_state_t s);
/** The station wanted (or last played). */
int radio_station(void);

typedef struct {
    uint32_t connects, failures, redirects;
    uint32_t bytes;           // stream bytes read, this connection
    uint32_t buffered;        // stream bytes waiting to be decoded now
    uint32_t rebuffers;       // times the buffer ran dry while playing, this connection
    uint32_t in_rate, in_channels;
    uint32_t decode_errors;
} radio_stats_t;
void radio_stats(radio_stats_t *out);
