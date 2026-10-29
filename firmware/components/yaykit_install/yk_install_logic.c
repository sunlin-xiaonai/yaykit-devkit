#include "yk_install_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- manifest

static bool printable(const char *s, size_t min, size_t max) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (s[n] < 0x21 || s[n] > 0x7e || n >= max) return false;
    return n >= min;
}

bool yk_inst_slug_ok(const char *slug) {
    if (!slug || !*slug || *slug == '-') return false;
    size_t n = 0;
    for (; slug[n]; n++) {
        const char c = slug[n];
        if (n >= 31 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

yk_forget_t yk_inst_forget_args(int argc, char **argv, const char **slug) {
    *slug = NULL;
    if (argc != 3) return YK_FORGET_USAGE;
    if (!strcmp(argv[2], "--all")) return YK_FORGET_ALL;
    if (!yk_inst_slug_ok(argv[2])) return YK_FORGET_USAGE;
    *slug = argv[2];
    return YK_FORGET_SLUG;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool yk_inst_hex32(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        const int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

const char *yk_inst_check_fields(const char *slug, const char *version, uint32_t size, const char *sha256,
                                 const char *url, const char *signature, bool allow_http) {
    uint8_t digest[32];
    if (!yk_inst_slug_ok(slug) || !printable(version, 1, 31) || !yk_inst_hex32(sha256, digest) ||
        !printable(signature, 1, 111) || size < YK_INST_SECTOR)
        return "BAD_REQUEST";
    if (size > YK_INST_MAX_IMAGE) return "TOO_LARGE";
    if (url && *url) {
        if (!printable(url, 9, 383)) return "BAD_REQUEST";
        if (strncmp(url, "https://", 8) && !(allow_http && !strncmp(url, "http://", 7))) return "NOT_HTTPS";
    }
    return NULL;
}

bool yk_inst_project_matches(const char *project, const char *slug) {
    if (!project || !slug || strncmp(project, "yaykit_", 7)) return false;
    project += 7;
    for (; *project && *slug; project++, slug++)
        if (*project != (*slug == '-' ? '_' : *slug)) return false;
    return !*project && !*slug;
}

// ---------------------------------------------------------------- where an app goes

const char *yk_inst_slot_rule(int slot, const char *slug) {
    if (!slug || !*slug) return NULL;
    return (slot == YK_INST_HOME_SLOT) != !strcmp(slug, "yay") ? "HOME_SLOT_RESERVED" : NULL;
}

const char *yk_inst_slot_rule_project(int slot, const char *project) {
    return (slot == YK_INST_HOME_SLOT) != yk_inst_project_matches(project, "yay") ? "HOME_SLOT_RESERVED" : NULL;
}

static void copy(char *out, size_t size, const char *s) {
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

void yk_inst_choose(const yk_inst_slot_t s[YK_INST_SLOTS], const char *slug, const char *replace,
                    yk_inst_choice_t *out) {
    memset(out, 0, sizeof(*out));
    out->slot = -1;
    if (!yk_inst_slug_ok(slug) || !strcmp(slug, "launcher") || !strcmp(slug, "recovery")) {
        out->error = "BAD_REQUEST";
        return;
    }
    int pick = -1;
    if (!strcmp(slug, "yay")) {
        if (s[YK_INST_HOME_SLOT].running) {
            out->error = "SLOT_IN_USE";
            return;
        }
        pick = YK_INST_HOME_SLOT;
    } else {
#define APP_SLOTS(i) for (int i = YK_INST_HOME_SLOT + 1; i < YK_INST_SLOTS; i++) if (!s[i].running)
        APP_SLOTS(i) if (pick < 0 && s[i].present && !strcmp(s[i].slug, slug)) pick = i;
        APP_SLOTS(i) if (pick < 0 && !s[i].present) pick = i;
        if (replace && *replace) APP_SLOTS(i) if (pick < 0 && s[i].present && !strcmp(s[i].slug, replace)) pick = i;
        APP_SLOTS(i) if (pick < 0 && s[i].broken) pick = i;
        if (pick < 0) APP_SLOTS(i) if (pick < 0 || s[i].used < s[pick].used) pick = i;  // the least recently opened
#undef APP_SLOTS
    }
    if (pick < 0) {
        out->error = "NO_FREE_SLOT";
        return;
    }
    out->slot = pick;
    if (s[pick].present) {
        out->update = !strcmp(s[pick].slug, slug);
        out->replaces = !out->update;
        if (out->replaces) copy(out->replace, sizeof(out->replace), s[pick].slug);
    }
}

// ---------------------------------------------------------------- package cache index

static uint32_t span(uint32_t size) { return (size + YK_INST_BLOCK - 1) / YK_INST_BLOCK * YK_INST_BLOCK; }

int yk_cache_find(const yk_cache_index_t *ix, const char *slug, const char *sha256) {
    for (int i = 0; i < ix->count; i++)
        if (!strcmp(ix->e[i].sha256, sha256) && !strcmp(ix->e[i].slug, slug)) return i;
    return -1;
}

int yk_cache_newest(const yk_cache_index_t *ix, const char *slug) {
    int best = -1;
    for (int i = 0; i < ix->count; i++)
        if (!strcmp(ix->e[i].slug, slug) && (best < 0 || ix->e[i].seq > ix->e[best].seq)) best = i;
    return best;
}

void yk_cache_touch(yk_cache_index_t *ix, int i) {
    if (i >= 0 && i < ix->count) ix->e[i].seq = ++ix->seq;
}

void yk_cache_remove(yk_cache_index_t *ix, int i) {
    if (i < 0 || i >= ix->count) return;
    memmove(&ix->e[i], &ix->e[i + 1], (size_t)(ix->count - i - 1) * sizeof(ix->e[0]));
    ix->count--;
}

static int least_recent(const yk_cache_index_t *ix) {
    int lru = -1;
    for (int i = 0; i < ix->count; i++)
        if (lru < 0 || ix->e[i].seq < ix->e[lru].seq) lru = i;
    return lru;
}

/** First gap of `need` bytes between the entries not `gone` (by offset), or -1. */
static int64_t first_gap(const yk_cache_index_t *ix, const bool *gone, uint32_t need, uint32_t capacity) {
    uint32_t at = 0;
    bool used[YK_INST_CACHE_MAX] = {0};
    for (int k = 0; k < ix->count; k++) {
        int next = -1;
        for (int i = 0; i < ix->count; i++)
            if (!used[i] && (next < 0 || ix->e[i].offset < ix->e[next].offset)) next = i;
        used[next] = true;
        if (gone[next]) continue;
        if (ix->e[next].offset >= at && ix->e[next].offset - at >= need) return at;
        const uint32_t end = ix->e[next].offset + span(ix->e[next].size);
        if (end > at) at = end;
    }
    return capacity >= at && capacity - at >= need ? (int64_t)at : -1;
}

static bool kept_image(const yk_cache_keep_t *keep, const char *sha256) {
    for (int i = 0; keep && i < YK_CACHE_KEEP_MAX; i++)
        if (keep->sha256[i] && !strcmp(sha256, keep->sha256[i])) return true;
    return false;
}

/** Whether `e` may go to make room: `keep->only` names its slug, or names none. */
static bool droppable(const yk_cache_entry_t *e, const yk_cache_keep_t *keep) {
    if (!keep || (!keep->only[0] && !keep->only[1])) return true;
    for (int i = 0; i < 2; i++)
        if (keep->only[i] && !strcmp(e->slug, keep->only[i])) return true;
    return false;
}

/** Whether `e` is of one of keep->last's slugs: it goes only after the others. */
static bool goes_last(const yk_cache_entry_t *e, const yk_cache_keep_t *keep) {
    for (int i = 0; keep && i < 3; i++)
        if (keep->last[i] && !strcmp(e->slug, keep->last[i])) return true;
    return false;
}

bool yk_cache_kept(const yk_cache_entry_t *e, const yk_cache_keep_t *keep) {
    return keep && (kept_image(keep, e->sha256) || (keep->slug && !strcmp(e->slug, keep->slug)));
}

void yk_cache_keep_add(yk_cache_keep_t *keep, const char *sha256) {
    for (int i = 0; keep && sha256 && i < YK_CACHE_KEEP_MAX; i++)
        if (!keep->sha256[i]) {
            keep->sha256[i] = sha256;
            return;
        }
}

bool yk_cache_is_backup(const yk_cache_entry_t *e) { return !strcmp(e->signature, YK_CACHE_BACKUP_SIGNATURE); }

int64_t yk_cache_place(yk_cache_index_t *ix, uint32_t size, uint32_t capacity, const yk_cache_keep_t *keep,
                       const char *replace, int *dropped) {
    const uint32_t need = span(size);
    if (dropped) *dropped = 0;
    if (!size || need > capacity) return YK_CACHE_NEVER;
    // Planned first, dropped only once it fits.
    bool gone[YK_INST_CACHE_MAX] = {0};
    int left = ix->count;
    for (int i = 0; i < ix->count; i++)
        if (replace && !strcmp(ix->e[i].slug, replace) && !kept_image(keep, ix->e[i].sha256))
            gone[i] = true, left--;
    int64_t at = -1;
    while (left >= YK_INST_CACHE_MAX || (at = first_gap(ix, gone, need, capacity)) < 0) {
        int lru = -1;
        for (int i = 0; i < ix->count; i++) {
            if (gone[i] || yk_cache_kept(&ix->e[i], keep) || !droppable(&ix->e[i], keep)) continue;
            const bool last = goes_last(&ix->e[i], keep);
            if (lru < 0 || last < goes_last(&ix->e[lru], keep) ||
                (last == goes_last(&ix->e[lru], keep) && ix->e[i].seq < ix->e[lru].seq))
                lru = i;
        }
        if (lru < 0) return YK_CACHE_FULL;
        gone[lru] = true, left--;
    }
    for (int i = ix->count - 1; i >= 0; i--)
        if (gone[i]) {
            yk_cache_remove(ix, i);
            if (dropped) ++*dropped;
        }
    return at;
}

int yk_cache_fits(const yk_cache_index_t *ix, uint32_t first, uint32_t second, uint32_t capacity,
                  const yk_cache_keep_t *keep) {
    yk_cache_index_t sim = *ix;
    yk_cache_keep_t k = keep ? *keep : (yk_cache_keep_t){0};
    static const char FIRST[] = "first";  // no real image's SHA-256 (those are hex)
    if (first) {
        const int64_t at = yk_cache_place(&sim, first, capacity, &k, NULL, NULL);
        if (at < 0) return (int)at;
        yk_cache_entry_t e = {.size = first, .offset = (uint32_t)at};
        copy(e.sha256, sizeof(e.sha256), FIRST);
        yk_cache_add(&sim, &e);
        yk_cache_keep_add(&k, FIRST);
    }
    if (!second) return 0;
    const int64_t at = yk_cache_place(&sim, second, capacity, &k, NULL, NULL);
    return at < 0 ? (int)at : 0;
}

void yk_cache_add(yk_cache_index_t *ix, const yk_cache_entry_t *entry) {
    if (ix->count >= YK_INST_CACHE_MAX) yk_cache_remove(ix, least_recent(ix));
    ix->e[ix->count] = *entry;
    ix->e[ix->count].seq = ++ix->seq;
    ix->count++;
}

int yk_cache_order(const yk_cache_index_t *ix, int *order, int max) {
    int n = 0;
    bool taken[YK_INST_CACHE_MAX] = {0};
    while (n < max && n < ix->count) {
        int best = -1;
        for (int i = 0; i < ix->count; i++)
            if (!taken[i] && (best < 0 || ix->e[i].seq > ix->e[best].seq)) best = i;
        taken[best] = true;
        order[n++] = best;
    }
    return n;
}

#define BLOB_MAGIC 0x31434B59u  // "YKC1"

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t yk_cache_encode(const yk_cache_index_t *ix, uint8_t *out, size_t size) {
    const size_t len = 16 + (size_t)ix->count * sizeof(yk_cache_entry_t);
    if (size < len) return 0;
    put32(out, BLOB_MAGIC);
    put32(out + 4, sizeof(yk_cache_entry_t));
    put32(out + 8, (uint32_t)ix->count);
    put32(out + 12, ix->seq);
    memcpy(out + 16, ix->e, (size_t)ix->count * sizeof(yk_cache_entry_t));
    return len;
}

#define TERMINATED(field) (memchr((field), 0, sizeof(field)) != NULL)

static bool entry_ok(const yk_cache_entry_t *e, uint32_t capacity) {
    uint8_t digest[32];
    return TERMINATED(e->slug) && TERMINATED(e->name) && TERMINATED(e->version) && TERMINATED(e->sha256) &&
           TERMINATED(e->signature) && yk_inst_slug_ok(e->slug) && printable(e->version, 1, 31) &&
           yk_inst_hex32(e->sha256, digest) && printable(e->signature, 1, 111) && e->size >= YK_INST_SECTOR &&
           e->size <= YK_INST_MAX_IMAGE && e->offset % YK_INST_BLOCK == 0 && e->offset <= capacity &&
           span(e->size) <= capacity - e->offset;
}

bool yk_cache_decode(const uint8_t *in, size_t len, uint32_t capacity, yk_cache_index_t *ix) {
    memset(ix, 0, sizeof(*ix));
    if (!in || len < 16 || get32(in) != BLOB_MAGIC || get32(in + 4) != sizeof(yk_cache_entry_t)) return false;
    const uint32_t count = get32(in + 8);
    if (count > YK_INST_CACHE_MAX || len != 16 + count * sizeof(yk_cache_entry_t)) return false;
    ix->seq = get32(in + 12);
    for (uint32_t i = 0; i < count; i++) {
        yk_cache_entry_t e;
        memcpy(&e, in + 16 + i * sizeof(e), sizeof(e));
        if (!entry_ok(&e, capacity)) continue;
        bool overlaps = false;
        for (int k = 0; k < ix->count; k++)
            overlaps |= e.offset < ix->e[k].offset + span(ix->e[k].size) && ix->e[k].offset < e.offset + span(e.size);
        if (overlaps) continue;
        if (e.seq > ix->seq) ix->seq = e.seq;
        ix->e[ix->count++] = e;
    }
    return true;
}

// ---------------------------------------------------------------- replacing an app

static const char *const REPLACE_STATES[] = {"none", "writing", "restoring"};

int yk_replace_format(char *out, size_t size, const yk_replace_t *r) {
    uint8_t digest[32];
    if (r->state == YK_REPLACE_NONE || r->state > YK_REPLACE_RESTORING || r->slot <= YK_INST_HOME_SLOT ||
        r->slot >= YK_INST_SLOTS || !yk_inst_slug_ok(r->new_slug) || !yk_inst_hex32(r->new_sha256, digest) ||
        !yk_inst_slug_ok(r->old_slug) || !yk_inst_hex32(r->old_sha256, digest))
        return -1;
    const int n = snprintf(out, size, "1|%d|%s|%s|%s|%s|%s|%u", r->slot, r->new_slug, r->new_sha256, r->old_slug,
                           r->old_sha256, REPLACE_STATES[r->state], (unsigned)r->tries);
    return n > 0 && (size_t)n < size ? n : -1;
}

/** The next '|'-separated field of `*in` into `out` (NUL-terminated); false when it does not fit or is missing. */
static bool field(const char **in, char *out, size_t size) {
    const char *end = strchr(*in, '|');
    const size_t n = end ? (size_t)(end - *in) : strlen(*in);
    if (n >= size) return false;
    memcpy(out, *in, n);
    out[n] = 0;
    *in += n + (end ? 1 : 0);
    return end != NULL;
}

bool yk_replace_parse(const char *in, yk_replace_t *out) {
    memset(out, 0, sizeof(*out));
    if (!in || strncmp(in, "1|", 2)) return false;
    in += 2;
    char slot[4] = "", state[12] = "", tries[4] = "";  // a field too long leaves its buffer as it was
    uint8_t digest[32];
    if (!field(&in, slot, sizeof(slot)) || !field(&in, out->new_slug, sizeof(out->new_slug)) ||
        !field(&in, out->new_sha256, sizeof(out->new_sha256)) || !field(&in, out->old_slug, sizeof(out->old_slug)) ||
        !field(&in, out->old_sha256, sizeof(out->old_sha256)) || !field(&in, state, sizeof(state)))
        return false;
    if (field(&in, tries, sizeof(tries)) || !tries[0] || strlen(tries) > 2) return false;  // the last field
    out->slot = slot[0] >= '1' && slot[0] < '0' + YK_INST_SLOTS && !slot[1] ? slot[0] - '0' : -1;
    for (int i = YK_REPLACE_WRITING; i <= YK_REPLACE_RESTORING; i++)
        if (!strcmp(state, REPLACE_STATES[i])) out->state = (yk_replace_state_t)i;
    for (const char *c = tries; *c; c++)
        if (*c < '0' || *c > '9') return false;
    out->tries = (uint8_t)atoi(tries);
    return out->slot > 0 && out->state != YK_REPLACE_NONE && yk_inst_slug_ok(out->new_slug) &&
           yk_inst_hex32(out->new_sha256, digest) && yk_inst_slug_ok(out->old_slug) &&
           yk_inst_hex32(out->old_sha256, digest);
}

yk_replace_step_t yk_replace_next(const yk_replace_in_t *in) {
    if (!in->has) return YK_REPLACE_GO;
    if (in->slot_valid) return YK_REPLACE_CLEAR;  // whichever app it is, it starts: never written over
    if (!in->backup_cached || in->rec.tries >= YK_REPLACE_TRIES) return YK_REPLACE_LOST;
    return YK_REPLACE_RESTORE;
}

bool yk_replace_retry(const yk_replace_t *r, bool backup_ok) { return backup_ok && r->tries < YK_REPLACE_TRIES; }

const char *yk_replace_step_name(yk_replace_step_t step) {
    static const char *const names[] = {"go", "clear", "restore", "lost"};
    return step <= YK_REPLACE_LOST ? names[step] : "?";
}

// ---------------------------------------------------------------- cancel and ③

bool yk_inst_cancel(yk_inst_flags_t *f) {
    if (!f->busy || f->writing) return false;
    f->cancel = true;
    return true;
}

bool yk_inst_begin_write(yk_inst_flags_t *f) {
    if (f->cancel) return false;
    f->writing = true;
    return true;
}

// ---------------------------------------------------------------- writer

static uint32_t sectors(uint32_t n) { return (n + YK_INST_SECTOR - 1) / YK_INST_SECTOR * YK_INST_SECTOR; }

static bool all_ff(const uint8_t *p, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        if (p[i] != 0xFF) return false;
    return true;
}

/** 1 when the range reads all 0xFF, 0 when not, -1 when it cannot be read. */
static int blank(yk_writer_t *w, uint32_t off, uint32_t len) {
    for (uint32_t at = 0; at < len; at += YK_INST_SECTOR) {
        if (w->flash.read(w->flash.ctx, w->base + off + at, w->scratch, YK_INST_SECTOR)) return -1;
        if (!all_ff(w->scratch, YK_INST_SECTOR)) return 0;
    }
    return 1;
}

/** Erases up to `end` (relative), in 64 KB blocks where aligned; ranges already blank are left alone. */
static const char *erase_to(yk_writer_t *w, uint32_t end) {
    const uint32_t limit = sectors(w->size);
    if (end > limit) end = limit;
    while (w->erased < end) {
        const uint32_t n = (w->erased % YK_INST_BLOCK == 0 && w->erased + YK_INST_BLOCK <= limit) ? YK_INST_BLOCK
                                                                                               : YK_INST_SECTOR;
        const int b = w->scratch ? blank(w, w->erased, n) : 0;
        if (b < 0) return "FLASH_ERROR";
        if (b) {
            w->erase_skipped++;
        } else {
            if (w->flash.erase(w->flash.ctx, w->base + w->erased, n)) return "FLASH_ERROR";
            w->erase_ops++;
        }
        w->erased += n;
    }
    return NULL;
}

const char *yk_writer_begin(yk_writer_t *w, const yk_flash_t *flash, uint32_t base, uint32_t size, uint8_t *head,
                            uint8_t *scratch) {
    memset(w, 0, sizeof(*w));
    w->flash = *flash;
    w->base = base;
    w->size = size;
    w->head = head;
    w->scratch = scratch;
    if (!size || base % YK_INST_BLOCK || base > flash->capacity || sectors(size) > flash->capacity - base)
        return "TOO_LARGE";
    mbedtls_sha256_init(&w->sha);
    mbedtls_sha256_starts(&w->sha, 0);
    w->started = true;
    // Head-last: the first block goes now, so the slot no longer holds the old app (its first sector is gone).
    return head ? erase_to(w, YK_INST_BLOCK) : NULL;
}

void yk_writer_restart(yk_writer_t *w) {
    mbedtls_sha256_free(&w->sha);
    mbedtls_sha256_init(&w->sha);
    mbedtls_sha256_starts(&w->sha, 0);
    w->written = 0;
}

const char *yk_writer_write(yk_writer_t *w, const uint8_t *data, uint32_t len) {
    if (len > w->size - w->written) return "SIZE_MISMATCH";
    mbedtls_sha256_update(&w->sha, data, len);
    if (w->head && w->written < YK_INST_SECTOR) {
        const uint32_t n = len < YK_INST_SECTOR - w->written ? len : YK_INST_SECTOR - w->written;
        memcpy(w->head + w->written, data, n);
        w->written += n, data += n, len -= n;
    }
    if (!len) return NULL;
    const char *error = erase_to(w, w->written + len);
    if (error) return error;
    // Erased flash already reads 0xFF; the same bytes again after a restart are the same bytes.
    if (!all_ff(data, len) && w->flash.write(w->flash.ctx, w->base + w->written, data, len)) return "FLASH_ERROR";
    w->written += len;
    return NULL;
}

const char *yk_writer_check(yk_writer_t *w, const uint8_t sha256[32]) {
    if (w->written != w->size) return "SIZE_MISMATCH";
    uint8_t digest[32];
    mbedtls_sha256_finish(&w->sha, digest);
    return memcmp(digest, sha256, 32) ? "HASH_MISMATCH" : NULL;
}

const char *yk_writer_commit_head(yk_writer_t *w) {
    if (!w->head) return NULL;
    const uint32_t n = w->size < YK_INST_SECTOR ? w->size : YK_INST_SECTOR;
    return w->flash.write(w->flash.ctx, w->base, w->head, n) ? "FLASH_ERROR" : NULL;
}

void yk_writer_end(yk_writer_t *w) {
    if (w->started) mbedtls_sha256_free(&w->sha);
    w->started = false;
}

const char *yk_inst_verify(const yk_flash_t *flash, uint32_t base, uint32_t size, const uint8_t sha256[32], uint8_t *buf) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    const char *error = NULL;
    for (uint32_t at = 0; !error && at < size; at += YK_INST_SECTOR) {
        const uint32_t n = size - at < YK_INST_SECTOR ? size - at : YK_INST_SECTOR;
        if (flash->read(flash->ctx, base + at, buf, n)) error = "FLASH_ERROR";
        else mbedtls_sha256_update(&sha, buf, n);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    return error ? error : memcmp(digest, sha256, 32) ? "HASH_MISMATCH" : NULL;
}
