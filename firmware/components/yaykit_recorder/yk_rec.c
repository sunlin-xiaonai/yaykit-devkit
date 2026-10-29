#define _POSIX_C_SOURCE 200809L
#include "yk_rec.h"

#include <dirent.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HEADER 44
#define WAV_MAX_DATA (0xFFFFFFFFull - 36)
#define MAX_PENDING 16

static void le32(uint8_t *p, uint32_t v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
    p[2] = (v >> 16) & 255;
    p[3] = (v >> 24) & 255;
}

static void le16(uint8_t *p, uint16_t v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void wav_header(uint8_t h[HEADER], uint32_t rate, uint16_t channels, uint32_t data) {
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);  // PCM
    le16(h + 22, channels);
    le32(h + 24, rate);
    le32(h + 28, rate * channels * 2);
    le16(h + 32, (uint16_t)(channels * 2));
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}

static int commit(FILE *f) { return fflush(f) == 0 && fsync(fileno(f)) == 0 ? 0 : -1; }

/** Rewrites the header for `data` bytes and commits it to the card. */
static int put_header(FILE *f, uint32_t rate, uint16_t channels, uint64_t data) {
    uint8_t h[HEADER];
    wav_header(h, rate, channels, (uint32_t)data);
    if (fflush(f) || fseek(f, 0, SEEK_SET) || fwrite(h, 1, HEADER, f) != HEADER || fseek(f, 0, SEEK_END)) return -1;
    return commit(f);
}

static void path_of(char *out, const char *dir, const char *id, const char *suffix) {
    snprintf(out, YK_REC_PATH_MAX, "%s/%s%s", dir, id, suffix);
}

static int write_meta(const char *dir, const char *id, int64_t started, uint32_t rate, uint16_t channels,
                      uint64_t bytes, const char *ended) {
    char path[YK_REC_PATH_MAX];
    path_of(path, dir, id, ".json");
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    double seconds = rate && channels ? (double)bytes / (rate * channels * 2.0) : 0;
    fprintf(f,
            "{\"id\":\"%s\",\"startedAt\":%" PRId64 ",\"rate\":%" PRIu32 ",\"channels\":%u,\"bytes\":%" PRIu64
            ",\"seconds\":%.3f,\"endedBy\":\"%s\"}\n",
            id, started, rate, (unsigned)channels, bytes, seconds, ended);
    int ok = commit(f);
    return fclose(f) == 0 ? ok : -1;
}

void yk_rec_make_id(char out[YK_REC_ID_MAX], int64_t unix_seconds, uint32_t random) {
    struct tm tm;
    time_t t = (time_t)unix_seconds;
    // gmtime_r keeps the fields in range, so this always fits (at most 27 characters); checking the length tells
    // GCC so (-Wformat-truncation), and anything longer would count as an unknown time.
    if (unix_seconds > 0 && gmtime_r(&t, &tm) &&
        snprintf(out, YK_REC_ID_MAX, "%04d%02d%02d-%02d%02d%02d-%04" PRIx32, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, random & 0xffff) < YK_REC_ID_MAX)
        return;
    snprintf(out, YK_REC_ID_MAX, "t0-%08" PRIx32, random);
}

