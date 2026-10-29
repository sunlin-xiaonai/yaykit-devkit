// Host tests for yaykit_sig. Run: bash firmware/components/yaykit_sig/test/run.sh (needs IDF_PATH).
// Every case of firmware/signing/TEST-ONLY-vectors.json (仅测试 keys) must be judged as the store judges it
// (tests/firmware-signature.test.ts), and the signed text must be the same bytes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "yaykit_sig.h"

static int failures, checks;
static const char *current = "";
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, current, #cond); \
        }                                                                        \
    } while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        text = NULL;
    }
    if (text) text[n] = '\0';
    fclose(f);
    return text;
}

static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/** A test key's public point, from its base64url text. */
static bool point_of(const char *text, uint8_t out[YK_SIG_KEY_BYTES]) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (const char *c = text; c && *c; c++) {
        const char *at = strchr(alphabet, *c);
        if (!at) return false;
        acc = (acc << 6) | (uint32_t)(at - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= YK_SIG_KEY_BYTES) return false;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n == YK_SIG_KEY_BYTES;
}

static yk_sig_image_t image_of(const cJSON *v) {
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(v, "size");
    return (yk_sig_image_t){
        .slug = str(v, "slug"),
        .version = str(v, "version"),
        .size = cJSON_IsNumber(size) && size->valuedouble > 0 ? (uint64_t)size->valuedouble : 0,
        .sha256 = str(v, "sha256"),
        .board = str(v, "board"),
        .layout = str(v, "layout"),
    };
}

int main(int argc, char **argv) {
    char *text = slurp(argc > 1 ? argv[1] : "../../../signing/TEST-ONLY-vectors.json");
    if (!text) {
        printf("no vectors\n");
        return 1;
    }
    cJSON *vectors = cJSON_Parse(text);
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(vectors, "keys");

    // The key ids are the store's.
    const cJSON *key;
    cJSON_ArrayForEach(key, keys) {
        current = key->string;
        uint8_t point[YK_SIG_KEY_BYTES];
        char id[17];
        CHECK(point_of(str(key, "publicKey"), point));
        CHECK(yk_sig_key_id(point, id) && !strcmp(id, str(key, "keyId")));
    }

    const cJSON *c;
    int cases = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(vectors, "cases")) {
        current = str(c, "name");
        cases++;
        yk_sig_image_t image = image_of(cJSON_GetObjectItemCaseSensitive(c, "image"));
        if (str(c, "message")) {
            char message[YK_SIG_MESSAGE_MAX];
            CHECK(yk_sig_message(&image, message, sizeof(message)) == strlen(str(c, "message")));
            CHECK(!strcmp(message, str(c, "message")));
        }
        uint8_t trusted[4][YK_SIG_KEY_BYTES];
        size_t count = 0;
        const cJSON *name;
        cJSON_ArrayForEach(name, cJSON_GetObjectItemCaseSensitive(c, "trust")) {
            const cJSON *k = cJSON_GetObjectItemCaseSensitive(keys, name->valuestring);
            CHECK(count < 4 && point_of(str(k, "publicKey"), trusted[count]));
            count++;
        }
        const char *got = yk_sig_result_name(yk_sig_verify(&image, str(c, "signature"), (const uint8_t(*)[YK_SIG_KEY_BYTES])trusted, count));
        if (strcmp(got, str(c, "expect"))) printf("  %s: got %s, expected %s\n", current, got, str(c, "expect"));
        CHECK(!strcmp(got, str(c, "expect")));
    }

    // What the vectors cannot say: nothing given, a buffer too small, a point that is not one.
    current = "edges";
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(vectors, "cases"), 0);
    yk_sig_image_t image = image_of(cJSON_GetObjectItemCaseSensitive(first, "image"));
    const char *signature = str(first, "signature");
    uint8_t test_key[1][YK_SIG_KEY_BYTES];
    CHECK(point_of(str(cJSON_GetObjectItemCaseSensitive(keys, "test"), "publicKey"), test_key[0]));
    CHECK(yk_sig_verify(&image, signature, (const uint8_t(*)[YK_SIG_KEY_BYTES])test_key, 1) == YK_SIG_OK);
    CHECK(yk_sig_verify(NULL, signature, (const uint8_t(*)[YK_SIG_KEY_BYTES])test_key, 1) == YK_SIG_BAD_FIELD);
    CHECK(yk_sig_verify(&image, NULL, (const uint8_t(*)[YK_SIG_KEY_BYTES])test_key, 1) == YK_SIG_MALFORMED);
    CHECK(yk_sig_verify(&image, signature, NULL, 0) == YK_SIG_UNKNOWN_KEY);
    char small[40];
    CHECK(yk_sig_message(&image, small, sizeof(small)) == 0);
    yk_sig_image_t missing = image;
    missing.board = NULL;
    CHECK(yk_sig_message(&missing, small, sizeof(small)) == 0);
    char id[17];
    uint8_t compressed[YK_SIG_KEY_BYTES] = {0x02};
    CHECK(!yk_sig_key_id(compressed, id));
    // A trusted point that is not on the curve, under the id the signature names: refused, not trusted.
    uint8_t off[1][YK_SIG_KEY_BYTES];
    memcpy(off[0], test_key[0], YK_SIG_KEY_BYTES);
    off[0][64] ^= 1;
    char off_id[17], forged[120];
    CHECK(yk_sig_key_id(off[0], off_id));
    snprintf(forged, sizeof(forged), "yks1.%s.%s", off_id, signature + 22);
    CHECK(yk_sig_verify(&image, forged, (const uint8_t(*)[YK_SIG_KEY_BYTES])off, 1) == YK_SIG_MISMATCH);

    printf("yaykit_sig host tests: %d checks over %d vectors, %d failures\n", checks, cases, failures);
    cJSON_Delete(vectors);
    free(text);
    return failures ? 1 : 0;
}
