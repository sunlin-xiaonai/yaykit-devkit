// Device API client for first-party firmware (services/device). HTTPS with the certificate bundle; every call
// turns Bluetooth off first and waits for Wi-Fi. Error codes are stable strings for the UI:
//   NO_WIFI, NETWORK, DEVICE_NOT_ACTIVATED, CREDITS_EXHAUSTED, RATE_LIMITED, VOICE_BUSY, SERVICE_BUSY,
//   CONTENT_BLOCKED, OPERATION_NOT_ALLOWED, AUDIO, SERVER.
// Level 2 (YK_CLOUD_API_LEVEL, the device API v1 of docs/design/yay-device.md §13.4) adds a function per capability and
// gives their refusals as the API names them (yk_api_code: FILE_GONE, VERSION_CONFLICT…). Check a capability with
// yk_cloud_has() after yk_cloud_me() before calling it, and read limits with yk_cloud_limit() instead of fixing them.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "yk_cloud_types.h"

/** One server-sent event of a streaming call: "heard", "answer", "done" or "error", with its parsed JSON. */
typedef void (*yk_cloud_event_fn)(const char *event, const cJSON *data, void *ctx);

/** CONFIG_YK_CLOUD_ORIGIN, e.g. https://device.yaykit.io */
const char *yk_cloud_origin(void);

/** GET /device/v1/me: credits and owner. The owner's nickname and locale are kept (yaykit_account), as at activation. */
esp_err_t yk_cloud_me(yk_cloud_me_t *out, const char **code);
/** yk_cloud_me(), but with `keep_owner` false the kept owner (nickname, locale) is left as it is: for Yay, whose language
 *  comes only with hello.ok (it changes the screen and the lists with it), reading /me for its time zone. */
esp_err_t yk_cloud_me_ex(yk_cloud_me_t *out, bool keep_owner, const char **code);

/** POST /device/v1/hanzi/lookup with a 16-bit mono WAV (at most 8 s). Calls `on_event` for each event as it
 *  arrives; ESP_OK means the stream completed (an "error" event may still have reported a failure). */
esp_err_t yk_cloud_hanzi_lookup(const uint8_t *wav, size_t len, yk_cloud_event_fn on_event, void *ctx,
                                const char **code);

// ---- Level 2: the device API v1 ----

/** Whether this app was granted `capability` ("asr", "files"…), as the last yk_cloud_me() saw it. */
bool yk_cloud_has(const char *capability);
/** A number among a capability's limits ("asr", "maxSeconds"), or `fallback` (also before yk_cloud_me()). */
int yk_cloud_limit(const char *capability, const char *key, int fallback);
/** Whether a capability's list ("asr", "languages") holds `value`. */
bool yk_cloud_allows(const char *capability, const char *list, const char *value);
/** This app's setting `key` as text (strings as they are, other values as JSON), from the last yk_cloud_me(). */
bool yk_cloud_setting(const char *key, char *out, size_t size);

/** POST /device/v1/asr: WAV or Ogg Opus within the app's asr limits; `language` NULL for the app's first. */
esp_err_t yk_cloud_asr(const uint8_t *audio, size_t len, yk_cloud_format_t format, const char *language,
                       yk_cloud_asr_t *out, const char **code);
/** POST /device/v1/llm/chat: `on_delta` for each piece of the answer, then `done` (may be NULL). ESP_FAIL with the
 *  error event's code, or NETWORK when the stream stops before its done event. */
esp_err_t yk_cloud_chat(const yk_cloud_chat_t *request, yk_cloud_delta_fn on_delta, void *ctx, yk_cloud_chat_done_t *done,
                        const char **code);
/** POST /device/v1/tts: the text in the app's voice, `on_audio` for each piece as it arrives; `credits` may be NULL.
 *  ESP_FAIL with the error event's code, or NETWORK when the stream stops before its done event (some speech may
 *  have played). Playing it needs yaykit_speak's Opus input (F0, not in this component): until then, ask for
 *  "pcm" (16-bit mono at the sample rate asked). */
esp_err_t yk_cloud_tts(const char *text, const yk_cloud_tts_t *options, yk_cloud_audio_fn on_audio, void *ctx,
                       yk_cloud_charge_t *credits, const char **code);
/** Any scene endpoint (/device/v1/<path>) that answers with events: the body as `content_type`. */
esp_err_t yk_cloud_scene(const char *path, const char *content_type, const uint8_t *body, size_t len,
                         yk_cloud_event_fn on_event, void *ctx, const char **code);