static bool valid_id(const char *id) {
    size_t n = strlen(id);
    if (!n || n >= YK_REC_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

int yk_rec_open(yk_rec_t *r, const char *dir, const char *id, uint32_t rate, uint16_t channels,
                int64_t started, float sync_seconds, float max_seconds) {
    memset(r, 0, sizeof(*r));
    if (!valid_id(id) || !rate || !channels || strlen(dir) >= sizeof(r->dir)) return -1;
    strcpy(r->dir, dir);
    strcpy(r->id, id);
    r->rate = rate;
    r->channels = channels;
    r->started = started;
    uint64_t block = channels * 2u, per_second = (uint64_t)rate * block;
    r->sync_every = (uint64_t)(sync_seconds * per_second) / block * block;
    if (r->sync_every < block) r->sync_every = block;
    r->max_bytes = WAV_MAX_DATA / block * block;
    if (max_seconds > 0 && (uint64_t)(max_seconds * per_second) / block * block < r->max_bytes)
        r->max_bytes = (uint64_t)(max_seconds * per_second) / block * block;

    char path[YK_REC_PATH_MAX];
    path_of(path, dir, id, ".wav.part");
    r->f = fopen(path, "w+b");
    if (!r->f) return -1;
    if (put_header(r->f, rate, channels, 0) || write_meta(dir, id, started, rate, channels, 0, "recording")) {
        fclose(r->f);
        r->f = NULL;
        remove(path);
        return -1;
    }
    return 0;
}

int yk_rec_write(yk_rec_t *r, const int16_t *samples, size_t count) {
    if (!r->f) return -1;
    uint64_t bytes = (uint64_t)count * 2, room = r->max_bytes - r->data_bytes;
    int full = 0;
    if (bytes > room) {
        bytes = room / (r->channels * 2u) * (r->channels * 2u);
        full = 1;
    }
    // Both the ESP32-S3 and the hosts the tests run on are little-endian, which is WAV's byte order.
    if (bytes && fwrite(samples, 1, (size_t)bytes, r->f) != bytes) return -1;
    r->data_bytes += bytes;
    if (r->data_bytes - r->synced_bytes >= r->sync_every) {
        if (put_header(r->f, r->rate, r->channels, r->data_bytes)) return -1;
        r->synced_bytes = r->data_bytes;
    }
    return full;
}

float yk_rec_seconds(const yk_rec_t *r) {
    return r->rate ? (float)((double)r->data_bytes / (r->rate * r->channels * 2.0)) : 0;
}

int yk_rec_close(yk_rec_t *r) {
    if (!r->f) return -1;
    int err = put_header(r->f, r->rate, r->channels, r->data_bytes);
    if (fclose(r->f)) err = -1;
    r->f = NULL;
    if (err || write_meta(r->dir, r->id, r->started, r->rate, r->channels, r->data_bytes, "stop")) return -1;
    char part[YK_REC_PATH_MAX], done[YK_REC_PATH_MAX];
    path_of(part, r->dir, r->id, ".wav.part");
    path_of(done, r->dir, r->id, ".wav");
    remove(done);  // FAT cannot rename over an existing file
    return rename(part, done) == 0 ? 0 : -1;
}

/** startedAt from an earlier "<id>.json", or 0. */
static int64_t started_from_meta(const char *dir, const char *id) {
    char path[YK_REC_PATH_MAX], text[256] = {0};
    path_of(path, dir, id, ".json");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    const char *at = strstr(text, "\"startedAt\":");
    long long started = 0;
    return at && sscanf(at + 12, "%lld", &started) == 1 ? (int64_t)started : 0;
}

static int recover_one(const char *dir, const char *id) {
    char part[YK_REC_PATH_MAX], done[YK_REC_PATH_MAX], meta[YK_REC_PATH_MAX];
    path_of(part, dir, id, ".wav.part");
    path_of(done, dir, id, ".wav");
    path_of(meta, dir, id, ".json");
    FILE *f = fopen(part, "r+b");
    if (!f) return -1;
    uint8_t h[HEADER];
    long size = -1;
    bool ok = fread(h, 1, HEADER, f) == HEADER && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) &&
              rd16(h + 22) > 0 && rd32(h + 24) > 0 && !fseek(f, 0, SEEK_END) && (size = ftell(f)) >= HEADER;
    if (!ok) {  // nothing worth keeping
        fclose(f);
        remove(part);
        remove(meta);
        return 0;
    }
    uint16_t channels = rd16(h + 22);
    uint32_t rate = rd32(h + 24);
    uint64_t block = channels * 2u, data = (uint64_t)(size - HEADER) / block * block;
    if (data > WAV_MAX_DATA / block * block) data = WAV_MAX_DATA / block * block;
    int err = put_header(f, rate, channels, data);
    if (fclose(f)) err = -1;
    if (err || write_meta(dir, id, started_from_meta(dir, id), rate, channels, data, "recovered")) return -1;
    remove(done);
    return rename(part, done) == 0 ? 1 : -1;
}

int yk_rec_recover(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    static const char suffix[] = ".wav.part";
    char pending[MAX_PENDING][YK_REC_ID_MAX];
    int count = 0;
    struct dirent *e;
    // Collect first: renaming while iterating a FAT directory can skip or repeat entries.
    while ((e = readdir(d)) && count < MAX_PENDING) {
        size_t n = strlen(e->d_name), s = sizeof(suffix) - 1;
        if (n > s && n - s < YK_REC_ID_MAX && !strcmp(e->d_name + n - s, suffix)) {
            memcpy(pending[count], e->d_name, n - s);
            pending[count++][n - s] = 0;
        }
    }
    closedir(d);
    int recovered = 0;
    for (int i = 0; i < count; i++)
        if (recover_one(dir, pending[i]) == 1) recovered++;
    return recovered;
}
