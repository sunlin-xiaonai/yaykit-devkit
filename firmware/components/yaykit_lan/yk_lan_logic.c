// Yaykit LAN's pure parts (yk_lan_logic.h).
#include "yk_lan_logic.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void yk_lan_token_text(const uint8_t random[16], char out[33]) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = HEX[random[i] >> 4];
        out[2 * i + 1] = HEX[random[i] & 15];
    }
    out[32] = 0;
}

bool yk_lan_token_equal(const char *want, const char *got) {
    if (!want || !got || !want[0]) return false;
    const size_t n = strlen(want);
    if (strlen(got) != n) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned)(unsigned char)(want[i] ^ got[i]);
    return diff == 0;
}

yk_lan_hello_t yk_lan_hello(const char *text, const char *token, char *name, size_t size) {
    if (name && size) name[0] = 0;
    cJSON *doc = cJSON_Parse(text);
    if (!doc) return YK_LAN_NOT_JSON;
    yk_lan_hello_t out = YK_LAN_NOT_HELLO;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(doc, "t");
    if (cJSON_IsObject(doc) && cJSON_IsString(t) && !strcmp(t->valuestring, "hello")) {
        const cJSON *tok = cJSON_GetObjectItemCaseSensitive(doc, "token");
        out = cJSON_IsString(tok) && yk_lan_token_equal(token, tok->valuestring) ? YK_LAN_HELLO_OK : YK_LAN_HELLO_BAD;
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(doc, "name");
        if (out == YK_LAN_HELLO_OK && name && size && cJSON_IsString(n)) {
            snprintf(name, size, "%s", n->valuestring);
            for (char *p = name; *p; p++)
                if ((unsigned char)*p < 0x20) *p = ' ';
            // A UTF-8 character cut by the buffer: drop what is left of it.
            size_t len = strlen(name), k = len;
            while (k > 0 && ((unsigned char)name[k - 1] & 0xC0) == 0x80) k--;
            if (k > 0 && ((unsigned char)name[k - 1] & 0xC0) == 0xC0) {
                const unsigned char lead = (unsigned char)name[k - 1];
                const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
                if (len - (k - 1) < need) name[k - 1] = 0;
            }
        }
    }
    cJSON_Delete(doc);
    return out;
}