/** POST /device/v1/files: registers (or resumes) a file; `out` holds the next part URLs (yk_cloud_file_free). */
esp_err_t yk_cloud_file_register(const yk_cloud_file_info_t *info, yk_cloud_file_t *out, const char **code);
/** Reads `bytes` bytes of the file from `offset` into `buf` (at most `len`); returns the count, or -1. */
typedef int (*yk_cloud_read_fn)(void *ctx, int64_t offset, uint8_t *buf, size_t len);
/** PUTs one part straight to storage from `read`. UPLOAD_REFUSED: the URL expired or the length was wrong. */
esp_err_t yk_cloud_file_upload(const yk_cloud_part_t *part, yk_cloud_read_fn read, void *ctx, const char **code);
/** POST /device/v1/files/:id/complete. FILE_INCOMPLETE: register again for the missing parts. */
esp_err_t yk_cloud_file_complete(const char *id, yk_cloud_file_t *out, const char **code);
/** GET /device/v1/files/:id: bytes received, state, transcription. */
esp_err_t yk_cloud_file_status(const char *id, yk_cloud_file_t *out, const char **code);
/** POST /device/v1/files/:id/transcribe, in `language` (NULL: the one asked last). */
esp_err_t yk_cloud_file_transcribe(const char *id, const char *language, yk_cloud_file_t *out, const char **code);
/** DELETE /device/v1/files/:id: the audio and its text go; the device should not send it again. */
esp_err_t yk_cloud_file_delete(const char *id, const char **code);

/** GET /device/v1/data/:collection/:key. DATA_NOT_FOUND when there is none. */
esp_err_t yk_cloud_data_get(const char *collection, const char *key, yk_cloud_data_item_t *out, const char **code);
/** PUT: `version` 0 creates only, n replaces version n, -1 overwrites. VERSION_CONFLICT: *current gets the version. */
esp_err_t yk_cloud_data_put(const char *collection, const char *key, const cJSON *value, int version,
                            yk_cloud_data_item_t *out, int *current, const char **code);
/** DELETE, of `version` only when it is not -1. */
esp_err_t yk_cloud_data_del(const char *collection, const char *key, int version, const char **code);
typedef void (*yk_cloud_data_fn)(const yk_cloud_data_item_t *item, void *ctx);
/** One page of a collection from `cursor` (NULL: the start); `next` gets the next page's cursor ("" at the end). */
esp_err_t yk_cloud_data_list(const char *collection, const char *cursor, yk_cloud_data_fn on_item, void *ctx, char *next,
                             size_t next_size, const char **code);

/** POST /device/v1/deliver: waits for the user's approval in Yay (state pending_approval). TARGET_NOT_CONNECTED: the
 *  user connects the service in Yay first. */
esp_err_t yk_cloud_deliver(const yk_cloud_delivery_info_t *info, yk_cloud_delivery_t *out, const char **code);
/** GET /device/v1/deliveries/:id. */
esp_err_t yk_cloud_delivery_status(const char *id, yk_cloud_delivery_t *out, const char **code);
/** POST /device/v1/deliveries/:id/retry, after it failed, was declined or expired. */
esp_err_t yk_cloud_delivery_retry(const char *id, yk_cloud_delivery_t *out, const char **code);

/** POST /device/v1/records with a JSON array of records; RECORDS_DISABLED until the user agreed. */
esp_err_t yk_cloud_records_push(const cJSON *records, int *accepted, const char **code);
/** GET /device/v1/inbox?since=: `on_item` per item; keep `now` for the next call (`since` NULL the first time). */
esp_err_t yk_cloud_inbox(const char *since, yk_cloud_inbox_fn on_item, void *ctx, char *now, size_t now_size,
                         const char **code);
/** POST /device/v1/status with a report ({firmware:{slug,version}, battery, wifi, storage, slots}). */
esp_err_t yk_cloud_status(const cJSON *report, const char **code);
/** GET /device/v1/apps?layout=&lang= (the main firmware's app ring, P23): the store's stable, signed releases for this
 *  board, as the API's body ({"items":[{slug,name,icon,version,size,sha256,url,signature,minYay,...}]}) in `*json`
 *  (PSRAM; free() it). `layout` NULL: yaykit-v1; `lang` the owner's language (yk_account_locale), asked for as "zh" or
 *  "en" (yk_locale_catalog: "zh…" Chinese, any other English). */
esp_err_t yk_cloud_apps(const char *layout, const char *lang, char **json, const char **code);
/** GET /device/v1/updates?slug=&version=&layout= (docs/delivery/2026-10-26-yay-update-e.md 2.1): whether a newer
 *  release of `slug` than `version` is there for this device, the body ({"update": null | {version, size, sha256,
 *  url, signature, minYay}}) into *json (PSRAM; free() it). `layout` NULL: yaykit-v1. Slug, version and layout are
 *  sent as they are: letters, digits, '.', '-', '_' only (else BAD_REQUEST). One request against the device API's 20
 *  a minute. */
esp_err_t yk_cloud_updates(const char *slug, const char *version, const char *layout, char **json, const char **code);
/** GET a small public file (an app's round-screen icon, /device/v1/apps iconFile.url) from the asset host: https only,
 *  no token, not the device API (not in its 20 requests a minute). At most `size` bytes into `buf`, their count in
 *  *len. Codes: BAD_URL (not https), NO_WIFI, NETWORK, SERVER (not 200), TOO_BIG (more than `size`). The caller checks
 *  what it got (its sha256). */
esp_err_t yk_cloud_get(const char *url, uint8_t *buf, size_t size, size_t *len, int timeout_ms, const char **code);
