// The device's P-256 key: generation, public point and ECDSA/SHA-256 signatures (DER).
// mbedTLS only, so the host tests build it from ESP-IDF's mbedTLS and check it against the server's verifier.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define YK_KEY_PRIVATE_BYTES 32
#define YK_KEY_PUBLIC_BYTES 65  // uncompressed point: 0x04 || X || Y
#define YK_SIGNATURE_MAX 72     // DER

typedef int (*yk_rng_fn)(void *ctx, unsigned char *buf, size_t len);

/** Makes a key pair. Returns 0 on success. */
int yk_key_generate(uint8_t sk[YK_KEY_PRIVATE_BYTES], uint8_t pk[YK_KEY_PUBLIC_BYTES], yk_rng_fn rng, void *rng_ctx);

/** Recomputes the public point from the private scalar (to check what NVS holds). Returns 0 on success. */
int yk_key_public(const uint8_t sk[YK_KEY_PRIVATE_BYTES], uint8_t pk[YK_KEY_PUBLIC_BYTES], yk_rng_fn rng,
                  void *rng_ctx);

/** DER signature of SHA-256(message). Returns its length, or 0 on failure. Needs about 3 KB of stack. */
size_t yk_sign(const uint8_t sk[YK_KEY_PRIVATE_BYTES], const char *message, uint8_t sig[YK_SIGNATURE_MAX],
               yk_rng_fn rng, void *rng_ctx);
