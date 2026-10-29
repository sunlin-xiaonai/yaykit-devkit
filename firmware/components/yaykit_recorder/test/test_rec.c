// Host tests for yk_rec.c. Run: bash firmware/components/yaykit_recorder/test/run.sh
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../yk_rec.h"

static int failures, checks;
#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            failures++;                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);  \
        }                                                          \
    } while (0)

static const char *dir;

static unsigned u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }

static long file_size(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int read_file(const char *name, unsigned char *buf, size_t cap) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static bool meta_has(const char *id, const char *text) {
    char name[64];
    unsigned char buf[512] = {0};
    snprintf(name, sizeof(name), "%s.json", id);
    int n = read_file(name, buf, sizeof(buf) - 1);
    return n > 0 && strstr((char *)buf, text);
}

static void write_seconds(yk_rec_t *r, float seconds, int *result) {
    static int16_t chunk[320];
    int total = (int)(seconds * r->rate * r->channels), at = 0;
    *result = 0;
    while (at < total && *result == 0) {
        int n = total - at < 320 ? total - at : 320;
        for (int i = 0; i < n; i++) chunk[i] = (int16_t)(at + i);
        *result = yk_rec_write(r, chunk, (size_t)n);
        at += n;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    dir = argv[1];
    char id[YK_REC_ID_MAX];
    yk_rec_make_id(id, 1791732612, 0xa1b2);
    CHECK(!strcmp(id, "20261011-153012-a1b2"));
    yk_rec_make_id(id, 0, 0xdeadbeef);
    CHECK(!strcmp(id, "t0-deadbeef"));
    yk_rec_make_id(id, 253402300800, 0xff);  // 10000-01-01: a five-digit year still fits
    CHECK(!strcmp(id, "100000101-000000-00ff"));

    yk_rec_t r;
    CHECK(yk_rec_open(&r, dir, "../escape", 16000, 1, 0, 1, 0) == -1);  // ids never leave the directory

    // A clean recording: 2.5 s mono, finalised and renamed, with its metadata.
    int result;
    CHECK(yk_rec_open(&r, dir, "clean", 16000, 1, 1791732612, 1, 0) == 0);
    CHECK(file_size("clean.wav.part") == 44);
    write_seconds(&r, 2.5f, &result);
    CHECK(result == 0);
    CHECK(r.synced_bytes == 64000);  // the last full second is on the card
    CHECK(yk_rec_seconds(&r) > 2.49f && yk_rec_seconds(&r) < 2.51f);
    CHECK(yk_rec_close(&r) == 0);
    CHECK(file_size("clean.wav.part") == -1);
    CHECK(file_size("clean.wav") == 44 + 80000);
    unsigned char h[64];
    read_file("clean.wav", h, sizeof(h));
    CHECK(!memcmp(h, "RIFF", 4) && u32(h + 40) == 80000 && u32(h + 24) == 16000 && u16(h + 22) == 1);
    CHECK(h[44] == 0 && h[45] == 0 && h[46] == 1 && h[47] == 0);  // first two samples: 0, 1
    CHECK(meta_has("clean", "\"endedBy\":\"stop\""));
    CHECK(meta_has("clean", "\"bytes\":80000"));
    CHECK(meta_has("clean", "\"startedAt\":1791732612"));

    // Power cut after 2.3 s: only what reached the card survives, plus a stray half sample.
    CHECK(yk_rec_open(&r, dir, "cut", 16000, 1, 1791732700, 1, 0) == 0);
    write_seconds(&r, 2.3f, &result);
    uint64_t synced = r.synced_bytes;
    CHECK(synced == 64000);
    fclose(r.f);  // no finalising
    char path[256];
    snprintf(path, sizeof(path), "%s/cut.wav.part", dir);
    CHECK(truncate(path, (off_t)(44 + synced + 3)) == 0);
    // And a file that never got its header.
    snprintf(path, sizeof(path), "%s/stub.wav.part", dir);
    FILE *stub = fopen(path, "wb");
    fwrite("RIFF", 1, 4, stub);
    fclose(stub);

    CHECK(yk_rec_recover(dir) == 1);
    CHECK(file_size("cut.wav.part") == -1 && file_size("stub.wav.part") == -1);
    CHECK(file_size("cut.wav") == (long)(44 + synced + 3));
    read_file("cut.wav", h, sizeof(h));
    CHECK(u32(h + 40) == synced + 2 && u32(h + 4) == 36 + synced + 2);
    CHECK(meta_has("cut", "\"endedBy\":\"recovered\""));
    CHECK(meta_has("cut", "\"startedAt\":1791732700"));  // kept from the metadata written at the start
    CHECK(yk_rec_recover(dir) == 0);

    // Length limit: stops exactly at 1 s, stereo frames stay whole.
    CHECK(yk_rec_open(&r, dir, "limit", 16000, 2, 0, 0.5f, 1) == 0);
    write_seconds(&r, 1.5f, &result);
    CHECK(result == 1);
    CHECK(r.data_bytes == 64000);
    CHECK(yk_rec_close(&r) == 0);
    read_file("limit.wav", h, sizeof(h));
    CHECK(u16(h + 22) == 2 && u16(h + 32) == 4 && u32(h + 28) == 64000 && u32(h + 40) == 64000);

    printf("yaykit_recorder host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
