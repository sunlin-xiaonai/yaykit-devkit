// Host tests for yaykit_install's pure parts (yk_install_logic.c) and its key table (yk_install_keys.c), built with
// ESP-IDF's mbedTLS. Flash is simulated as NOR: erasing sets bytes to 0xFF, writing can only clear bits, so a write
// over data that was not erased first fails the test.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/sha256.h"
#include "yk_install_keys.h"
#include "yk_install_logic.h"

static int failures;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)
#define STREQ(a, b) CHECK(((a) == NULL && (b) == NULL) || ((a) && (b) && !strcmp((a), (b))))

// ---------------------------------------------------------------- simulated NOR flash

typedef struct {
    uint8_t *mem;
    uint32_t size;
    int erases, writes, bad_writes;
    int fail_after_writes;  // > 0: the write that makes it this many fails (power cut)
} nor_t;

static int nor_erase(void *ctx, uint32_t off, uint32_t len) {
    nor_t *f = ctx;
    if (off % YK_INST_SECTOR || len % YK_INST_SECTOR || off + len > f->size) return 1;
    memset(f->mem + off, 0xFF, len);
    f->erases++;
    return 0;
}

static int nor_write(void *ctx, uint32_t off, const void *data, uint32_t len) {
    nor_t *f = ctx;
    if (off + len > f->size) return 1;
    if (f->fail_after_writes && ++f->writes >= f->fail_after_writes) return 1;
    const uint8_t *d = data;
    for (uint32_t i = 0; i < len; i++) {
        if ((f->mem[off + i] & d[i]) != d[i]) f->bad_writes++;  // a 0 bit would have to become 1: not erased
        f->mem[off + i] &= d[i];
    }
    return 0;
}

static int nor_read(void *ctx, uint32_t off, void *data, uint32_t len) {
    nor_t *f = ctx;
    if (off + len > f->size) return 1;
    memcpy(data, f->mem + off, len);
    return 0;
}

static nor_t nor_new(uint32_t size, uint8_t fill) {
    nor_t f = {.mem = malloc(size), .size = size};
    memset(f.mem, fill, size);
    return f;
}

static yk_flash_t flash_of(nor_t *f) {
    return (yk_flash_t){.erase = nor_erase, .write = nor_write, .read = nor_read, .ctx = f, .capacity = f->size};
}

static void sha(const uint8_t *data, size_t len, uint8_t out[32]) { mbedtls_sha256(data, len, out, 0); }

static uint8_t *image(uint32_t size, uint32_t seed) {
    uint8_t *p = malloc(size);
    for (uint32_t i = 0; i < size; i++) p[i] = (uint8_t)((i * 2654435761u + seed) >> 13);
    // Some 0xFF runs, as in real images' padding.
    if (size > 3 * YK_INST_SECTOR) memset(p + 2 * YK_INST_SECTOR, 0xFF, YK_INST_SECTOR);
    return p;
}

/** Writes `img` in chunks of `chunk` bytes. */
static const char *write_all(yk_writer_t *w, const uint8_t *img, uint32_t size, uint32_t chunk) {
    for (uint32_t at = 0; at < size; at += chunk) {
        const uint32_t n = size - at < chunk ? size - at : chunk;
        const char *e = yk_writer_write(w, img + at, n);
        if (e) return e;
    }
    return NULL;
}

static void test_writer_slot(void) {
    const uint32_t cap = 0x380000, size = 1234567;
    nor_t f = nor_new(cap, 0x00);  // an old app everywhere
    yk_flash_t flash = flash_of(&f);
    uint8_t head[YK_INST_SECTOR], scratch[YK_INST_SECTOR], digest[32];
    uint8_t *img = image(size, 7);
    sha(img, size, digest);
    yk_writer_t w;
    STREQ(yk_writer_begin(&w, &flash, 0, size, head, scratch), NULL);
    // Right away the slot no longer holds an app: its first sector reads blank.
    CHECK(f.mem[0] == 0xFF && f.mem[YK_INST_SECTOR - 1] == 0xFF);
    STREQ(write_all(&w, img, size, 1000), NULL);  // odd chunks
    // Before the commit the first sector is still blank; the rest is there.
    CHECK(f.mem[0] == 0xFF);
    CHECK(!memcmp(f.mem + YK_INST_SECTOR, img + YK_INST_SECTOR, size - YK_INST_SECTOR));
    STREQ(yk_writer_check(&w, digest), NULL);
    STREQ(yk_writer_commit_head(&w), NULL);
    CHECK(!memcmp(f.mem, img, size));
    CHECK(f.bad_writes == 0);
    // 64 KB blocks where aligned: 1234567 bytes = 18 full blocks + 54 KB (14 sectors).
    CHECK(w.erase_ops == 18 + 14 && w.erase_skipped == 0);
    // Past the image, the old app's bytes are left alone beyond the last erased sector.
    const uint32_t end = (size + YK_INST_SECTOR - 1) / YK_INST_SECTOR * YK_INST_SECTOR;
    CHECK(f.mem[end] == 0x00 && f.mem[end - 1] == 0xFF);
    yk_writer_end(&w);
    // Too much data, too little, a wrong hash.
    STREQ(yk_writer_begin(&w, &flash, 0, size, head, scratch), NULL);
    STREQ(write_all(&w, img, size, 4096), NULL);
    STREQ(yk_writer_write(&w, img, 1), "SIZE_MISMATCH");
    digest[0] ^= 1;
    STREQ(yk_writer_check(&w, digest), "HASH_MISMATCH");
    yk_writer_end(&w);
    STREQ(yk_writer_begin(&w, &flash, 0, size, head, scratch), NULL);
    STREQ(write_all(&w, img, size - 1, 4096), NULL);
    STREQ(yk_writer_check(&w, digest), "SIZE_MISMATCH");
    yk_writer_end(&w);
    STREQ(yk_writer_begin(&w, &flash, 0, cap + 1, head, scratch), "TOO_LARGE");
    free(img);
    free(f.mem);
}

static void test_writer_power_cut_and_restart(void) {
    const uint32_t cap = 0x380000, size = 700000;
    nor_t f = nor_new(cap, 0x5A);
    yk_flash_t flash = flash_of(&f);
    uint8_t head[YK_INST_SECTOR], scratch[YK_INST_SECTOR], digest[32];
    uint8_t *img = image(size, 99);
    sha(img, size, digest);
    yk_writer_t w;
    // Power cut half way: the slot reads empty (no image header), never half an app.
    f.fail_after_writes = 50;
    STREQ(yk_writer_begin(&w, &flash, 0, size, head, scratch), NULL);
    STREQ(write_all(&w, img, size, 4096), "FLASH_ERROR");
    CHECK(f.mem[0] == 0xFF);
    yk_writer_end(&w);
    f.fail_after_writes = 0;
    // A server that ignores the range: the same bytes from the start again, written over themselves.
    STREQ(yk_writer_begin(&w, &flash, 0, size, head, scratch), NULL);
    STREQ(write_all(&w, img, 300000, 4096), NULL);
    const uint32_t erased = w.erase_ops;
    yk_writer_restart(&w);
    STREQ(write_all(&w, img, size, 4096), NULL);
    STREQ(yk_writer_check(&w, digest), NULL);
    STREQ(yk_writer_commit_head(&w), NULL);
    CHECK(!memcmp(f.mem, img, size));
    CHECK(f.bad_writes == 0);
    CHECK(w.erase_ops > erased);  // only what lies beyond the first pass
    yk_writer_end(&w);
    free(img);
    free(f.mem);
}

