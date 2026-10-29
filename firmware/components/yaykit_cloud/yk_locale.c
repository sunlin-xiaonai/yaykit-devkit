// The owner's language (yk_locale.h).
#include "yk_locale.h"

#include <string.h>

bool yk_locale_ok(const char *locale) {
    const size_t n = locale ? strlen(locale) : 0;
    if (n < 2 || n >= YK_LOCALE_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = locale[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!letter && (i == 0 || !((c >= '0' && c <= '9') || c == '-'))) return false;
    }
    return true;
}

bool yk_locale_english(const char *locale) {
    if (!locale || !locale[0]) return false;
    return !((locale[0] == 'z' || locale[0] == 'Z') && (locale[1] == 'h' || locale[1] == 'H'));
}

const char *yk_locale_catalog(const char *locale) { return yk_locale_english(locale) ? "en" : "zh"; }

bool yk_locale_take(char *kept, const char *seen) {
    if (!yk_locale_ok(seen) || !strcmp(kept, seen)) return false;
    memcpy(kept, seen, strlen(seen) + 1);
    return true;
}
