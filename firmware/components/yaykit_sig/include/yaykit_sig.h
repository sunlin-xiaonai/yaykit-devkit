// Checks a slot image's signature before it is installed (docs/design/yay-device.md §5.2 and §6.8,
// firmware/signing/README.md). Pure C and mbedTLS (ESP-IDF's), no allocation: the host tests build it on a computer
// and judge firmware/signing/TEST-ONLY-vectors.json exactly as the store does (packages/cloud/src/firmware-signature.ts).
//
// What is signed: ECDSA P-256 over SHA-256 of
//     yaykit-slot-image/1\nslug=<slug>\nversion=<version>\nsize=<size>\nsha256=<sha256>\nboard=<board>\nlayout=<layout>\n
// every field printable ASCII without spaces (1–80 characters), sha256 64 lowercase hex, size a positive decimal.
// The signature: "yks1.<key id>.<r‖s>", the key id being 16 lowercase hex characters (the first 8 bytes of SHA-256 of
// the key's 65-byte uncompressed point) and r‖s the 64-byte signature in base64url without padding (86 characters).
//
// This component only checks; which keys a build trusts is the caller's (yaykit_install, F0): the release keys, and in a
// development build (CONFIG_YAY_DEV) a developer's key too.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** A trusted public key: the uncompressed P-256 point 0x04 ‖ X ‖ Y. */
#define YK_SIG_KEY_BYTES 65
/** The longest signed text (every field at its longest), with its NUL. */
#define YK_SIG_MESSAGE_MAX 520

/** The image a signature covers, as the install request and the slot name it. */
typedef struct {
    const char *slug;
    const char *version;
    uint64_t size;
    const char *sha256;  // 64 lowercase hex characters
    const char *board;   // "tikpal-r1"
    const char *layout;  // "yaykit-v1"
} yk_sig_image_t;

typedef enum {
    YK_SIG_OK = 0,
    YK_SIG_MALFORMED,    // not "yks1.<16 hex>.<86 base64url>", or not its one spelling
    YK_SIG_BAD_FIELD,    // a field of the image cannot be signed
    YK_SIG_UNKNOWN_KEY,  // signed with a key this build does not trust
    YK_SIG_MISMATCH,     // not that key's signature over this image
} yk_sig_result_t;

/** The text signed for `image` into `out` (NUL-terminated). Returns its length, or 0 when a field cannot be signed or it
 *  does not fit (YK_SIG_MESSAGE_MAX always does). */
size_t yk_sig_message(const yk_sig_image_t *image, char *out, size_t size);

/** A key's id: 16 lowercase hex characters and a NUL. False when `point` is not an uncompressed point. */
bool yk_sig_key_id(const uint8_t point[YK_SIG_KEY_BYTES], char id[17]);

/** Whether `signature` is one of `keys`' over `image`. Needs about 3 KB of stack. */
yk_sig_result_t yk_sig_verify(const yk_sig_image_t *image, const char *signature, const uint8_t (*keys)[YK_SIG_KEY_BYTES],
                              size_t count);

/** "ok", "malformed", "bad-field", "unknown-key", "mismatch": the names the store and the vectors use. */
const char *yk_sig_result_name(yk_sig_result_t result);
