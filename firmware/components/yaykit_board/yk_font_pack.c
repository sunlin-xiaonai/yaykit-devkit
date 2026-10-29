#include "yk_font_pack.h"

#include <string.h>

uint32_t yk_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool yk_font_pack_parse(const uint8_t tail[YK_FONT_PACK_TAIL], uint32_t part_size, yk_font_pack_t *out) {
    memset(out, 0, sizeof(*out));
    if (memcmp(tail, "YKFP", 4) || tail[4] != 1 || tail[5] < 1 || tail[5] > YK_FONT_PACK_MAX) return false;
    if (part_size < YK_FONT_PACK_TAIL || yk_crc32(tail, 56) != u32(tail + 56)) return false;
    const uint32_t room = part_size - YK_FONT_PACK_TAIL;
    for (int i = 0; i < tail[5]; i++) {
        const uint8_t *e = tail + 8 + 12 * i;
        yk_font_entry_t *f = &out->fonts[i];
        memcpy(f->lang, e, 4);
        f->lang[4] = 0;
        f->offset = u32(e + 4);
        f->size = u32(e + 8);
        if (!f->lang[0] || f->size < 12 || f->offset > room || f->size > room - f->offset) {
            memset(out, 0, sizeof(*out));
            return false;
        }
    }
    out->count = tail[5];
    return true;
}

/** "ja-JP" -> "ja"; the comparison ignores case. */
static bool same_lang(const char *font_lang, const char *locale) {
    size_t n = 0;
    while (locale[n] && locale[n] != '-' && locale[n] != '_') n++;
    if (n != strlen(font_lang)) return false;
    for (size_t i = 0; i < n; i++) {
        char c = locale[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != font_lang[i]) return false;
    }
    return true;
}

int yk_font_pack_order(const yk_font_pack_t *pack, const char *locale, int order[YK_FONT_PACK_MAX]) {
    int n = 0, first = -1;
    for (int i = 0; locale && i < pack->count; i++)
        if (same_lang(pack->fonts[i].lang, locale)) {
            first = i;
            break;
        }
    if (first >= 0) order[n++] = first;
    for (int i = 0; i < pack->count; i++)
        if (i != first) order[n++] = i;
    return n;
}
