#include "yaykit_account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "yaykit_link.h"
#include "yk_codec.h"
#include "yk_http.h"
#include "yk_locale.h"
#include "yk_sign.h"

static const char *TAG = "yk_account";
static const char *NS = "yk_acct";

static yk_account_config_t cfg;
static SemaphoreHandle_t lock;
static yk_account_state_t state = YK_ACCOUNT_UNBOUND;
static const char *error = "";
static char nickname[64];
static char locale[8] = "zh";
static bool have_key;
static uint8_t private_key[YK_KEY_PRIVATE_BYTES], public_key[YK_KEY_PUBLIC_BYTES];
static char token[512];
static int64_t token_expires_us;
static char claim[128];

static int rng(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static void changed(void) {
    if (cfg.on_change) cfg.on_change(cfg.ctx);
}

static void set_state(yk_account_state_t next, const char *why) {
    xSemaphoreTake(lock, portMAX_DELAY);
    state = next;
    error = why ? why : "";
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s%s%s", yk_account_state_name(), *error ? " " : "", error);
    changed();
}

/** Stores the binding and the owner. NVS writes: call from a task whose stack is in internal RAM. */
static void save_binding(bool bound) {
    char nick[sizeof(nickname)], loc[sizeof(locale)];
    xSemaphoreTake(lock, portMAX_DELAY);
    strcpy(nick, nickname);
    strcpy(loc, locale);
    xSemaphoreGive(lock);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bound", bound ? 1 : 0);
    if (bound) {
        nvs_set_str(h, "nick", nick);
        nvs_set_str(h, "loc", loc);
    } else {
        nvs_erase_key(h, "nick");
        nvs_erase_key(h, "loc");
    }
    nvs_commit(h);
    nvs_close(h);
}

/** Loads the key, or makes and stores one on first activation. */
static bool ensure_key(void) {
    if (have_key) return true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    size_t sk_len = sizeof(private_key), pk_len = sizeof(public_key);
    bool ok = nvs_get_blob(h, "sk", private_key, &sk_len) == ESP_OK && sk_len == sizeof(private_key) &&
              nvs_get_blob(h, "pk", public_key, &pk_len) == ESP_OK && pk_len == sizeof(public_key);
    if (!ok) {
        ok = yk_key_generate(private_key, public_key, rng, NULL) == 0 &&
             nvs_set_blob(h, "sk", private_key, sizeof(private_key)) == ESP_OK &&
             nvs_set_blob(h, "pk", public_key, sizeof(public_key)) == ESP_OK && nvs_commit(h) == ESP_OK;
        if (ok) ESP_LOGI(TAG, "made the device key");
    }
    nvs_close(h);
    have_key = ok;
    return ok;
}

// The token is also kept in NVS with its end in wall-clock time, so that after a restart (an app opened and
// left, say) it is used again instead of fetched: two TLS handshakes less before the session can connect. Only
// with a known clock (yk_token_left_s); the device key it was made with is in the same namespace anyway.
typedef struct {
    char app[48];
    int64_t expires_unix;
    char token[sizeof(token)];
} kept_token_t;

static void kept_token_save(int64_t ttl_s) {
    const int64_t now = (int64_t)time(NULL);
    kept_token_t *k = calloc(1, sizeof(*k));
    if (!k) return;
    snprintf(k->app, sizeof(k->app), "%s", cfg.app_slug);
    k->expires_unix = now + ttl_s;
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(k->token, token, sizeof(k->token));
    xSemaphoreGive(lock);
    char key[11];
    yk_token_key(cfg.app_slug, key);
    nvs_handle_t h;
    if (yk_token_left_s(now, k->expires_unix) && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, key, k, sizeof(*k)) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    memset(k, 0, sizeof(*k));
    free(k);
}

static void kept_token_load(void) {
    kept_token_t *k = calloc(1, sizeof(*k));
    if (!k) return;
    char key[11];
    yk_token_key(cfg.app_slug, key);
    nvs_handle_t h;
    size_t len = sizeof(*k);
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, key, k, &len) != ESP_OK || len != sizeof(*k)) k->app[0] = 0;
        nvs_close(h);
    }
    k->app[sizeof(k->app) - 1] = k->token[sizeof(k->token) - 1] = 0;
    const int64_t left = !strcmp(k->app, cfg.app_slug) ? yk_token_left_s((int64_t)time(NULL), k->expires_unix) : 0;
    if (left && k->token[0]) {
        strcpy(token, k->token);
        token_expires_us = esp_timer_get_time() + left * 1000000;
        ESP_LOGI(TAG, "token kept from before the restart: %lld s left", (long long)left);
    }
    memset(k, 0, sizeof(*k));
    free(k);
}

