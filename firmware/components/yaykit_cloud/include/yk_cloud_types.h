// What the device API v1 (docs/design/yay-device.md §13) gives and takes, as yaykit_cloud's functions use it.
// Pure C (cJSON only), so the host tests can read the same fixtures the server's tests check.
// Rules (§13.3): functions and fields are only added; YK_CLOUD_API_LEVEL goes up whenever something is.
// (YK_API_LEVEL in yaykit_app.h is the level of all public components together; this is the cloud client's own.)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** 1: activation, /me, hanzi lookup. 2: the device API v1 (capabilities, asr, chat, files, data, records, inbox). */
#define YK_CLOUD_API_LEVEL 2

typedef struct {
    int remaining;        // credits left today plus bonus credits
    int daily_limit;
    int daily_remaining;
    char reset_at[32];    // ISO time of the next refill (Beijing midnight)
    char nickname[64];
    // Level 2:
    int api_version;
    char locale[16];
    char timezone[48];
    int64_t storage_used;
    int64_t storage_limit;
    int settings_version;
    bool records_enabled;  // whether POST /records keeps anything (the user agreed in Yay)
    char min_firmware[24]; // "" when none
} yk_cloud_me_t;

/** What a metered call cost (whole points) and what is left. */
typedef struct {
    int charged;
    int remaining;
    int daily_limit;
    int daily_remaining;
    char reset_at[32];
} yk_cloud_charge_t;

typedef enum { YK_CLOUD_WAV = 0, YK_CLOUD_OGG_OPUS = 1 } yk_cloud_format_t;

typedef struct {
    char text[512];
    float seconds;
    char language[8];
    yk_cloud_charge_t credits;
} yk_cloud_asr_t;

typedef struct {
    const char *role;     // "system", "user" or "assistant"
    const char *content;
} yk_cloud_message_t;

typedef struct {
    const char *tier;     // "fast" (NULL) or "smart", one of the app's llm.tiers
    const yk_cloud_message_t *messages;
    size_t count;
    int max_tokens;       // 0: the server's default; at most the app's llm.maxTokens
    bool json;            // ask for a JSON object back (when the app's llm.json allows it)
    // Instead of messages: one of the app's llm.templates ("homework-buddy/hanzi.v1") and its variables.
    const char *template_name;
    const char *const *var_names;
    const char *const *var_values;
    size_t var_count;
} yk_cloud_chat_t;

/** One piece of a streamed answer, as it arrives. */
typedef void (*yk_cloud_delta_fn)(const char *text, void *ctx);

typedef struct {
    int input_tokens;
    int output_tokens;
    yk_cloud_charge_t credits;
} yk_cloud_chat_done_t;

/** How to speak (POST /tts); NULL or 0 for the app's first voice and sample rate, and Opus. The voice reads the
 *  text as written (there is no language to pick). */
typedef struct {
    const char *voice;
    const char *format;  // "opus" (NULL) or "pcm"
    int sample_rate;     // 16000 or 24000 (0: the app's first)
} yk_cloud_tts_t;

/** One piece of speech as it arrives: an Opus packet (60 ms), or 60 ms of 16-bit mono PCM. */
typedef void (*yk_cloud_audio_fn)(int seq, const uint8_t *data, size_t len, void *ctx);

/** A file to register (POST /files). Strings may be NULL and numbers negative when not sent. */
typedef struct {
    const char *id;            // the device's own name, e.g. 20261018-091530-a1b2
    const char *kind;          // "recording", "photo" or "log"
    const char *content_type;  // "audio/wav", "audio/ogg"…
    int64_t size;
    const char *sha256;        // 64 lowercase hex characters
    int64_t duration_ms;       // -1: not sent
    int64_t speech_ms;         // -1: not sent
    const char *recorded_at;   // ISO time, or NULL
    bool clock_trusted;
    const char *retention;     // NULL: the app's first
    const char *recording_id;  // NULL: not part of a longer recording
    int segment;
    bool transcribe;
    const char *language;      // NULL: the app's first
} yk_cloud_file_info_t;

#define YK_CLOUD_PARTS 16

/** Where to PUT one part: exactly `bytes` bytes from `offset`, straight to storage. `url` is allocated. */
typedef struct {
    int number;
    int64_t offset;
    int64_t bytes;
    char *url;
} yk_cloud_part_t;

typedef struct {
    char id[81];
    char state[16];            // uploading, stored, deleted, expired, abandoned
    int64_t size;
    int64_t received;
    char transcript[16];       // "" when none asked; queued, running, done, failed, waiting_quota
    char transcript_error[64];
    int charged;               // points the transcription cost, once done
    // While uploading: the next parts to send (call register again for more, or when expires_at passes).
    bool has_upload;
    int part_bytes;
    int part_count;
    int received_parts;
    yk_cloud_part_t parts[YK_CLOUD_PARTS];
    int nparts;
    char expires_at[32];
    // Level 2, when the answer carries them (registering for a transcription, completing).
    bool has_credits;
    yk_cloud_charge_t credits;
} yk_cloud_file_t;

/** A delivery to ask for (POST /deliver): a stored recording's transcript (file_id) or a text, into Notion or Gmail. */
typedef struct {
    const char *id;             // the device's own name for the request; asking again is the same delivery
    const char *target;         // "notion" or "gmail", one of the app's deliver.targets
    const char *file_id;        // a stored recording, or NULL with `text`
    const char *text;
    const char *title;          // for a text (NULL: a default)
    const char *notion_parent;  // Notion: the page to put the new page under
} yk_cloud_delivery_info_t;

/** Where a delivery stands: pending_approval (去手机上确认), sending, delivered, failed, declined, expired. */
typedef struct {
    char id[81];
    char target[8];
    char state[20];
    char error[48];        // when failed: CONNECTION_INACTIVE (connect again in Yay)…
    int tries;
    char external_url[512];  // Notion and Gmail links run long
} yk_cloud_delivery_t;

/** One item of app data. `value` is owned by the item (yk_cloud_data_free). */
typedef struct {
    char key[129];
    int version;
    char updated_at[32];
    cJSON *value;
} yk_cloud_data_item_t;

/** One inbox item: its id, kind (skip kinds you do not know) and data. */
typedef void (*yk_cloud_inbox_fn)(const char *id, const char *kind, const cJSON *data, void *ctx);

/** Frees the part URLs of a file answer. */
void yk_cloud_file_free(yk_cloud_file_t *file);
/** Frees an item's value. */
void yk_cloud_data_free(yk_cloud_data_item_t *item);
