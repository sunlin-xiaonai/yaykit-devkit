#include "yk_codec.h"

#include <stdio.h>
#include <string.h>

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t yk_b64url_encode(const uint8_t *in, size_t len, char *out, size_t out_size) {
    size_t need = len / 3 * 4 + (len % 3 ? len % 3 + 1 : 0);
    if (!out || out_size < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = ALPHABET[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = ALPHABET[v & 63];
    }
    out[o] = 0;
    return o;
}

/** snprintf's result as a length, or 0 when the text did not fit. */
static size_t fitted(size_t size, int n) { return n > 0 && (size_t)n < size ? (size_t)n : 0; }

size_t yk_activation_message(const char *device_id, const char *claim, char *out, size_t size) {
    return fitted(size, snprintf(out, size, "yaykit-activate:v1:%s:%s", device_id, claim));
}

size_t yk_token_message(const char *device_id, const char *app, const char *nonce, char *out, size_t size) {
    return fitted(size, snprintf(out, size, "yaykit-token:v1:%s:%s:%s", device_id, app, nonce));
}

void yk_token_key(const char *app, char out[11]) {
    uint32_t h = 2166136261u;
    for (const char *p = app ? app : ""; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    static const char hex[] = "0123456789abcdef";
    out[0] = 't', out[1] = 'k';
    for (int i = 0; i < 8; i++) out[2 + i] = hex[(h >> (28 - 4 * i)) & 0xf];
    out[10] = 0;
}

int64_t yk_token_left_s(int64_t now_unix, int64_t expires_unix) {
    if (now_unix < 1735689600) return 0;  // 2025-01-01: before that the clock was never set
    const int64_t left = expires_unix - now_unix;
    return left > 120 && left <= 24 * 3600 ? left : 0;
}

bool yk_claim_valid(const char *claim) {
    if (!claim || strncmp(claim, "ykc_", 4)) return false;
    size_t len = strlen(claim);
    if (len < 20 || len > 100) return false;
    for (size_t i = 4; i < len; i++) {
        char c = claim[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}