static void kept_token_drop(void) {
    char key[11];
    yk_token_key(cfg.app_slug, key);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, key) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void keep_token(const cJSON *json) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(json, "accessToken");
    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(json, "expiresIn");
    if (!cJSON_IsString(t) || strlen(t->valuestring) >= sizeof(token) || !cJSON_IsNumber(ttl)) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    strcpy(token, t->valuestring);
    token_expires_us = esp_timer_get_time() + (int64_t)ttl->valuedouble * 1000000;
    xSemaphoreGive(lock);
    kept_token_save((int64_t)ttl->valuedouble);
}

/** Keeps owner.nickname and owner.locale. True when either changed. */
static bool keep_owner(const cJSON *owner) {
    const cJSON *nick = cJSON_GetObjectItemCaseSensitive(owner, "nickname");
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(owner, "locale");
    char next_nick[sizeof(nickname)], next_loc[sizeof(locale)];
    snprintf(next_nick, sizeof(next_nick), "%s", cJSON_IsString(nick) ? nick->valuestring : "");
    snprintf(next_loc, sizeof(next_loc), "%s", cJSON_IsString(loc) && *loc->valuestring ? loc->valuestring : "zh");
    xSemaphoreTake(lock, portMAX_DELAY);
    bool same = !strcmp(nickname, next_nick) && !strcmp(locale, next_loc);
    strcpy(nickname, next_nick);
    strcpy(locale, next_loc);
    xSemaphoreGive(lock);
    return !same;
}

/** Signs `message` and returns it as base64url in `out`. */
static bool sign_b64(const char *message, char *out, size_t size) {
    uint8_t sig[YK_SIGNATURE_MAX];
    size_t len = yk_sign(private_key, message, sig, rng, NULL);
    return len && yk_b64url_encode(sig, len, out, size);
}

/** Activates with `claim`. Returns NULL or the failure code. */
static const char *activate_now(const char *claim_text) {
    if (!ensure_key()) return "NETWORK";
    char message[200], signature[128], key[100];
    const char *device_id = yk_link_device_id();
    if (!yk_activation_message(device_id, claim_text, message, sizeof(message)) ||
        !sign_b64(message, signature, sizeof(signature)) ||
        !yk_b64url_encode(public_key, sizeof(public_key), key, sizeof(key)))
        return "NETWORK";
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "deviceId", device_id);
    cJSON_AddStringToObject(body, "publicKey", key);
    cJSON_AddStringToObject(body, "claim", claim_text);
    cJSON_AddStringToObject(body, "signature", signature);
    cJSON_AddStringToObject(body, "app", cfg.app_slug);
    cJSON_AddStringToObject(body, "appVersion", cfg.app_version);
    cJSON_AddStringToObject(body, "board", cfg.board);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    yk_http_response_t res;
    esp_err_t err = json ? yk_http_json(HTTP_METHOD_POST, "/device/v1/activate", NULL, json, &res) : ESP_ERR_NO_MEM;
    cJSON_free(json);
    if (err != ESP_OK) return "NETWORK";
    const char *code = NULL;
    if (res.status == 200) {
        cJSON *reply = cJSON_Parse(res.body ? res.body : "");
        keep_token(reply);
        keep_owner(cJSON_GetObjectItemCaseSensitive(reply, "owner"));
        cJSON_Delete(reply);
        save_binding(true);
    } else {
        code = yk_http_error_code(&res);
        if (strcmp(code, "DEVICE_OWNED_BY_OTHER") && strcmp(code, "SERVER")) code = "CLAIM_INVALID";
        if (!strcmp(code, "SERVER")) code = "NETWORK";
        ESP_LOGW(TAG, "activation refused: HTTP %d %s", res.status, code);
    }
    yk_http_free(&res);
    return code;
}

static void activate_task(void *arg) {
    (void)arg;
    // Let the {"state":"received"} reply reach the phone before Bluetooth goes off.
    vTaskDelay(pdMS_TO_TICKS(1500));
    const char *code = NULL;
    if (yk_http_prepare(&code) == ESP_OK) code = activate_now(claim);
    memset(claim, 0, sizeof(claim));
    set_state(code ? YK_ACCOUNT_FAILED : YK_ACCOUNT_BOUND, code);
    vTaskDelete(NULL);
}

