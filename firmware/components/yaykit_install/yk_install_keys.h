// The public keys this build trusts for slot images (firmware/signing/README.md): the release keys, the same as the
// store's RELEASE_KEYS (packages/cloud/src/firmware-signature.ts); development builds (CONFIG_YAY_DEV) add the
// developer key, so images built and signed on the bench install on the test device. The store never offers what the
// developer key signs.
#pragma once

#include <stddef.h>

#include "yaykit_sig.h"

/** The keys, and their number in `count`. */
const uint8_t (*yk_install_keys(size_t *count))[YK_SIG_KEY_BYTES];
