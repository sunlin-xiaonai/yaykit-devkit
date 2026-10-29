#include "yaykit_sig.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

// The same rules as packages/cloud/src/firmware-signature.ts, in the same order: the signature's spelling, then the
// image's fields, then the key, then the curve.

#define SIGNATURE_LEN 108  // "yks1." 16 "." 86
#define RAW_LEN 64

static bool field(const char *s) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (n >= 80 || (unsigned char)s[n] < 0x21 || (unsigned char)s[n] > 0x7e) return false;
    return n > 0;
}

static bool hex64(const char *s) {
    if (!s) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return s[64] == '\0';
}

static bool signable(const yk_sig_image_t *image) {
    // A positive safe integer, as in the store (Number.isSafeInteger).
    return image && field(image->slug) && field(image->version) && field(image->board) && field(image->layout) &&
           hex64(image->sha256) && image->size > 0 && image->size <= 9007199254740991ULL;
}

size_t yk_sig_message(const yk_sig_image_t *image, char *out, size_t size) {
    if (!signable(image) || !out) return 0;
    int n = snprintf(out, size,
                     "yaykit-slot-image/1\nslug=%s\nversion=%s\nsize=%llu\nsha256=%s\nboard=%s\nlayout=%s\n",
                     image->slug, image->version, (unsigned long long)image->size, image->sha256, image->board,
                     image->layout);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

bool yk_sig_key_id(const uint8_t point[YK_SIG_KEY_BYTES], char id[17]) {
    if (!point || point[0] != 0x04) return false;
    unsigned char hash[32];
    if (mbedtls_sha256(point, YK_SIG_KEY_BYTES, hash, 0)) return false;
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        id[2 * i] = digits[hash[i] >> 4];
        id[2 * i + 1] = digits[hash[i] & 15];
    }
    id[16] = '\0';
    return true;
}

static int b64url(char c) {
    return c >= 'A' && c <= 'Z'   ? c - 'A'
           : c >= 'a' && c <= 'z' ? c - 'a' + 26
           : c >= '0' && c <= '9' ? c - '0' + 52
           : c == '-'             ? 62
           : c == '_'             ? 63
                                  : -1;
}

/** 86 base64url characters into 64 bytes; false unless it is their one spelling (the last 4 bits unused, so 0). */
static bool decode(const char *in, uint8_t out[RAW_LEN]) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (int i = 0; i < 86; i++) {
        int v = b64url(in[i]);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < RAW_LEN) out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n == RAW_LEN && bits == 4 && (acc & 0x0f) == 0;
}

static bool parse(const char *signature, char id[17], uint8_t raw[RAW_LEN]) {
    if (!signature || strncmp(signature, "yks1.", 5) != 0) return false;
    // strnlen without relying on POSIX: a longer string is malformed too.
    size_t len = 0;
    while (len <= SIGNATURE_LEN && signature[len]) len++;
    if (len != SIGNATURE_LEN || signature[21] != '.') return false;
    for (int i = 0; i < 16; i++) {
        char c = signature[5 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        id[i] = c;
    }
    id[16] = '\0';
    return decode(signature + 22, raw);
}

static bool curve_verify(const uint8_t point[YK_SIG_KEY_BYTES], const unsigned char hash[32], const uint8_t raw[RAW_LEN]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int e = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!e) e = mbedtls_ecp_point_read_binary(&grp, &q, point, YK_SIG_KEY_BYTES);
    if (!e) e = mbedtls_ecp_check_pubkey(&grp, &q);
    if (!e) e = mbedtls_mpi_read_binary(&r, raw, 32);
    if (!e) e = mbedtls_mpi_read_binary(&s, raw + 32, 32);
    if (!e) e = mbedtls_ecdsa_verify(&grp, hash, 32, &q, &r, &s);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return e == 0;
}

yk_sig_result_t yk_sig_verify(const yk_sig_image_t *image, const char *signature, const uint8_t (*keys)[YK_SIG_KEY_BYTES],
                              size_t count) {
    char id[17];
    uint8_t raw[RAW_LEN];
    if (!parse(signature, id, raw)) return YK_SIG_MALFORMED;
    char message[YK_SIG_MESSAGE_MAX];
    size_t len = yk_sig_message(image, message, sizeof(message));
    if (!len) return YK_SIG_BAD_FIELD;
    const uint8_t *key = NULL;
    for (size_t i = 0; keys && i < count && !key; i++) {
        char other[17];
        if (yk_sig_key_id(keys[i], other) && !strcmp(other, id)) key = keys[i];
    }
    if (!key) return YK_SIG_UNKNOWN_KEY;
    unsigned char hash[32];
    if (mbedtls_sha256((const unsigned char *)message, len, hash, 0)) return YK_SIG_MISMATCH;
    return curve_verify(key, hash, raw) ? YK_SIG_OK : YK_SIG_MISMATCH;
}

const char *yk_sig_result_name(yk_sig_result_t result) {
    switch (result) {
    case YK_SIG_OK: return "ok";
    case YK_SIG_MALFORMED: return "malformed";
    case YK_SIG_BAD_FIELD: return "bad-field";
    case YK_SIG_UNKNOWN_KEY: return "unknown-key";
    case YK_SIG_MISMATCH: return "mismatch";
    }
    return "mismatch";
}