/** Starts activation with a claim. */
static esp_err_t start_activation(const char *claim_text, const char **code) {
    if (!yk_claim_valid(claim_text)) {
        *code = "BAD_CLAIM";
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool busy = state == YK_ACCOUNT_ACTIVATING;
    if (!busy) {
        snprintf(claim, sizeof(claim), "%s", claim_text);
        state = YK_ACCOUNT_ACTIVATING;
        error = "";
        token[0] = 0;
    }
    xSemaphoreGive(lock);
    if (busy) {
        *code = "BUSY";
        return ESP_ERR_INVALID_STATE;
    }
    changed();
    // Internal-RAM stack: TLS, signing and NVS writes happen here.
    if (xTaskCreate(activate_task, "yk_activate", 10240, NULL, 5, NULL) != pdPASS) {
        set_state(YK_ACCOUNT_FAILED, "NETWORK");
        *code = "NO_MEMORY";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void forget(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    nickname[0] = 0;
    strcpy(locale, "zh");
    token[0] = 0;
    xSemaphoreGive(lock);
    kept_token_drop();
    save_binding(false);
    set_state(YK_ACCOUNT_UNBOUND, NULL);
}

esp_err_t yk_account_init(const yk_account_config_t *config) {
    if (!config || !config->app_slug || !config->app_version || !config->board) return ESP_ERR_INVALID_ARG;
    cfg = *config;
    yk_http_set_app(cfg.app_slug, cfg.app_version);
    if (!lock) lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    uint8_t bound = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bound", &bound);
        size_t len = sizeof(nickname);
        if (nvs_get_str(h, "nick", nickname, &len) != ESP_OK) nickname[0] = 0;
        len = sizeof(locale);
        if (nvs_get_str(h, "loc", locale, &len) != ESP_OK) strcpy(locale, "zh");
        size_t sk_len = sizeof(private_key), pk_len = sizeof(public_key);
        have_key = nvs_get_blob(h, "sk", private_key, &sk_len) == ESP_OK && sk_len == sizeof(private_key) &&
                   nvs_get_blob(h, "pk", public_key, &pk_len) == ESP_OK && pk_len == sizeof(public_key);
        nvs_close(h);
    }
    state = bound && have_key ? YK_ACCOUNT_BOUND : YK_ACCOUNT_UNBOUND;
    if (state == YK_ACCOUNT_BOUND) kept_token_load();
    ESP_LOGI(TAG, "%s", yk_account_state_name());
    return ESP_OK;
}

yk_account_state_t yk_account_state(void) { return state; }

const char *yk_account_state_name(void) {
    switch (state) {
        case YK_ACCOUNT_ACTIVATING: return "activating";
        case YK_ACCOUNT_BOUND: return "bound";
        case YK_ACCOUNT_FAILED: return "failed";
        default: return "unbound";
    }
}

const char *yk_account_error(void) { return error; }

void yk_account_nickname(char *out, size_t size) {
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(out, size, "%s", nickname);
    xSemaphoreGive(lock);
}

void yk_account_locale(char *out, size_t size) {
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(out, size, "%s", locale);
    xSemaphoreGive(lock);
}

bool yk_account_english(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool en = yk_locale_english(locale);
    xSemaphoreGive(lock);
    return en;
}

bool yk_account_locale_seen(const char *seen) {
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool took = yk_locale_take(locale, seen);
    const bool bound = state == YK_ACCOUNT_BOUND;
    xSemaphoreGive(lock);
    if (!took) return false;
    nvs_handle_t h;
    if (bound && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {  // with the binding, as save_binding keeps it
        nvs_set_str(h, "loc", seen);
        nvs_commit(h);
        nvs_close(h);
    }
    changed();
    return true;
}

void yk_account_owner_seen(const cJSON *owner) {
    if (!owner || state != YK_ACCOUNT_BOUND || !keep_owner(owner)) return;
    save_binding(true);
    changed();
}

void yk_account_describe(cJSON *account) {
    cJSON_AddStringToObject(account, "state", yk_account_state_name());
    if (*error) cJSON_AddStringToObject(account, "error", error);
    char nick[64];
    yk_account_nickname(nick, sizeof(nick));
    if (state == YK_ACCOUNT_BOUND && *nick) cJSON_AddStringToObject(account, "owner", nick);
    if (state == YK_ACCOUNT_BOUND) {  // the owner's language as kept ("zh-Hans", "en"…; #545)
        char loc[sizeof(locale)];
        yk_account_locale(loc, sizeof(loc));
        cJSON_AddStringToObject(account, "locale", loc);
    }
}

esp_err_t yk_account_command(const char *name, const cJSON *args, cJSON *reply, const char **code) {
    if (!strcmp(name, "account.activate")) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(args, "claim");
        esp_err_t err = start_activation(cJSON_IsString(c) ? c->valuestring : NULL, code);
        if (err == ESP_OK) cJSON_AddStringToObject(reply, "state", "received");
        return err;
    }
    if (!strcmp(name, "account.forget")) forget();
    else if (strcmp(name, "account.status")) return ESP_ERR_NOT_FOUND;
    yk_account_describe(reply);
    return ESP_OK;
}

void yk_account_token_expired(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    token[0] = 0;
    xSemaphoreGive(lock);
    kept_token_drop();
}

void yk_account_unbound_by_server(void) {
    ESP_LOGW(TAG, "the cloud no longer knows this binding");
    forget();
}

esp_err_t yk_account_token(char *out, size_t size, const char **code) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool bound = state == YK_ACCOUNT_BOUND && have_key;
    bool fresh = token[0] && esp_timer_get_time() < token_expires_us - 60LL * 1000000;
    if (bound && fresh && strlen(token) < size) strcpy(out, token);
    xSemaphoreGive(lock);
    if (!bound) {
        *code = "DEVICE_NOT_ACTIVATED";
        return ESP_ERR_INVALID_STATE;
    }
    if (fresh) return ESP_OK;
    if (yk_http_prepare(code) != ESP_OK) return ESP_FAIL;

    const char *device_id = yk_link_device_id();
    char json[400];
    snprintf(json, sizeof(json), "{\"deviceId\":\"%s\"}", device_id);
    yk_http_response_t res;
    if (yk_http_json(HTTP_METHOD_POST, "/device/v1/token/challenge", NULL, json, &res) != ESP_OK) {
        *code = "NETWORK";
        return ESP_FAIL;
    }
    cJSON *reply = res.status == 200 ? cJSON_Parse(res.body ? res.body : "") : NULL;
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(reply, "nonce");
    char nonce_text[96] = "";
    if (cJSON_IsString(nonce)) snprintf(nonce_text, sizeof(nonce_text), "%s", nonce->valuestring);
    cJSON_Delete(reply);
    yk_http_free(&res);
    char message[220], signature[128];
    if (!*nonce_text || !yk_token_message(device_id, cfg.app_slug, nonce_text, message, sizeof(message)) ||
        !sign_b64(message, signature, sizeof(signature))) {
        *code = "SERVER";
        return ESP_FAIL;
    }
    snprintf(json, sizeof(json), "{\"deviceId\":\"%s\",\"app\":\"%s\",\"nonce\":\"%s\",\"signature\":\"%s\"}",
             device_id, cfg.app_slug, nonce_text, signature);
    if (yk_http_json(HTTP_METHOD_POST, "/device/v1/token", NULL, json, &res) != ESP_OK) {
        *code = "NETWORK";
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    if (res.status == 200) {
        reply = cJSON_Parse(res.body ? res.body : "");
        keep_token(reply);
        cJSON_Delete(reply);
        xSemaphoreTake(lock, portMAX_DELAY);
        if (token[0] && strlen(token) < size) strcpy(out, token);
        else err = ESP_FAIL;
        xSemaphoreGive(lock);
        if (err != ESP_OK) *code = "SERVER";
    } else {
        *code = yk_http_error_code(&res);
        if (!strcmp(*code, "DEVICE_NOT_ACTIVATED")) yk_account_unbound_by_server();
        err = ESP_FAIL;
    }
    yk_http_free(&res);
    return err;
}

static struct {
    struct arg_str *action;
    struct arg_str *value;
    struct arg_end *end;
} account_args;

static int account_cmd(int argc, char **argv) {
    if (arg_parse(argc, argv, (void **)&account_args)) {
        arg_print_errors(stdout, account_args.end, argv[0]);
        return 1;
    }
    const char *action = account_args.action->count ? account_args.action->sval[0] : "status";
    const char *code = NULL;
    if (!strcmp(action, "claim") && account_args.value->count) {
        if (start_activation(account_args.value->sval[0], &code) != ESP_OK) {
            printf("YK_ACCOUNT error=%s\n", code);
            return 1;
        }
    } else if (!strcmp(action, "forget")) {
        forget();
    } else if (strcmp(action, "status")) {
        printf("usage: account status | claim <ykc_...> | forget\n");
        return 1;
    }
    char nick[64];
    yk_account_nickname(nick, sizeof(nick));
    printf("YK_ACCOUNT state=%s error=%s owner=%s device=%s origin=%s\n", yk_account_state_name(), error,
           *nick ? nick : "-", yk_link_device_id(), CONFIG_YK_CLOUD_ORIGIN);
    return 0;
}

esp_err_t yk_account_register_console(void) {
    account_args.action = arg_str0(NULL, NULL, "<status|claim|forget>", "action");
    account_args.value = arg_str0(NULL, NULL, "<claim>", "one-time claim for bench activation");
    account_args.end = arg_end(2);
    const esp_console_cmd_t cmd = {
        .command = "account",
        .help = "Yaykit account: status | claim <ykc_...> | forget",
        .func = account_cmd,
        .argtable = &account_args,
    };
    return esp_console_cmd_register(&cmd);
}