static void test_writer_cache_extent(void) {
    const uint32_t cap = 10 * 1024 * 1024, size = 200000, base = 3 * YK_INST_BLOCK;
    nor_t f = nor_new(cap, 0xFF);  // a fresh cache partition: blank, nothing to erase
    yk_flash_t flash = flash_of(&f);
    uint8_t scratch[YK_INST_SECTOR], digest[32];
    uint8_t *img = image(size, 3);
    sha(img, size, digest);
    yk_writer_t w;
    STREQ(yk_writer_begin(&w, &flash, base, size, NULL, scratch), NULL);
    STREQ(write_all(&w, img, size, 4096), NULL);
    STREQ(yk_writer_check(&w, digest), NULL);
    CHECK(!memcmp(f.mem + base, img, size));
    CHECK(w.erase_ops == 0 && w.erase_skipped > 0);
    CHECK(f.erases == 0 && f.bad_writes == 0);
    yk_writer_end(&w);
    // Over an older image: erased first, no stale bits.
    uint8_t *other = image(size, 4);
    sha(other, size, digest);
    STREQ(yk_writer_begin(&w, &flash, base, size, NULL, scratch), NULL);
    STREQ(write_all(&w, other, size, 4096), NULL);
    STREQ(yk_writer_check(&w, digest), NULL);
    CHECK(!memcmp(f.mem + base, other, size));
    CHECK(f.bad_writes == 0 && w.erase_ops > 0);
    yk_writer_end(&w);
    STREQ(yk_writer_begin(&w, &flash, base + 1, size, NULL, scratch), "TOO_LARGE");  // not block aligned
    STREQ(yk_writer_begin(&w, &flash, cap - YK_INST_BLOCK, size, NULL, scratch), "TOO_LARGE");
    free(img);
    free(other);
    free(f.mem);
}

// ---------------------------------------------------------------- manifests

static const char *SHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char *SIG = "yks1.07f1c022330a0a65.AAAA";

