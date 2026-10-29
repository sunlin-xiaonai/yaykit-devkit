// App slots in the yaykit-v1 layout: the launcher in `factory`, apps in ota_0..ota_2.
#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_SLOT_COUNT 3

typedef struct {
    int index;             // 0..2
    bool present;          // holds a valid app image
    bool running;
    bool boot;             // selected for the next boot
    char state[16];        // "empty", "valid", "pending", "invalid", "aborted", "new", "unknown"
    char slug[32];         // from the image's project name: yaykit_party_play -> party-play
    char version[32];
} yk_slot_t;

/** Reads one slot's image header and OTA state. */
esp_err_t yk_slot_read(int index, yk_slot_t *out);
/** Adds every slot to a yk-info "slots" array. */
void yk_slots_to_json(cJSON *array);
/** Boots into an app slot (does not return on success). */
esp_err_t yk_slot_boot(int index);
/** Hands the device back to the launcher in `factory` (does not return on success). */
esp_err_t yk_return_to_launcher(void);
/** True when running from an app slot of the yaykit-v1 layout. */
bool yk_in_slot(void);
/** Called by an app once it has started correctly: cancels the pending rollback. */
esp_err_t yk_app_confirm(void);

#ifdef __cplusplus
}
#endif
