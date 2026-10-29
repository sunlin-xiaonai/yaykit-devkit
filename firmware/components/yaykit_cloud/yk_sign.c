#include "yk_sign.h"

#include <string.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

int yk_key_generate(uint8_t sk[YK_KEY_PRIVATE_BYTES], uint8_t pk[YK_KEY_PUBLIC_BYTES], yk_rng_fn rng, void *rng_ctx) {
    mbedtls_ecp_keypair key;
    mbedtls_ecp_keypair_init(&key);
    size_t len = 0;
    int r = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &key, rng, rng_ctx);
    if (!r) r = mbedtls_ecp_write_key_ext(&key, &len, sk, YK_KEY_PRIVATE_BYTES);
    if (!r && len != YK_KEY_PRIVATE_BYTES) r = -1;
    if (!r) r = mbedtls_ecp_write_public_key(&key, MBEDTLS_ECP_PF_UNCOMPRESSED, &len, pk, YK_KEY_PUBLIC_BYTES);
    if (!r && len != YK_KEY_PUBLIC_BYTES) r = -1;
    mbedtls_ecp_keypair_free(&key);
    return r;
}

int yk_key_public(const uint8_t sk[YK_KEY_PRIVATE_BYTES], uint8_t pk[YK_KEY_PUBLIC_BYTES], yk_rng_fn rng,
                  void *rng_ctx) {
    mbedtls_ecp_keypair key;
    mbedtls_ecp_keypair_init(&key);
    size_t len = 0;
    int r = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &key, sk, YK_KEY_PRIVATE_BYTES);
    if (!r) r = mbedtls_ecp_keypair_calc_public(&key, rng, rng_ctx);
    if (!r) r = mbedtls_ecp_write_public_key(&key, MBEDTLS_ECP_PF_UNCOMPRESSED, &len, pk, YK_KEY_PUBLIC_BYTES);
    if (!r && len != YK_KEY_PUBLIC_BYTES) r = -1;
    mbedtls_ecp_keypair_free(&key);
    return r;
}

size_t yk_sign(const uint8_t sk[YK_KEY_PRIVATE_BYTES], const char *message, uint8_t sig[YK_SIGNATURE_MAX],
               yk_rng_fn rng, void *rng_ctx) {
    unsigned char hash[32];
    if (mbedtls_sha256((const unsigned char *)message, strlen(message), hash, 0)) return 0;
    mbedtls_ecp_keypair key;
    mbedtls_ecp_keypair_init(&key);
    size_t len = 0;
    int r = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &key, sk, YK_KEY_PRIVATE_BYTES);
    if (!r) r = mbedtls_ecdsa_write_signature(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, YK_SIGNATURE_MAX, &len,
                                              rng, rng_ctx);
    mbedtls_ecp_keypair_free(&key);
    return r ? 0 : len;
}
