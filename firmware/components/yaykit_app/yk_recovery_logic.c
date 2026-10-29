// The recovery's version from its ELF sha256 (yk_app_recovery.h). Pure C, host tested (test/run.sh).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yk_app_recovery.h"

// The released recoveries (00060000-yaykit_launcher.bin of each Yay package, release/yay-0.1.x/images). A new release
// whose recovery changed adds a line; one not here is hashed once on the device.
static const struct {
    uint8_t elf[YK_RECOVERY_ELF_KEY];
    const char *version;
    uint32_t size;
    const char *sha256;
} TABLE[] = {
    {{0xa4, 0x6d, 0xf9, 0xf5, 0x5a, 0x54, 0x47, 0xce}, "0.1.0", 1609904,
     "e38d0934112584ade7efa5f6f88605d1ce996869dfa8d9c3fa10b9396e9f421c"},
    {{0x1c, 0x9b, 0x03, 0x2d, 0xfc, 0x9c, 0xf8, 0x09}, "0.1.1", 1609680,
     "0734ed73f13d0a81a9f895b1caf7258e7b1ab71c022812cd40071a2312f86ae4"},
    {{0x6a, 0x58, 0xc5, 0x49, 0x94, 0xcc, 0x75, 0x05}, "0.1.2", 1609680,
     "6f61d5a71a3f18f422fce9f2752b654e8f3e709b9950a7df790e5d7eb1c0a6c1"},
    {{0x95, 0x5e, 0x05, 0xde, 0x33, 0x6b, 0x16, 0xa8}, "0.1.3", 1616384,
     "e96787bd89c3bc2b498d3a131738b6fe7d7bf2b37d59a832fd6193294b2ccbf5"},
    {{0xbe, 0x93, 0x0f, 0x2d, 0xf9, 0x8a, 0x47, 0xed}, "0.1.4", 1616592,
     "51b7736e02ffcd7c47469f4fd4f55fda95d1dda08fea10f65ecdfbff2ad533cb"},
    // 0.1.5's was built again (it links yaykit_app with P2 in it) and still says 0.1.0: checked against the release
    // evidence (yay-0.1.5-evidence.json). From 0.1.6 the description carries the version (P3): no more lines.
    {{0x29, 0x76, 0xa4, 0xb3, 0xf5, 0x47, 0xe7, 0xea}, "0.1.5", 1616624,
     "417b48262d11e981135a937f2933e45eb8bf55fbe4316a4baf74c8e8cea41d5f"},
};

static void copy(char *out, size_t size, const char *in) { snprintf(out, size, "%s", in ? in : ""); }

static bool hex64(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

void yk_recovery_identify(const uint8_t *elf, const char *desc_version, uint32_t image_len, yk_recovery_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->elf, elf, YK_RECOVERY_ELF_KEY);
    out->size = image_len;
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (memcmp(TABLE[i].elf, elf, YK_RECOVERY_ELF_KEY) || TABLE[i].size != image_len) continue;
        copy(out->version, sizeof(out->version), TABLE[i].version);
        copy(out->sha256, sizeof(out->sha256), TABLE[i].sha256);
        out->from = YK_RECOVERY_TABLE;
        return;
    }
    const bool own = desc_version && desc_version[0] && strcmp(desc_version, "0.1.0") &&
                     strlen(desc_version) < sizeof(out->version);
    copy(out->version, sizeof(out->version), own ? desc_version : YK_RECOVERY_UNKNOWN);
    out->from = YK_RECOVERY_PENDING;
}

static void key_hex(char out[2 * YK_RECOVERY_ELF_KEY + 1], const uint8_t *elf) {
    for (int i = 0; i < YK_RECOVERY_ELF_KEY; i++) snprintf(out + 2 * i, 3, "%02x", elf[i]);
}

int yk_recovery_record(char *out, size_t size, const yk_recovery_t *r) {
    if (!hex64(r->sha256)) return -1;
    char key[2 * YK_RECOVERY_ELF_KEY + 1];
    key_hex(key, r->elf);
    const int n = snprintf(out, size, "1|%s|%lu|%s", key, (unsigned long)r->size, r->sha256);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

bool yk_recovery_from_record(const char *rec, yk_recovery_t *r) {
    char key[2 * YK_RECOVERY_ELF_KEY + 1];
    key_hex(key, r->elf);
    // "1|" + key + "|" + length + "|" + sha256
    if (!rec || strncmp(rec, "1|", 2) || strncmp(rec + 2, key, sizeof(key) - 1) || rec[2 + sizeof(key) - 1] != '|')
        return false;
    const char *len = rec + 2 + sizeof(key);
    char *end;
    const unsigned long n = strtoul(len, &end, 10);
    if (end == len || *end != '|' || n != r->size || !hex64(end + 1)) return false;
    copy(r->sha256, sizeof(r->sha256), end + 1);
    r->from = YK_RECOVERY_NVS;
    return true;
}

bool yk_recovery_hashed(yk_recovery_t *r, const char *sha256) {
    if (!hex64(sha256)) return false;
    copy(r->sha256, sizeof(r->sha256), sha256);
    r->from = YK_RECOVERY_HASHED;
    return true;
}

const char *yk_recovery_from_name(yk_recovery_from_t from) {
    switch (from) {
    case YK_RECOVERY_TABLE: return "table";
    case YK_RECOVERY_NVS: return "nvs";
    case YK_RECOVERY_HASHED: return "hashed";
    case YK_RECOVERY_PENDING: return "pending";
    default: return "none";
    }
}
