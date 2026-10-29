// What an app leaves for Yay and the platform, in pure C (no ESP-IDF; host tested with yaykit_app's test/run.sh):
// the summary Yay draws when it is back, and the records waiting in the outbox. docs/design/extension-platform.md
// P3 and P19.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YK_APP_TIME_VALID_S 1767225600LL  // 2026-01-01: earlier means the clock was never set

/** The summary Yay draws when it is back from an app (docs/design/extension-platform.md P3): up to 3 numbers in
 *  the ring, up to 20 beads on it (some in coral), and whether the session was recorded. It goes in the result's
 *  data as "summary"; the main firmware knows no app's own fields. */
#define YK_SUMMARY_STATS 3
#define YK_SUMMARY_BEADS 20
#define YK_RESULT_FRESH_S (30 * 60)  // an older result is handed to the platform but not said

typedef struct {
    int stat_count;
    struct {
        char v[16];  // "8"
        char l[24];  // "轮"
    } stats[YK_SUMMARY_STATS];
    int beads;        // 0: none; 1..20
    uint32_t marked;  // bit b: bead b is coral
    int recorded;     // 1 recorded, 0 not, -1 not said
} yk_summary_t;

void yk_summary_init(yk_summary_t *s);
/** Adds a number with its label; false when there are 3 already (or no value). Cut to fit. */
bool yk_summary_stat(yk_summary_t *s, const char *value, const char *label);
/** The bead item i (0-based) of n lands on: i itself up to 20, floor(i * 20 / n) beyond. */
int yk_summary_bead(int i, int n);
/** n items (rounds, breaths...), of which marked[0..marked_count) are marked (0-based): min(n, 20) beads. */
void yk_summary_beads(yk_summary_t *s, int n, const int *marked, int marked_count);
/** The "summary" object: {"stats":[{"v":..,"l":..}],"beads":{"total":..,"marked":[..]},"recorded":..}, beads and
 *  recorded left out when not set. Returns the length, or -1 when it does not fit. */
int yk_summary_format(char *out, size_t size, const yk_summary_t *s);
/** Whether a result written at at_s may still be said at now_s (30 minutes). A clock not known at either end
 *  cannot tell: then it is said. */
bool yk_result_fresh(int64_t at_s, int64_t now_s);

/** The record outbox (P19): what an app wants to put in the platform's records, kept in the shared NVS (yk_sys)
 *  until someone online delivers it: the last YK_RECORD_SLOTS, a new one replacing the oldest. */
#define YK_RECORD_SLOTS 16
#define YK_RECORD_DATA_MAX 512

/** The records API's kind: ^[a-z][a-z0-9.]{2,40}$ ("breathe.session"). */
bool yk_record_kind_ok(const char *kind);
/** The NVS key of the outbox entry for seq: "rec0".."rec15". */
void yk_record_key(uint32_t seq, char out[8]);
/** One record as POST /records takes it, plus the app it is for: {"app":..,"id":"<device>:<app>:<seq>",
 *  "kind":..,"occurredAt":"2026-10-20T08:00:00.000Z","clockTrusted":..,"data":<object>}. The id lets a second
 *  delivery count once; the clock is trusted when at_ms is after 2026-01-01. Returns the length, or -1. */
int yk_record_format(char *out, size_t size, const char *app, const char *device_id, uint32_t seq, const char *kind,
                     const char *data_json, int64_t at_ms);

#ifdef __cplusplus
}
#endif
