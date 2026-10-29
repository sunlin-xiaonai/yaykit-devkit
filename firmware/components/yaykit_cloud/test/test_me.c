// Host test: yk_cloud_me() keeps the owner it reads, yk_cloud_me_ex(…, false) leaves it as it is (Yay's daily check
// reads /me for its time zone; its language comes only with hello.ok). yaykit_cloud.c built against test/stub, with
// the HTTPS helpers and the account answered here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yaykit_account.h"
#include "yaykit_cloud.h"
#include "yk_http.h"

static int checks, failures;
#define CHECK(c)                                                    \
    do {                                                            \
        checks++;                                                   \
        if (!(c)) {                                                 \
            failures++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        }                                                           \
    } while (0)

static const char *ME =
    "{\"credits\":{\"remaining\":120,\"dailyLimit\":200,\"dailyRemaining\":80,\"resetAt\":\"2026-10-27T00:00:00+08:00\"},"
    "\"owner\":{\"nickname\":\"小鱼\",\"locale\":\"zh\",\"timezone\":\"Asia/Shanghai\"},\"apiVersion\":1}";

// What the cloud answers, in turn: an HTTP status and body; status 0 is no answer (the network).
static struct {
    int status;
    const char *body;
} s_replies[4];
static int s_reply_count, s_replies_used;
static char s_path[64], s_bearer[32];
static int s_owner_seen, s_token_expired, s_unbound;
static char s_owner_locale[16];

static void reset(void) {
    memset(s_replies, 0, sizeof(s_replies));
    s_reply_count = s_replies_used = 0;
    s_path[0] = s_bearer[0] = s_owner_locale[0] = 0;
    s_owner_seen = s_token_expired = s_unbound = 0;
}

static void reply(int status, const char *body) {
    s_replies[s_reply_count].status = status;
    s_replies[s_reply_count].body = body;
    s_reply_count++;
}

// ---- yk_http (yk_http.c is ESP-IDF's client)

esp_err_t yk_http_json(esp_http_client_method_t method, const char *path, const char *bearer, const char *json,
                       yk_http_response_t *out) {
    (void)method, (void)json;
    memset(out, 0, sizeof(*out));
    snprintf(s_path, sizeof(s_path), "%s", path);
    snprintf(s_bearer, sizeof(s_bearer), "%s", bearer ? bearer : "");
    if (s_replies_used >= s_reply_count || !s_replies[s_replies_used].status) {
        s_replies_used++;
        return ESP_FAIL;
    }
    out->status = s_replies[s_replies_used].status;
    out->body = strdup(s_replies[s_replies_used].body);
    out->len = strlen(out->body);
    s_replies_used++;
    return ESP_OK;
}
void yk_http_free(yk_http_response_t *response) {
    free(response->body);
    response->body = NULL;
}
const char *yk_http_error_code(const yk_http_response_t *response) {
    if (response->body && strstr(response->body, "TOKEN_EXPIRED")) return "TOKEN_EXPIRED";
    if (response->body && strstr(response->body, "DEVICE_NOT_ACTIVATED")) return "DEVICE_NOT_ACTIVATED";
    return "SERVER";
}
esp_err_t yk_http_prepare(const char **code) {
    *code = "NO_WIFI";
    return ESP_FAIL;
}
esp_http_client_handle_t yk_http_client(const char *path, esp_http_client_method_t method, const char *bearer,
                                        const char *content_type, int timeout_ms) {
    (void)path, (void)method, (void)bearer, (void)content_type, (void)timeout_ms;
    return NULL;
}
esp_err_t yk_http_send(esp_http_client_method_t method, const char *path, const char *bearer, const char *content_type,
                       const uint8_t *body, size_t body_len, int timeout_ms, yk_http_response_t *out) {
    (void)method, (void)path, (void)bearer, (void)content_type, (void)body, (void)body_len, (void)timeout_ms;
    memset(out, 0, sizeof(*out));
    return ESP_FAIL;
}
void yk_http_read_body(esp_http_client_handle_t client, yk_http_response_t *out) {
    (void)client;
    memset(out, 0, sizeof(*out));
}

// ---- yaykit_account

esp_err_t yk_account_token(char *out, size_t size, const char **code) {
    (void)code;
    snprintf(out, size, "token-%d", s_token_expired);
    return ESP_OK;
}
void yk_account_token_expired(void) { s_token_expired++; }
void yk_account_unbound_by_server(void) { s_unbound++; }
void yk_account_owner_seen(const cJSON *owner) {
    s_owner_seen++;
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(owner, "locale");
    snprintf(s_owner_locale, sizeof(s_owner_locale), "%s", cJSON_IsString(loc) ? loc->valuestring : "");
}

// ---- ESP-IDF, not reached on the /me path

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    (void)config;
    return NULL;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) { (void)c, (void)n; return ESP_FAIL; }
int esp_http_client_write(esp_http_client_handle_t c, const char *b, int n) { (void)c, (void)b, (void)n; return -1; }
int esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return -1; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return 0; }
int esp_http_client_read(esp_http_client_handle_t c, char *b, int n) { (void)c, (void)b, (void)n; return -1; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) { (void)c, (void)ms; return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
int64_t esp_timer_get_time(void) { return 0; }

int main(void) {
    yk_cloud_me_t me;
    const char *code = NULL;

    // An app's /me: the owner kept, as ever
    reset();
    reply(200, ME);
    CHECK(yk_cloud_me(&me, &code) == ESP_OK);
    CHECK(!strcmp(s_path, "/device/v1/me") && !strcmp(s_bearer, "token-0"));
    CHECK(s_owner_seen == 1 && !strcmp(s_owner_locale, "zh"));
    CHECK(!strcmp(me.timezone, "Asia/Shanghai") && me.remaining == 120);

    // Yay's daily check: the time zone read, the kept owner (its language) left alone
    reset();
    reply(200, ME);
    CHECK(yk_cloud_me_ex(&me, false, &code) == ESP_OK);
    CHECK(s_owner_seen == 0);
    CHECK(!strcmp(me.timezone, "Asia/Shanghai") && !strcmp(me.locale, "zh"));
    CHECK(yk_cloud_me_ex(&me, true, &code) == ESP_FAIL);  // nothing more to answer: NETWORK
    CHECK(s_owner_seen == 0);

    // ... also after a token's refresh
    reset();
    reply(401, "{\"error\":\"TOKEN_EXPIRED\"}");
    reply(200, ME);
    CHECK(yk_cloud_me_ex(&me, false, &code) == ESP_OK);
    CHECK(s_token_expired == 1 && s_replies_used == 2 && !strcmp(s_bearer, "token-1"));
    CHECK(s_owner_seen == 0 && !strcmp(me.timezone, "Asia/Shanghai"));

    // No answer, or refused: nothing kept either way
    reset();
    CHECK(yk_cloud_me_ex(&me, false, &code) == ESP_FAIL && !strcmp(code, "NETWORK"));
    CHECK(!me.timezone[0] && s_owner_seen == 0);
    reset();
    reply(401, "{\"error\":\"DEVICE_NOT_ACTIVATED\"}");
    CHECK(yk_cloud_me(&me, &code) == ESP_FAIL && !strcmp(code, "DEVICE_NOT_ACTIVATED"));
    CHECK(s_unbound == 1 && s_owner_seen == 0);

    printf("yaykit_cloud me host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
