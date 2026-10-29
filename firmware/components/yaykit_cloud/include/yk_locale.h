// The owner's language as the firmware uses it (docs/delivery/2026-10-24-yay-locale-sync.md 3): kept as the platform
// says it ("zh-Hans", "en", "ja", "es-419"; "zh" from before), read by its prefix. Screens have Chinese and English:
// "zh…" is Chinese, anything else (en, ja, es-419, one not known yet) English, which is what 10-20 chose as the main
// language. Pure C, host tested (test/run.sh).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YK_LOCALE_MAX 8  // with its terminator: "zh-Hans" just fits (NVS yk_acct "loc")

/** A value to keep: 2 to 7 characters, a letter first, then letters, digits and '-' ("zh-Hans", "es-419"). */
bool yk_locale_ok(const char *locale);
/** Whether screens are in English: not "zh…". NULL or "" (nothing kept: the device's default) is Chinese. */
bool yk_locale_english(const char *locale);
/** The store's list and the catalog in that language: "zh" or "en" (the device API takes those two). */
const char *yk_locale_catalog(const char *locale);
/** `seen` (hello.ok's owner.locale) into `kept` (YK_LOCALE_MAX) when it is one to keep and another than kept. True
 *  when kept changed. */
bool yk_locale_take(char *kept, const char *seen);

#ifdef __cplusplus
}
#endif
