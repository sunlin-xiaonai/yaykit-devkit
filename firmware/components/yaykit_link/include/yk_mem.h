// The internal-RAM figures every firmware reports (`yk info`, `app mem`, YK_TALK mem; firmware-guide.md 5 「内存」).
// ESP-IDF's heap_caps_get_minimum_free_size() is the sum of each internal region's own lowest, from boot, and one
// 32 KB region filled once early in the start keeps it ~32 KB under the real lowest for good (10-24, both test
// devices). So the line (40 KB) is read from yk_mem_ready() on, which yk_app_ready() calls: ESP-IDF's local minimum
// monitor counts each region afresh from there.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** From now on the lowest is counted afresh (idempotent; yk_app_ready() calls it once the screen is up). */
void yk_mem_ready(void);
/** Internal RAM's lowest since yk_mem_ready(), the figure the 40 KB line is read on; before it, from boot. */
size_t yk_mem_min_since_ready(void);
/** The figure from before 10-24 (from boot), kept while the two are compared. After yk_mem_ready() it is the lower
 *  of the two spans' sums: the old one took each region's lowest over both, so it may read a little higher now. */
size_t yk_mem_min_ever(void);

/** yk_mem_min_ever() from the parts (plain C, host tested): the boot figure at yk_mem_ready() and the lowest since. */
size_t yk_mem_ever(bool ready, size_t at_ready, size_t since_ready);

#ifdef __cplusplus
}
#endif
