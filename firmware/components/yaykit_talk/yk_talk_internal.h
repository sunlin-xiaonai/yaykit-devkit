// yaykit_talk's own, between its files.
#pragma once

#include <stdbool.h>

/** In the microphones' task, after everything given before it (a stream's audio, its listen.stop): sends the JSON
 *  on the session and frees it. False when it could not be queued (it is freed then too). */
bool yk_talk_mic_post_send(char *json);
/** yk_talk_flush(): the uplink's queue through, at most `timeout_ms`. */
esp_err_t yk_talk_mic_flush(uint32_t timeout_ms);