static void test_fields(void) {
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "https://assets.yaykit.io/b.bin", SIG, false), NULL);
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "", SIG, false), NULL);  // from the cache
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "http://192.168.1.2/b.bin", SIG, false), "NOT_HTTPS");
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "http://192.168.1.2/b.bin", SIG, true), NULL);
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "ftp://x/b.bin", SIG, true), "NOT_HTTPS");
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "https://a b", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("Breathe", "0.1.0", 900000, SHA, "", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("-x", "0.1.0", 900000, SHA, "", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("breathe", "0.1 0", 900000, SHA, "", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("breathe", "", 900000, SHA, "", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 100, SHA, "", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 0x380001, SHA, "", SIG, false), "TOO_LARGE");
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef", "", SIG, false), "BAD_REQUEST");
    STREQ(yk_inst_check_fields("breathe", "0.1.0", 900000, SHA, "", "", false), "BAD_REQUEST");
    CHECK(yk_inst_project_matches("yaykit_party_play", "party-play"));
    CHECK(yk_inst_project_matches("yaykit_yay", "yay"));
    CHECK(!yk_inst_project_matches("yaykit_party_play", "party"));
    CHECK(!yk_inst_project_matches("yaykit_party", "party-play"));
    CHECK(!yk_inst_project_matches("party_play", "party-play"));
}

// ---------------------------------------------------------------- where an app goes

static yk_inst_slot_t slot(bool present, const char *slug, int64_t used) {
    yk_inst_slot_t s = {.present = present, .used = used};
    snprintf(s.slug, sizeof(s.slug), "%s", slug);
    return s;
}

static void test_choose(void) {
    yk_inst_choice_t c;
    // Yay runs from slot 0; two apps.
    yk_inst_slot_t s[3] = {slot(true, "yay", 0), slot(true, "party-play", 2000), slot(true, "breathe", 1000)};
    s[0].running = true;
    yk_inst_choose(s, "breathe", NULL, &c);  // the same app: an update in place
    CHECK(c.slot == 2 && c.update && !c.replaces);
    yk_inst_choose(s, "translator", NULL, &c);  // full: the least recently opened goes
    CHECK(c.slot == 2 && c.replaces && !strcmp(c.replace, "breathe") && !c.error);
    yk_inst_choose(s, "translator", "party-play", &c);  // what Yay told the user
    CHECK(c.slot == 1 && c.replaces && !strcmp(c.replace, "party-play"));
    yk_inst_choose(s, "translator", "homework-buddy", &c);  // a name that is not there: the least recent
    CHECK(c.slot == 2);
    s[1].broken = true;
    yk_inst_choose(s, "translator", NULL, &c);  // a broken one before the least recent
    CHECK(c.slot == 1);
    s[1].broken = false;
    s[2] = slot(false, "", 0);
    yk_inst_choose(s, "translator", "party-play", &c);  // an empty slot before replacing anything
    CHECK(c.slot == 2 && !c.replaces && !c.update);
    s[2] = slot(true, "breathe", 0);  // never opened: counts as oldest
    yk_inst_choose(s, "translator", NULL, &c);
    CHECK(c.slot == 2);
    yk_inst_choose(s, "yay", NULL, &c);  // never over the running main firmware
    CHECK(c.slot == -1 && !strcmp(c.error, "SLOT_IN_USE"));
    yk_inst_choose(s, "launcher", NULL, &c);
    CHECK(c.slot == -1 && !strcmp(c.error, "BAD_REQUEST"));
    // An app running from slot 2 (a bench) installs into slot 1 only; Yay can go into slot 0.
    s[0].running = false;
    s[2].running = true;
    yk_inst_choose(s, "translator", "breathe", &c);
    CHECK(c.slot == 1 && !strcmp(c.replace, "party-play"));
    yk_inst_choose(s, "yay", NULL, &c);
    CHECK(c.slot == 0 && c.update);
    s[1].running = true;  // nowhere left
    yk_inst_choose(s, "translator", NULL, &c);
    CHECK(c.slot == -1 && !strcmp(c.error, "NO_FREE_SLOT"));
}

static void test_home_slot(void) {
    // 位置 1 takes Yay only; Yay goes nowhere else (HOME_SLOT_RESERVED).
    STREQ(yk_inst_slot_rule(0, "yay"), NULL);
    STREQ(yk_inst_slot_rule(0, "party-play"), "HOME_SLOT_RESERVED");
    STREQ(yk_inst_slot_rule(1, "party-play"), NULL);
    STREQ(yk_inst_slot_rule(2, "breathe"), NULL);
    STREQ(yk_inst_slot_rule(1, "yay"), "HOME_SLOT_RESERVED");
    STREQ(yk_inst_slot_rule(2, "yay"), "HOME_SLOT_RESERVED");
    STREQ(yk_inst_slot_rule(1, ""), NULL);  // a USB install naming no app: its project name decides
    STREQ(yk_inst_slot_rule(0, NULL), NULL);
    // The image's project name, whatever the request said.
    STREQ(yk_inst_slot_rule_project(0, "yaykit_yay"), NULL);
    STREQ(yk_inst_slot_rule_project(0, "yaykit_party_play"), "HOME_SLOT_RESERVED");
    STREQ(yk_inst_slot_rule_project(0, "yaykit_yay_extra"), "HOME_SLOT_RESERVED");
    STREQ(yk_inst_slot_rule_project(1, "yaykit_party_play"), NULL);
    STREQ(yk_inst_slot_rule_project(2, "yaykit_yay"), "HOME_SLOT_RESERVED");
}

// ---------------------------------------------------------------- package cache index

static yk_cache_entry_t entry(const char *slug, uint32_t size, char sha_digit) {
    yk_cache_entry_t e = {.size = size};
    snprintf(e.slug, sizeof(e.slug), "%s", slug);
    snprintf(e.version, sizeof(e.version), "0.1.0");
    snprintf(e.signature, sizeof(e.signature), "%s", SIG);
    memset(e.sha256, sha_digit, 64);
    return e;
}

static int64_t place_keep(yk_cache_index_t *ix, const char *slug, uint32_t size, char d, uint32_t cap,
                          const yk_cache_keep_t *keep, const char *replace, int *dropped) {
    const int64_t at = yk_cache_place(ix, size, cap, keep, replace, dropped);
    if (at >= 0) {
        yk_cache_entry_t e = entry(slug, size, d);
        e.offset = (uint32_t)at;
        yk_cache_add(ix, &e);
    }
    return at;
}

static int64_t place_add(yk_cache_index_t *ix, const char *slug, uint32_t size, char d, int *dropped) {
    return place_keep(ix, slug, size, d, 10 * 1024 * 1024, NULL, NULL, dropped);
}

static void test_cache(void) {
    const uint32_t MB = 1024 * 1024, cap = 10 * MB;
    yk_cache_index_t ix = {0};
    int dropped;
    // Five apps of about 2 MB fit in 10 MB.
    CHECK(place_add(&ix, "party-play", 2 * MB, 'a', &dropped) == 0);
    CHECK(place_add(&ix, "breathe", 900000, 'b', &dropped) == 2 * MB);
    CHECK(place_add(&ix, "homework-buddy", 1900000, 'c', &dropped) == 2 * MB + 14 * YK_INST_BLOCK);  // 900000 bytes take 14 blocks
    CHECK(place_add(&ix, "language-buddy", 2 * MB, 'd', &dropped) >= 0 && !dropped);
    CHECK(place_add(&ix, "translator", 2 * MB, 'e', &dropped) >= 0 && !dropped);
    CHECK(ix.count == 5);
    CHECK(yk_cache_find(&ix, "breathe", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 1);
    CHECK(yk_cache_find(&ix, "breathe", SHA) == -1);
    CHECK(yk_cache_find(&ix, "party-play", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == -1);
    // Using one makes it the most recent: a 3.4 MB image then drops the least recently used until it fits.
    yk_cache_touch(&ix, yk_cache_newest(&ix, "party-play"));
    const int64_t at = place_add(&ix, "big", 3400000, 'f', &dropped);
    CHECK(at >= 0 && dropped >= 1);
    CHECK(yk_cache_newest(&ix, "party-play") >= 0);
    CHECK(yk_cache_newest(&ix, "breathe") < 0);  // the oldest went first
    // No overlaps, all inside.
    for (int i = 0; i < ix.count; i++) {
        CHECK(ix.e[i].offset % YK_INST_BLOCK == 0 && ix.e[i].offset + ix.e[i].size <= cap);
        for (int k = 0; k < ix.count; k++)
            if (k != i) CHECK(ix.e[i].offset + ix.e[i].size <= ix.e[k].offset || ix.e[k].offset + ix.e[k].size <= ix.e[i].offset);
    }
    // Most recent first.
    int order[YK_INST_CACHE_MAX];
    const int n = yk_cache_order(&ix, order, YK_INST_CACHE_MAX);
    CHECK(n == ix.count && !strcmp(ix.e[order[0]].slug, "big") && !strcmp(ix.e[order[1]].slug, "party-play"));
    // Never more than YK_INST_CACHE_MAX entries; too big never fits.
    yk_cache_index_t small = {0};
    for (int i = 0; i < 8; i++) {
        char slug[8];
        snprintf(slug, sizeof(slug), "app%d", i);
        CHECK(place_add(&small, slug, 5000, 'a' + i, &dropped) >= 0);
    }
    CHECK(small.count == YK_INST_CACHE_MAX && yk_cache_newest(&small, "app0") < 0 && yk_cache_newest(&small, "app7") >= 0);
    CHECK(yk_cache_place(&small, cap + 1, cap, NULL, NULL, &dropped) == YK_CACHE_NEVER);
    // Stored and read back; junk and overlaps are dropped.
    uint8_t blob[YK_CACHE_BLOB_MAX];
    const size_t len = yk_cache_encode(&ix, blob, sizeof(blob));
    yk_cache_index_t back;
    CHECK(len && yk_cache_decode(blob, len, cap, &back));
    CHECK(back.count == ix.count && back.seq == ix.seq && !memcmp(back.e, ix.e, sizeof(ix.e[0]) * ix.count));
    CHECK(!yk_cache_decode(blob, len - 1, cap, &back) && back.count == 0);
    CHECK(yk_cache_decode(blob, len, 4 * MB, &back) && back.count < ix.count);  // a smaller partition: outside ones go
    yk_cache_index_t bad = {0};
    for (int i = 0; i < 5; i++) {
        char slug[8];
        snprintf(slug, sizeof(slug), "app%d", i);
        CHECK(place_add(&bad, slug, MB, 'a' + i, &dropped) == (int64_t)i * MB);
    }
    bad.e[1].offset = bad.e[0].offset;                        // overlaps the first
    bad.e[2].slug[0] = 'X';                                   // not a slug
    memset(bad.e[3].version, 'v', sizeof(bad.e[3].version));  // not terminated
    const size_t bad_len = yk_cache_encode(&bad, blob, sizeof(blob));
    CHECK(yk_cache_decode(blob, bad_len, cap, &back) && back.count == 2);
    CHECK(!strcmp(back.e[0].slug, "app0") && !strcmp(back.e[1].slug, "app4"));
    CHECK(yk_cache_encode(&ix, blob, 10) == 0);
    yk_cache_remove(&ix, 0);
    CHECK(ix.count == n - 1);
}

static bool has_image(const yk_cache_index_t *ix, const char *slug, char d) {
    char sha[65];
    memset(sha, d, 64);
    sha[64] = 0;
    return yk_cache_find(ix, slug, sha) >= 0;
}

// The main firmware's update (yk_app_update.h): from the request until the new Yay is confirmed, the cache never drops
// its new image or the previous version for room; what does not fit beside them is refused (NO_SPACE).
static void test_cache_update(void) {
    const uint32_t MB = 1024 * 1024, cap = 10 * MB, YAY = 3355443;  // Yay about 3.2 MB
    const char NEW = 'n', PREV = 'p';
    char new_sha[65];
    memset(new_sha, NEW, 64);
    new_sha[64] = 0;
    const yk_cache_keep_t keep = {.sha256 = {new_sha}, .slug = "yay-previous"};
    int dropped;
    // As on a device: the previous version from an earlier update, 聚会玩, 呼吸之间, then Yay downloads the new image
    // (no update going yet: nothing kept). 9.4 MB of 10.
    yk_cache_index_t ix = {0};
    CHECK(place_add(&ix, "yay-previous", YAY, 'o', &dropped) == 0);
    CHECK(place_add(&ix, "party-play", 2 * MB, 'a', &dropped) >= 0);
    CHECK(place_add(&ix, "breathe", 900000, 'b', &dropped) >= 0);
    CHECK(place_add(&ix, "yay", YAY, NEW, &dropped) >= 0 && !dropped && ix.count == 4);
    // Kept: the new image (any slug) and every previous version; nothing else, and nothing without an update.
    CHECK(yk_cache_kept(&ix.e[yk_cache_newest(&ix, "yay")], &keep));
    CHECK(yk_cache_kept(&ix.e[yk_cache_newest(&ix, "yay-previous")], &keep));
    CHECK(!yk_cache_kept(&ix.e[yk_cache_newest(&ix, "party-play")], &keep));
    CHECK(!yk_cache_kept(&ix.e[0], NULL));
    yk_cache_entry_t other = entry("yay", YAY, 'x');
    CHECK(!yk_cache_kept(&other, &keep));  // another image of Yay
    // The recovery's backup (pending): the older previous version makes way for the Yay that ran; the new image stays.
    int64_t at = place_keep(&ix, "yay-previous", YAY, PREV, cap, &keep, "yay-previous", &dropped);
    CHECK(at >= 0 && dropped == 1);
    CHECK(has_image(&ix, "yay", NEW) && has_image(&ix, "yay-previous", PREV) && !has_image(&ix, "yay-previous", 'o'));
    CHECK(has_image(&ix, "party-play", 'a') && has_image(&ix, "breathe", 'b'));
    // On trial. The update's two made the least recently used: a plain LRU would drop them first.
    for (int i = 0; i < ix.count; i++)
        if (!yk_cache_kept(&ix.e[i], &keep)) yk_cache_touch(&ix, i);
    // A 2.5 MB app downloads: the other two go, the update's stay.
    at = place_keep(&ix, "homework-buddy", 2500000, 'h', cap, &keep, NULL, &dropped);
    CHECK(at >= 0 && dropped == 2);
    CHECK(has_image(&ix, "yay", NEW) && has_image(&ix, "yay-previous", PREV) && has_image(&ix, "homework-buddy", 'h'));
    // A 4 MB one does not fit beside them: refused, and nothing was dropped for it.
    const int before = ix.count;
    CHECK(yk_cache_place(&ix, 4 * MB, cap, &keep, NULL, &dropped) == YK_CACHE_FULL && !dropped && ix.count == before);
    CHECK(has_image(&ix, "homework-buddy", 'h'));
    // Without the update going, the same 4 MB app may drop the previous version (and the confirmed Yay's image).
    yk_cache_index_t after = ix;
    CHECK(yk_cache_place(&after, 4 * MB, cap, NULL, NULL, &dropped) >= 0 && dropped >= 1);
    // A backup that can not fit beside the new image: NO_SPACE, and the older previous version, 聚会玩 stay.
    yk_cache_index_t big = {0};
    CHECK(place_add(&big, "party-play", 2 * MB, 'a', &dropped) == 0);
    CHECK(place_add(&big, "yay-previous", 3 * MB, 'o', &dropped) >= 0);
    CHECK(place_add(&big, "yay", 4500000, NEW, &dropped) >= 0 && big.count == 3);
    CHECK(yk_cache_place(&big, 6 * MB, cap, &keep, "yay-previous", &dropped) == YK_CACHE_FULL && !dropped);
    CHECK(big.count == 3 && has_image(&big, "yay-previous", 'o') && has_image(&big, "party-play", 'a'));
    CHECK(yk_cache_place(&big, cap + 1, cap, &keep, "yay-previous", &dropped) == YK_CACHE_NEVER);  // never fits
    // `replace` never takes the update's new image: a Yay of the same slug replaces only the other ones.
    yk_cache_index_t same = {0};
    CHECK(place_add(&same, "yay", MB, 'x', &dropped) == 0);
    CHECK(place_add(&same, "yay", MB, NEW, &dropped) >= 0);
    CHECK(place_keep(&same, "yay", MB, 'y', cap, &keep, "yay", &dropped) >= 0 && dropped == 1);
    CHECK(has_image(&same, "yay", NEW) && has_image(&same, "yay", 'y') && !has_image(&same, "yay", 'x'));
    // A full index (YK_INST_CACHE_MAX images) whose oldest are the update's: the oldest other one makes room.
    yk_cache_index_t full = {0};
    CHECK(place_add(&full, "yay-previous", 5000, PREV, &dropped) == 0);
    CHECK(place_add(&full, "yay", 5000, NEW, &dropped) >= 0);
    for (int i = 0; full.count < YK_INST_CACHE_MAX; i++) {
        char slug[8];
        snprintf(slug, sizeof(slug), "app%d", i);
        CHECK(place_add(&full, slug, 5000, 'a' + i, &dropped) >= 0);
    }
    CHECK(place_keep(&full, "app9", 5000, 'z', cap, &keep, NULL, &dropped) >= 0 && dropped == 1);
    CHECK(full.count == YK_INST_CACHE_MAX && has_image(&full, "yay-previous", PREV) && has_image(&full, "yay", NEW));
    CHECK(yk_cache_newest(&full, "app0") < 0 && yk_cache_newest(&full, "app1") >= 0);
}

// Yay's update downloading in the background (yk_install_fetch, make_room false; docs/delivery/2026-10-26-yay-update-e.md
// 2.2): only Yay's own older images may go for its new one and the previous version beside it, never an app's.
static void test_cache_only(void) {
    const uint32_t MB = 1024 * 1024, cap = 10 * MB;
    int dropped;
    const yk_cache_keep_t only = {.only = {"yay", "yay-previous"}};
    // The last update's two (3.3 MB each) and one app: the new Yay and the previous one fit once Yay's two go.
    yk_cache_index_t ix = {0};
    CHECK(place_add(&ix, "yay-previous", 3300000, 'p', &dropped) >= 0);
    CHECK(place_add(&ix, "yay", 3300000, 'o', &dropped) >= 0);
    CHECK(place_add(&ix, "breathe", 1702272, 'b', &dropped) >= 0);
    CHECK(yk_cache_fits(&ix, 3300000, 3300000, cap, &only) == 0);
    yk_cache_index_t a = ix;
    CHECK(place_keep(&a, "yay", 3300000, 'n', cap, &only, NULL, &dropped) >= 0 && dropped >= 1);
    CHECK(yk_cache_newest(&a, "breathe") >= 0 && has_image(&a, "yay", 'n'));
    // Apps fill the rest: none goes, so it does not fit, and nothing is dropped by asking or placing.
    yk_cache_index_t b = {0};
    CHECK(place_add(&b, "yay", 3300000, 'o', &dropped) >= 0);
    CHECK(place_add(&b, "party-play", 1745664, 'a', &dropped) >= 0);
    CHECK(place_add(&b, "language-buddy", 2300000, 'd', &dropped) >= 0);
    CHECK(place_add(&b, "translator", 2400000, 't', &dropped) >= 0);
    CHECK(yk_cache_fits(&b, 3300000, 3300000, cap, &only) == YK_CACHE_FULL);
    // A tap may push the apps out as an install does (nothing in `only`).
    CHECK(yk_cache_fits(&b, 3300000, 3300000, cap, NULL) == 0);
    // The new image alone: Yay's old one goes, the apps stay.
    CHECK(place_keep(&b, "yay", 3300000, 'n', cap, &only, NULL, &dropped) >= 0 && dropped == 1);
    CHECK(!has_image(&b, "yay", 'o') && yk_cache_newest(&b, "party-play") >= 0 &&
          yk_cache_newest(&b, "language-buddy") >= 0 && yk_cache_newest(&b, "translator") >= 0);
    // Only apps there: nothing may go, nothing goes.
    yk_cache_index_t c = {0};
    CHECK(place_add(&c, "party-play", 3200000, 'a', &dropped) >= 0);
    CHECK(place_add(&c, "language-buddy", 3200000, 'd', &dropped) >= 0);
    CHECK(place_add(&c, "translator", 3200000, 't', &dropped) >= 0);
    const yk_cache_index_t before = c;
    CHECK(place_keep(&c, "yay", 3300000, 'n', cap, &only, NULL, &dropped) == YK_CACHE_FULL && dropped == 0);
    CHECK(!memcmp(&before, &c, sizeof(c)));
}

// Yay's night update making room (0.1.10, YK_FETCH_ROOM_APPS_LAST): any app's cached copy may go, those of apps in no
// slot first, then the least recently used of the apps in the slots. 10-28 on yk_3bfc4: breathe 1.7 MB and translator
// 3.1 MB in no slot, recording-card 2.3 MB in one; the new Yay 3.43 MB and the backup 3.48 MB beside it.
static void test_cache_apps_last(void) {
    const uint32_t MB = 1024 * 1024, cap = 10 * MB;
    int dropped;
    const yk_cache_keep_t last = {.last = {"recording-card", "selfie-lab", NULL}};
    yk_cache_index_t ix = {0};
    CHECK(place_add(&ix, "recording-card", 2342480, 'r', &dropped) >= 0);  // the oldest, but in a slot
    CHECK(place_add(&ix, "breathe", 1702272, 'b', &dropped) >= 0);
    CHECK(place_add(&ix, "translator", 3104816, 't', &dropped) >= 0);
    // As the background did in 0.1.8 and 0.1.9 (only Yay's own): NO_SPACE.
    const yk_cache_keep_t only = {.only = {"yay", "yay-previous"}};
    CHECK(yk_cache_fits(&ix, 3433728, 3478208, cap, &only) == YK_CACHE_FULL);
    // Apps last: it fits, by dropping the two in no slot; the one in a slot stays, though it is the least recently used.
    CHECK(yk_cache_fits(&ix, 3433728, 3478208, cap, &last) == 0);
    yk_cache_index_t a = ix;
    CHECK(place_keep(&a, "yay", 3433728, 'n', cap, &last, NULL, &dropped) >= 0);
    CHECK(yk_cache_newest(&a, "recording-card") >= 0 && has_image(&a, "yay", 'n'));
    // In no slot first: both go (breathe's room alone is not next to the free end), the one in a slot stays.
    CHECK(dropped == 2 && yk_cache_newest(&a, "breathe") < 0 && yk_cache_newest(&a, "translator") < 0);
    // Not enough with those gone: then the apps in the slots, the least recently used first.
    yk_cache_index_t b = {0};
    CHECK(place_add(&b, "recording-card", 3600000, 'r', &dropped) >= 0);  // in a slot, the least recently used
    CHECK(place_add(&b, "selfie-lab", 2000000, 's', &dropped) >= 0);      // in a slot
    CHECK(place_add(&b, "party-play", 2000000, 'a', &dropped) >= 0);      // in no slot
    yk_cache_index_t c = b;
    CHECK(place_keep(&c, "yay", 3433728, 'n', cap, &last, NULL, &dropped) >= 0 && dropped == 1);
    CHECK(yk_cache_newest(&c, "party-play") < 0 && yk_cache_newest(&c, "recording-card") >= 0);
    // The backup beside it, the new image held (as yk_cache_fits and the recovery hold it).
    yk_cache_keep_t with_new = last;
    with_new.sha256[0] = c.e[yk_cache_newest(&c, "yay")].sha256;
    CHECK(place_keep(&c, "yay-previous", 3478208, 'p', cap, &with_new, NULL, &dropped) >= 0 && dropped == 1);
    CHECK(yk_cache_newest(&c, "recording-card") < 0 && yk_cache_newest(&c, "selfie-lab") >= 0);
    // What `keep` holds never goes, last or not.
    yk_cache_keep_t hold = last;
    hold.sha256[0] = c.e[yk_cache_newest(&c, "selfie-lab")].sha256;
    hold.sha256[1] = c.e[yk_cache_newest(&c, "yay")].sha256;
    hold.sha256[2] = c.e[yk_cache_newest(&c, "yay-previous")].sha256;
    const yk_cache_index_t before = c;
    CHECK(place_keep(&c, "translator", 3200000, 't', cap, &hold, NULL, &dropped) == YK_CACHE_FULL && dropped == 0);
    CHECK(!memcmp(&before, &c, sizeof(c)));
}

// An app taking the place of another (yaykit_install.h, three stages): the new image and the backup of the one it
// replaces fit beside each other, and neither goes for room while the other is placed; the main firmware's update keeps
// its two whatever happens.
static void test_cache_replace(void) {
    const uint32_t MB = 1024 * 1024, cap = 10 * MB;
    int dropped;
    char new_sha[65], old_sha[65];
    memset(new_sha, 'r', 64), new_sha[64] = 0;  // 录音卡, 2.3 MB, replacing
    memset(old_sha, 't', 64), old_sha[64] = 0;  // 翻译, 3.1 MB, installed over USB: not cached
    // A full-ish cache: four apps the device downloaded before, the oldest first.
    yk_cache_index_t ix = {0};
    CHECK(place_add(&ix, "party-play", 1745664, 'a', &dropped) >= 0);
    CHECK(place_add(&ix, "breathe", 1702272, 'b', &dropped) >= 0);
    CHECK(place_add(&ix, "homework-buddy", 1900000, 'c', &dropped) >= 0);
    CHECK(place_add(&ix, "language-buddy", 2300000, 'd', &dropped) >= 0 && ix.count == 4);
    // Both fit (what goes is decided when each is placed); the index is not changed by asking.
    const yk_cache_index_t before = ix;
    CHECK(yk_cache_fits(&ix, 2342480, 3104192, cap, NULL) == 0);
    CHECK(!memcmp(&before, &ix, sizeof(ix)));
    CHECK(yk_cache_fits(&ix, 0, 3104192, cap, NULL) == 0);  // the new one cached already: only the backup
    CHECK(yk_cache_fits(&ix, 0, 0, cap, NULL) == 0);
    CHECK(yk_cache_fits(&ix, cap + 1, 0, cap, NULL) == YK_CACHE_NEVER);
    CHECK(yk_cache_fits(&ix, 6 * MB, 5 * MB, cap, NULL) == YK_CACHE_FULL);  // each fits alone, not both
    // ① the new image (the oldest ones make room), ② the backup beside it: the new one stays although it is not the
    // most recent any more once the others are touched.
    yk_cache_keep_t keep = {0};
    yk_cache_keep_add(&keep, old_sha);  // the old app's own copy, were it cached, is not dropped while it downloads
    CHECK(place_keep(&ix, "recording-card", 2342480, 'r', cap, &keep, NULL, &dropped) >= 0);
    for (int i = 0; i < ix.count; i++)
        if (strcmp(ix.e[i].slug, "recording-card")) yk_cache_touch(&ix, i);
    yk_cache_keep_add(&keep, new_sha);
    CHECK(place_keep(&ix, "translator", 3104192, 't', cap, &keep, NULL, &dropped) >= 0 && dropped >= 1);
    CHECK(has_image(&ix, "recording-card", 'r') && has_image(&ix, "translator", 't'));
    // While the main firmware updates (its two, 3.2 MB each), there is no room for both: refused before anything.
    char yay_sha[65];
    memset(yay_sha, 'n', 64), yay_sha[64] = 0;
    yk_cache_index_t up = {0};
    CHECK(place_add(&up, "yay-previous", 3355443, 'p', &dropped) >= 0);
    CHECK(place_add(&up, "yay", 3355443, 'n', &dropped) >= 0);
    CHECK(place_add(&up, "breathe", 1702272, 'b', &dropped) >= 0);
    const yk_cache_keep_t update = {.sha256 = {yay_sha}, .slug = "yay-previous"};
    CHECK(yk_cache_fits(&up, 2342480, 3104192, cap, &update) == YK_CACHE_FULL);
    CHECK(yk_cache_fits(&up, 2342480, 0, cap, &update) == 0);  // into an empty slot: no backup, it fits
    CHECK(yk_cache_fits(&up, 2342480, 3104192, cap, NULL) == 0);  // after the update: room again
    // At most YK_CACHE_KEEP_MAX images kept; a fifth is ignored, not written past the end.
    yk_cache_keep_t four = {0};
    const char *const shas[] = {"1", "2", "3", "4", "5"};
    for (int i = 0; i < 5; i++) yk_cache_keep_add(&four, shas[i]);
    CHECK(four.sha256[3] == shas[3]);
    yk_cache_entry_t e = entry("x", 5000, 'x');
    snprintf(e.sha256, sizeof(e.sha256), "5");
    CHECK(!yk_cache_kept(&e, &four));
    snprintf(e.sha256, sizeof(e.sha256), "4");
    CHECK(yk_cache_kept(&e, &four));
    yk_cache_keep_add(NULL, "x");  // nothing
    // A backup without a signature is told apart.
    CHECK(!yk_cache_is_backup(&e));
    snprintf(e.signature, sizeof(e.signature), YK_CACHE_BACKUP_SIGNATURE);
    CHECK(yk_cache_is_backup(&e));
}

static void test_replace_record(void) {
    yk_replace_t r = {.state = YK_REPLACE_WRITING, .slot = 1, .tries = 0};
    snprintf(r.new_slug, sizeof(r.new_slug), "breathe");
    memset(r.new_sha256, 'b', 64);
    snprintf(r.old_slug, sizeof(r.old_slug), "recording-card");
    memset(r.old_sha256, '9', 64);
    char buf[256];
    const int n = yk_replace_format(buf, sizeof(buf), &r);
    CHECK(n > 0 && !strncmp(buf, "1|1|breathe|bbbb", 16) && strstr(buf, "|recording-card|9999") &&
          !strcmp(buf + n - 10, "|writing|0"));
    yk_replace_t back;
    CHECK(yk_replace_parse(buf, &back) && !memcmp(&back, &r, sizeof(r)));
    r.state = YK_REPLACE_RESTORING, r.tries = 2, r.slot = 2;
    CHECK(yk_replace_format(buf, sizeof(buf), &r) > 0 && yk_replace_parse(buf, &back) && !memcmp(&back, &r, sizeof(r)));
    CHECK(yk_replace_format(buf, 40, &r) == -1);  // does not fit
    // Not one of ours: dropped.
    yk_replace_t bad = r;
    bad.slot = 0;  // 位置 1 is Yay's, never replaced this way
    CHECK(yk_replace_format(buf, sizeof(buf), &bad) == -1);
    bad = r, bad.state = YK_REPLACE_NONE;
    CHECK(yk_replace_format(buf, sizeof(buf), &bad) == -1);
    bad = r, bad.new_sha256[3] = 'X';
    CHECK(yk_replace_format(buf, sizeof(buf), &bad) == -1);
    yk_replace_format(buf, sizeof(buf), &r);
    char edited[300];
    snprintf(edited, sizeof(edited), "2%s", buf + 1);  // another format
    CHECK(!yk_replace_parse(edited, &back));
    snprintf(edited, sizeof(edited), "%s|1", buf);  // a field too many
    CHECK(!yk_replace_parse(edited, &back));
    snprintf(edited, sizeof(edited), "%.*s", (int)strlen(buf) - 2, buf);  // the last field gone
    CHECK(!yk_replace_parse(edited, &back));
    snprintf(edited, sizeof(edited), "%s", buf);
    edited[2] = '3';  // no such slot
    CHECK(!yk_replace_parse(edited, &back));
    snprintf(edited, sizeof(edited), "%s", buf);
    *strstr(edited, "restoring") = 'R';
    CHECK(!yk_replace_parse(edited, &back));
    snprintf(edited, sizeof(edited), "%s", buf);
    edited[strlen(edited) - 1] = 'x';  // tries not a number
    CHECK(!yk_replace_parse(edited, &back));
    CHECK(!yk_replace_parse("", &back) && !yk_replace_parse(NULL, &back));
}

static void test_replace_next(void) {
    yk_replace_in_t in = {0};
    CHECK(yk_replace_next(&in) == YK_REPLACE_GO);  // no record
    in.has = true;
    in.rec.state = YK_REPLACE_WRITING;
    // Cut before the first block was erased, after the new app's first sector, or another app installed since: an
    // app that starts is never written over.
    in.slot_valid = true, in.backup_cached = true;
    CHECK(yk_replace_next(&in) == YK_REPLACE_CLEAR);
    in.backup_cached = false;
    CHECK(yk_replace_next(&in) == YK_REPLACE_CLEAR);
    // Cut half way (the slot reads empty: its first sector is written last): the old app back.
    in.slot_valid = false, in.backup_cached = true;
    CHECK(yk_replace_next(&in) == YK_REPLACE_RESTORE);
    in.rec.state = YK_REPLACE_RESTORING, in.rec.tries = 2;  // cut while putting it back: again
    CHECK(yk_replace_next(&in) == YK_REPLACE_RESTORE);
    in.rec.tries = YK_REPLACE_TRIES;  // given up
    CHECK(yk_replace_next(&in) == YK_REPLACE_LOST);
    in.rec.tries = 0, in.backup_cached = false;  // the backup gone (or bad)
    CHECK(yk_replace_next(&in) == YK_REPLACE_LOST);
    STREQ(yk_replace_step_name(YK_REPLACE_RESTORE), "restore");
}

// The slot written from the cache is cut at every point (power gone): the slot reads empty each time, and the backup
// in the cache puts the old app back byte for byte; the download and the backup only ever touch the cache.
static void test_replace_cut_and_restore(void) {
    const uint32_t cap = 0x380000, old_size = 900000, new_size = 700000, cache_cap = 4 * 1024 * 1024;
    nor_t slot = nor_new(cap, 0xFF), cache = nor_new(cache_cap, 0xFF);
    yk_flash_t sf = flash_of(&slot), cf = flash_of(&cache);
    uint8_t head[YK_INST_SECTOR], scratch[YK_INST_SECTOR], old_digest[32], new_digest[32];
    uint8_t *old_img = image(old_size, 11), *new_img = image(new_size, 12);
    sha(old_img, old_size, old_digest);
    sha(new_img, new_size, new_digest);
    yk_writer_t w;
    // The old app in its slot.
    STREQ(yk_writer_begin(&w, &sf, 0, old_size, head, scratch), NULL);
    STREQ(write_all(&w, old_img, old_size, 4096), NULL);
    STREQ(yk_writer_check(&w, old_digest), NULL);
    STREQ(yk_writer_commit_head(&w), NULL);
    yk_writer_end(&w);
    // ① the new image into the cache, ② the slot's app copied beside it: the slot is not written.
    const uint32_t new_at = 0, old_at = 15 * YK_INST_BLOCK;
    const int slot_writes = slot.writes;
    STREQ(yk_writer_begin(&w, &cf, new_at, new_size, NULL, scratch), NULL);
    STREQ(write_all(&w, new_img, new_size, 4096), NULL);
    STREQ(yk_writer_check(&w, new_digest), NULL);
    yk_writer_end(&w);
    STREQ(yk_writer_begin(&w, &cf, old_at, old_size, NULL, scratch), NULL);
    STREQ(write_all(&w, slot.mem, old_size, 4096), NULL);  // read from the slot
    STREQ(yk_writer_check(&w, old_digest), NULL);
    yk_writer_end(&w);
    CHECK(slot.writes == slot_writes && !memcmp(slot.mem, old_img, old_size));
    // ③ cut at many points, from the first write to the last chunk before the first sector.
    for (int cut = 1; cut < (int)(new_size / 4096); cut += 17) {
        slot.writes = 0;
        slot.fail_after_writes = cut;
        STREQ(yk_writer_begin(&w, &sf, 0, new_size, head, scratch), NULL);
        CHECK(write_all(&w, cache.mem + new_at, new_size, 4096) != NULL);
        yk_writer_end(&w);
        CHECK(slot.mem[0] == 0xFF);  // empty: the next start puts the old one back
        slot.fail_after_writes = 0;
        STREQ(yk_writer_begin(&w, &sf, 0, old_size, head, scratch), NULL);
        STREQ(write_all(&w, cache.mem + old_at, old_size, 4096), NULL);
        STREQ(yk_writer_check(&w, old_digest), NULL);
        STREQ(yk_writer_commit_head(&w), NULL);
        yk_writer_end(&w);
        CHECK(!memcmp(slot.mem, old_img, old_size));
    }
    // Not cut: the new app.
    STREQ(yk_writer_begin(&w, &sf, 0, new_size, head, scratch), NULL);
    STREQ(write_all(&w, cache.mem + new_at, new_size, 4096), NULL);
    STREQ(yk_writer_check(&w, new_digest), NULL);
    STREQ(yk_writer_commit_head(&w), NULL);
    yk_writer_end(&w);
    CHECK(!memcmp(slot.mem, new_img, new_size));
    CHECK(slot.bad_writes == 0 && cache.bad_writes == 0);
    free(old_img);
    free(new_img);
    free(slot.mem);
    free(cache.mem);
}

// Review of #536: the order of a cancel and ③, reading the backup back, the record's length, giving up, and the
// leftovers a cut between two steps may leave.
static void test_replace_review(void) {
    // 2. A cancel and the start of ③ under one lock: whichever comes first decides.
    yk_inst_flags_t f = {.busy = true};
    CHECK(yk_inst_cancel(&f) && f.cancel);
    CHECK(!yk_inst_begin_write(&f) && !f.writing);  // cancelled first: the slot is not touched
    f = (yk_inst_flags_t){.busy = true};
    CHECK(yk_inst_begin_write(&f) && f.writing);
    CHECK(!yk_inst_cancel(&f) && !f.cancel);  // writing first: the cancel is refused
    f = (yk_inst_flags_t){0};
    CHECK(!yk_inst_cancel(&f));  // nothing runs
    // 3. A backup that went bad on flash is caught before the slot is written over.
    const uint32_t size = 300000, base = 2 * YK_INST_BLOCK;
    nor_t cache = nor_new(1024 * 1024, 0xFF);
    yk_flash_t cf = flash_of(&cache);
    uint8_t scratch[YK_INST_SECTOR], buf[YK_INST_SECTOR], digest[32];
    uint8_t *img = image(size, 21);
    sha(img, size, digest);
    yk_writer_t w;
    STREQ(yk_writer_begin(&w, &cf, base, size, NULL, scratch), NULL);
    STREQ(write_all(&w, img, size, 4096), NULL);
    STREQ(yk_writer_check(&w, digest), NULL);
    yk_writer_end(&w);
    STREQ(yk_inst_verify(&cf, base, size, digest, buf), NULL);
    cache.mem[base + 123456] ^= 0x10;  // one bit flipped
    STREQ(yk_inst_verify(&cf, base, size, digest, buf), "HASH_MISMATCH");
    STREQ(yk_inst_verify(&cf, cache.size, size, digest, buf), "FLASH_ERROR");  // past the end
    free(img);
    free(cache.mem);
    // 7. The longest record fits its buffer (it did not fit 200); a tries field too long is not read uninitialised.
    yk_replace_t r = {.state = YK_REPLACE_RESTORING, .slot = 2, .tries = 2};
    memset(r.new_slug, 'n', 31), memset(r.old_slug, 'o', 31);
    memset(r.new_sha256, 'a', 64), memset(r.old_sha256, 'b', 64);
    char text[YK_REPLACE_TEXT_MAX], back_text[YK_REPLACE_TEXT_MAX];
    const int n = yk_replace_format(text, sizeof(text), &r);
    CHECK(n > 200 && n < YK_REPLACE_TEXT_MAX);
    yk_replace_t back;
    CHECK(yk_replace_parse(text, &back) && !memcmp(&back, &r, sizeof(r)));
    snprintf(back_text, sizeof(back_text), "%.*s12345", n - 1, text);  // tries "212345"
    CHECK(!yk_replace_parse(back_text, &back));
    // 5. Put back and failed now: kept for the next start while the backup reads right and tries are left.
    r.tries = 1;
    CHECK(yk_replace_retry(&r, true));
    CHECK(!yk_replace_retry(&r, false));  // the backup is bad: given up
    r.tries = YK_REPLACE_TRIES;
    CHECK(!yk_replace_retry(&r, true));
    // 8. The backup goes before the record: a cut between them leaves a record whose slot holds an app (installed, or
    //    put back) and no backup: cleared, never taken for a lost app.
    yk_replace_in_t in = {.has = true, .rec = r, .slot_valid = true, .backup_cached = false};
    CHECK(yk_replace_next(&in) == YK_REPLACE_CLEAR);
    // 8. The same image installed again: the copy ① made is found as the slot's (slug and SHA-256), no second backup.
    yk_cache_index_t ix = {0};
    int dropped;
    CHECK(place_add(&ix, "breathe", 1702272, 'b', &dropped) >= 0);
    char bsha[65];
    memset(bsha, 'b', 64), bsha[64] = 0;
    CHECK(yk_cache_find(&ix, "breathe", bsha) == 0 && ix.count == 1);
}

// ---------------------------------------------------------------- keys

static void test_keys(void) {
    size_t count;
    const uint8_t (*keys)[YK_SIG_KEY_BYTES] = yk_install_keys(&count);
    char id[17];
    CHECK(count == 2);  // built with CONFIG_YAY_DEV (test/sdkconfig.h)
    CHECK(yk_sig_key_id(keys[0], id) && !strcmp(id, "07f1c022330a0a65"));
    CHECK(count > 1 && yk_sig_key_id(keys[1], id) && !strcmp(id, "acdc1a5c2d08e68c"));
    // A signature from a test key (firmware/signing/TEST-ONLY-vectors.json) is never trusted.
    const yk_sig_image_t img = {.slug = "party-play", .version = "0.2.0", .size = 1234567,
                                .sha256 = "a3f1c2d4e5b6a7980a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f6071",
                                .board = "tikpal-r1", .layout = "yaykit-v1"};
    CHECK(yk_sig_verify(&img, "yks1.0000000000000000.AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", keys, count) == YK_SIG_UNKNOWN_KEY);
}

/** `pkg forget`: nothing after it (a line cut short) must not empty the cache (10-25). */
static void test_forget_args(void) {
    const char *slug = "x";
    char *bare[] = {"pkg", "forget"};
    CHECK(yk_inst_forget_args(2, bare, &slug) == YK_FORGET_USAGE && !slug);
    char *all[] = {"pkg", "forget", "--all"};
    CHECK(yk_inst_forget_args(3, all, &slug) == YK_FORGET_ALL && !slug);
    char *one[] = {"pkg", "forget", "breathe"};
    CHECK(yk_inst_forget_args(3, one, &slug) == YK_FORGET_SLUG && slug && !strcmp(slug, "breathe"));
    char *more[] = {"pkg", "forget", "breathe", "0.2.3"};
    CHECK(yk_inst_forget_args(4, more, &slug) == YK_FORGET_USAGE && !slug);
    char *all_more[] = {"pkg", "forget", "--all", "now"};
    CHECK(yk_inst_forget_args(4, all_more, &slug) == YK_FORGET_USAGE);
    char *odd[] = {"pkg", "forget", "-a"};
    CHECK(yk_inst_forget_args(3, odd, &slug) == YK_FORGET_USAGE);
    char *empty[] = {"pkg", "forget", ""};
    CHECK(yk_inst_forget_args(3, empty, &slug) == YK_FORGET_USAGE);
}

int main(void) {
    test_writer_slot();
    test_writer_power_cut_and_restart();
    test_writer_cache_extent();
    test_fields();
    test_choose();
    test_home_slot();
    test_cache();
    test_cache_update();
    test_cache_replace();
    test_cache_only();
    test_cache_apps_last();
    test_replace_record();
    test_replace_next();
    test_replace_cut_and_restore();
    test_replace_review();
    test_keys();
    test_forget_args();
    if (failures) {
        printf("yaykit_install: %d failed\n", failures);
        return 1;
    }
    printf("yaykit_install: all passed\n");
    return 0;
}
