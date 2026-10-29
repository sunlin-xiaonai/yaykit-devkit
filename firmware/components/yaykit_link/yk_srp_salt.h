// SRP6a salt and verifier for protocomm security 2 (the phone link's pairing code), without esp_srp's 1-in-256 salt
// that no client can pair with (yk_srp_salt.c).
#pragma once

#include <stdint.h>

#include "esp_err.h"

#define YK_SRP_SALT_LEN 16

/** The verifier for user/pass with this salt; *verifier is malloc'd (free() it). */
esp_err_t yk_srp_verifier(const char *user, const char *pass, const uint8_t salt[YK_SRP_SALT_LEN], char **verifier,
                          int *verifier_len);
/** A fresh salt (first byte never 0) and the verifier for user/pass; *verifier is malloc'd (free() it). */
esp_err_t yk_srp_salt_verifier(const char *user, const char *pass, uint8_t salt[YK_SRP_SALT_LEN], char **verifier,
                               int *verifier_len);
