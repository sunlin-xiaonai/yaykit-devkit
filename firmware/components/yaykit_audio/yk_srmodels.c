#include "yk_srmodels.h"

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

size_t yk_srmodels_header_size(const uint8_t *data, size_t have) {
    if (have < 4) return 0;
    const uint32_t count = u32(data);
    if (count == 0 || count > YK_SRMODELS_MAX_MODELS) return 0;
    size_t at = 4;
    for (uint32_t m = 0; m < count; m++) {
        if (have < at + 36) return 0;
        const uint32_t files = u32(data + at + 32);
        if (files == 0 || files > YK_SRMODELS_MAX_FILES) return 0;
        at += 36 + (size_t)files * 40;
    }
    return have >= at ? at : 0;
}

size_t yk_srmodels_size(const uint8_t *data, size_t have, size_t limit) {
    const size_t header = yk_srmodels_header_size(data, have);
    if (!header) return 0;
    size_t end = header, at = 4;
    const uint32_t count = u32(data);
    for (uint32_t m = 0; m < count; m++) {
        const uint32_t files = u32(data + at + 32);
        at += 36;
        for (uint32_t f = 0; f < files; f++, at += 40) {
            const uint64_t start = u32(data + at + 32), size = u32(data + at + 36);
            if (start < header || start + size > limit) return 0;
            if (start + size > end) end = (size_t)(start + size);
        }
    }
    return end;
}
