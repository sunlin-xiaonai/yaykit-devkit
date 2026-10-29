#include "yaykit_slots.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

static const char *TAG = "yk_slots";

static const esp_partition_t *slot_partition(int index) {
    if (index < 0 || index >= YK_SLOT_COUNT) return NULL;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + index, NULL);
}

static const char *state_name(esp_ota_img_states_t state) {
    switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "unknown";
    }
}

esp_err_t yk_slot_read(int index, yk_slot_t *out) {
    const esp_partition_t *part = slot_partition(index);
    if (!part || !out) return ESP_ERR_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    out->index = index;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    out->running = running && running->address == part->address;
    out->boot = boot && boot->address == part->address;
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        strcpy(out->state, "empty");
        return ESP_OK;
    }
    out->present = true;
    // Project names are yaykit_<slug with underscores>.
    const char *name = strncmp(desc.project_name, "yaykit_", 7) ? desc.project_name : desc.project_name + 7;
    snprintf(out->slug, sizeof(out->slug), "%.31s", name);
    for (char *c = out->slug; *c; c++)
        if (*c == '_') *c = '-';
    snprintf(out->version, sizeof(out->version), "%.31s", desc.version);
    esp_ota_img_states_t state;
    snprintf(out->state, sizeof(out->state), "%s",
             esp_ota_get_state_partition(part, &state) == ESP_OK ? state_name(state) : "valid");
    return ESP_OK;
}

void yk_slots_to_json(cJSON *array) {
    for (int i = 0; i < YK_SLOT_COUNT; i++) {
        yk_slot_t slot;
        if (yk_slot_read(i, &slot) != ESP_OK) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "state", slot.state);
        if (slot.present) {
            cJSON_AddStringToObject(item, "slug", slot.slug);
            cJSON_AddStringToObject(item, "version", slot.version);
        }
        cJSON_AddBoolToObject(item, "running", slot.running);
        cJSON_AddBoolToObject(item, "boot", slot.boot);
        cJSON_AddItemToArray(array, item);
    }
}

esp_err_t yk_slot_boot(int index) {
    yk_slot_t slot;
    if (yk_slot_read(index, &slot) != ESP_OK || !slot.present) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_ota_set_boot_partition(slot_partition(index));
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "booting slot %d (%s %s)", index, slot.slug, slot.version);
    esp_restart();
    return ESP_OK;
}

esp_err_t yk_return_to_launcher(void) {
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "returning to the launcher");
    esp_restart();
    return ESP_OK;
}

bool yk_in_slot(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN + YK_SLOT_COUNT;
}

esp_err_t yk_app_confirm(void) {
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "self-test passed; keeping this app");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}
