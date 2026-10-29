// Yaykit account on the device: one binding per device, shared by every app on it.
//
// A signed-in client (iOS app over BLE, installer over USB) asks the cloud for a one-time claim and sends
// `account.activate {"claim":"ykc_..."}` through Yaykit Link. The device answers {"state":"received"}, turns
// Bluetooth off (HTTPS and BLE do not fit in internal RAM together), joins Wi-Fi and activates with its own
// P-256 key. Clients follow the result in the yk-info "account" object (USB) or on /v1/me/devices (phone).
//
// The key and the binding live in the shared NVS namespace `yk_acct`, so switching apps keeps the account;
// a full USB install clears it. Access tokens are per app, 15 minutes, kept in RAM only. See
// docs/architecture/cloud-services.md.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

typedef enum {
    YK_ACCOUNT_UNBOUND,
    YK_ACCOUNT_ACTIVATING,
    YK_ACCOUNT_BOUND,
    YK_ACCOUNT_FAILED,  // see yk_account_error(); a new claim can be sent at any time
} yk_account_state_t;

typedef struct {
    const char *app_slug;  // must be enabled in packages/cloud/src/apps.ts
    const char *app_version;
    const char *board;
    /** State or owner changed. Called from a background task: keep it short, do not block. */
    void (*on_change)(void *ctx);
    void *ctx;
} yk_account_config_t;

/** Loads the binding from NVS. Call after nvs_flash_init() and yk_link_init(). */
esp_err_t yk_account_init(const yk_account_config_t *config);

yk_account_state_t yk_account_state(void);
const char *yk_account_state_name(void);
/** After YK_ACCOUNT_FAILED: DEVICE_OWNED_BY_OTHER, CLAIM_INVALID, NO_WIFI or NETWORK. "" otherwise. */
const char *yk_account_error(void);
/** The owner's nickname as the cloud last reported it; "" when unknown. Never the email. */
void yk_account_nickname(char *out, size_t size);
/** The owner's language as the platform said it ("zh-Hans", "en", "ja", "es-419"; "zh" from before), kept with the
 *  binding; "zh" when unbound. Read it with yk_locale.h (by its prefix), never by comparing whole strings. */
void yk_account_locale(char *out, size_t size);
/** Screens in English: the owner's language is not Chinese (yk_locale_english). */
bool yk_account_english(void);
/** The session's hello.ok said the owner's language (owner.locale, #545): kept in NVS when it is one to keep
 *  (yk_locale_ok) and not the one kept, and on_change called. True when it changed. Writes NVS: call from a task
 *  whose stack is in internal RAM. */
bool yk_account_locale_seen(const char *locale);
/** The cloud reported the owner (an `owner` object from /device/v1/me): keeps a renamed nickname or a changed
 *  language and calls on_change. Called by yaykit_cloud; writes NVS, so call from an internal-RAM stack. */
void yk_account_owner_seen(const cJSON *owner);

/** Handles account.activate, account.status and account.forget. ESP_ERR_NOT_FOUND for other names. */
esp_err_t yk_account_command(const char *name, const cJSON *args, cJSON *reply, const char **code);
/** Fills the yk-info "account" object: state, error, owner, locale (the owner's language as kept, when bound). Pass
 *  to yk_link_config_t.describe_account. */
void yk_account_describe(cJSON *account);

/** A valid access token for this app, fetched with a signed challenge when the cached one is about to expire.
 *  Blocking network call: run it from a task with at least 8 KB of internal stack. */
esp_err_t yk_account_token(char *out, size_t size, const char **code);
/** The API rejected the token (TOKEN_EXPIRED): drop it so the next call fetches a new one. */
void yk_account_token_expired(void);
/** The API says the device is no longer bound (unbound from the account page): forget the binding. */
void yk_account_unbound_by_server(void);

/** Console: account status | claim <ykc_...> | forget. `claim` is for bench work without a phone. */
esp_err_t yk_account_register_console(void);
