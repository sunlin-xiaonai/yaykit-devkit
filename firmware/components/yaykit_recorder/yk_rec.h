// Recordings on the TF card that survive a sudden power loss. Pure C over stdio and POSIX (host tested); on the
// device the TF card is a FAT volume behind the VFS, where fsync() commits the file size to the directory.
//
// While recording, a file is "<dir>/<id>.wav.part" with a valid header that is rewritten at every sync, so a
// power cut loses at most the last sync interval. A clean stop finalises the header, writes "<id>.json" and
// renames the file to "<id>.wav". At boot, yk_rec_recover() turns any leftover .part into a playable .wav.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define YK_REC_ID_MAX   32
#define YK_REC_PATH_MAX 128

typedef struct {
    FILE *f;
    char dir[YK_REC_PATH_MAX - YK_REC_ID_MAX - 12];
    char id[YK_REC_ID_MAX];
    uint32_t rate;
    uint16_t channels;
    int64_t started;          // Unix seconds, 0 when the clock is unknown
    uint64_t data_bytes;      // samples written
    uint64_t synced_bytes;    // samples committed to the card
    uint64_t sync_every;      // bytes between syncs
    uint64_t max_bytes;       // stop before the WAV size fields overflow, or earlier if the caller limits it
} yk_rec_t;

/** A file name that sorts by time: "20261011-153012-a1b2" (UTC) or "t0-a1b2c3d4" without a clock. */
void yk_rec_make_id(char out[YK_REC_ID_MAX], int64_t unix_seconds, uint32_t random);

/** Starts "<dir>/<id>.wav.part". sync_seconds: how much a power cut may lose. max_seconds: 0 = no limit. */
int yk_rec_open(yk_rec_t *r, const char *dir, const char *id, uint32_t rate, uint16_t channels,
                int64_t started, float sync_seconds, float max_seconds);
/** Appends interleaved 16-bit samples. Returns 0, or 1 when the length limit was reached (nothing more is
 *  written; the caller stops), or -1 on a write error. */
int yk_rec_write(yk_rec_t *r, const int16_t *samples, size_t count);
float yk_rec_seconds(const yk_rec_t *r);
/** Finalises: header, fsync, "<id>.json", rename to "<id>.wav". */
int yk_rec_close(yk_rec_t *r);

/** Repairs every "<id>.wav.part" in dir (size from the file length, rounded down to whole samples) and gives
 *  it its .json with "endedBy":"recovered". Files too short to hold a header are deleted. Returns how many
 *  were recovered, or -1 if dir cannot be read. */
int yk_rec_recover(const char *dir);
