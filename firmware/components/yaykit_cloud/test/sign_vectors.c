// Prints a fresh key pair and signatures made by yk_sign.c (with ESP-IDF's mbedTLS) as JSON, for
// verify_vectors.ts to check against the server's verifier. Run through test/run.sh with IDF_PATH set.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../yk_codec.h"
#include "../yk_sign.h"

static int rng(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    arc4random_buf(buf, len);
    return 0;
}

static void print_signature(const char *label, const uint8_t sk[32], const char *message, int last) {
    uint8_t sig[YK_SIGNATURE_MAX];
    char text[128];
    size_t len = yk_sign(sk, message, sig, rng, NULL);
    if (!len || !yk_b64url_encode(sig, len, text, sizeof(text))) {
        fprintf(stderr, "signing failed\n");
        exit(1);
    }
    printf("  \"%s\": {\"message\": \"%s\", \"signature\": \"%s\"}%s\n", label, message, text, last ? "" : ",");
}

int main(void) {
    uint8_t sk[32], pk[65], again[65];
    char key[100], activation[200], token[200];
    if (yk_key_generate(sk, pk, rng, NULL) || yk_key_public(sk, again, rng, NULL) || memcmp(pk, again, sizeof(pk))) {
        fprintf(stderr, "key generation failed\n");
        return 1;
    }
    yk_b64url_encode(pk, sizeof(pk), key, sizeof(key));
    yk_activation_message("yk_00000000beef", "ykc_0123456789abcdefABCDEF", activation, sizeof(activation));
    yk_token_message("yk_00000000beef", "homework-buddy", "1791549000.c2lnbmF0dXJl", token, sizeof(token));
    printf("{\n  \"publicKey\": \"%s\",\n", key);
    print_signature("activation", sk, activation, 0);
    print_signature("token", sk, token, 1);
    printf("}\n");
    return 0;
}
