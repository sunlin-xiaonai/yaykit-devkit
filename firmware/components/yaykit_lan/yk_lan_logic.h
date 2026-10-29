// Yaykit LAN's pure parts (host tested, test/run.sh): the token and hello.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 16 random bytes as 32 lowercase hex characters and a NUL (`out` holds 33). */
void yk_lan_token_text(const uint8_t random[16], char out[33]);
/** Same length and same characters, compared in constant time; an empty `want` never matches. */
bool yk_lan_token_equal(const char *want, const char *got);

typedef enum {
    YK_LAN_HELLO_OK,       // a hello with the right token
    YK_LAN_HELLO_BAD,      // a hello with no token or another one
    YK_LAN_NOT_HELLO,      // valid JSON, but not a hello (or not an object)
    YK_LAN_NOT_JSON,
} yk_lan_hello_t;

/** Reads `text` (JSON) as hello against `token`; the phone's name (cut to fit, control characters as spaces) into
 *  `name` when it is one (`size` > 0). */
yk_lan_hello_t yk_lan_hello(const char *text, const char *token, char *name, size_t size);

#ifdef __cplusplus
}
#endif
