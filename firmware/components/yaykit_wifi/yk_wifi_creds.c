#include "yk_wifi_creds.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool printable(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return false;
    return true;
}

static bool hex(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

const char *yk_wifi_join_error(const char *ssid, const char *password) {
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len < 1 || ssid_len > 32) return "BAD_SSID";
    size_t len = password ? strlen(password) : 0;
    if (len == 0) return NULL;
    if (len == 64) return hex(password, len) ? NULL : "BAD_PASSWORD";
    if (len < 8 || len > 63 || !printable(password, len)) return "BAD_PASSWORD";
    return NULL;
}
