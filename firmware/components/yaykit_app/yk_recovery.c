// The recovery's version on the device (yaykit_app.h, yk_app_recovery.h): its header and description at start, its
// image's sha256 only for a recovery the table does not know.
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "yaykit_app.h"

#define CHUNK 4096  // bytes read and hashed at a time, with a 1 ms pause after each

static const esp_partition_t *factory(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

esp_err_t yk_app_recovery_read(yk_recovery_t *out) {
    memset(out, 0, sizeof(*out));
    const esp_partition_t *p = factory();
    if (!p) return ESP_ERR_NOT_FOUND;
    const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    esp_image_metadata_t meta;
    esp_app_desc_t desc;
    esp_err_t err = esp_image_get_metadata(&pos, &meta);  // the header and the segments' headers: a few ms
    if (err == ESP_OK) err = esp_ota_get_partition_description(p, &desc);
    if (err != ESP_OK) return err;
    yk_recovery_identify(desc.app_elf_sha256, desc.version, meta.image_len, out);
    return ESP_OK;
}

/** Hashes `len` bytes of `p` from `at` into `ctx`, CHUNK at a time through `buf`, a 1 ms pause after each. */
static esp_err_t hash_range(const esp_partition_t *p, uint32_t at, uint32_t len, mbedtls_sha256_context *ctx,
                            uint8_t *buf) {
    esp_err_t err = ESP_OK;
    for (uint32_t done = 0; err == ESP_OK && done < len; done += CHUNK) {
        const uint32_t n = MIN(CHUNK, len - done);
        err = esp_partition_read(p, at + done, buf, n);  // the other core waits only for these 4 KB
        if (err == ESP_OK && mbedtls_sha256_update(ctx, buf, n)) err = ESP_FAIL;
        vTaskDelay(1);  // both IDLE tasks get their turn: a copy without pauses starved IDLE0 into task_wdt (#555)
    }
    return err;
}

static void to_hex(const uint8_t digest[32], char out[65]) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = HEX[digest[i] >> 4];
        out[2 * i + 1] = HEX[digest[i] & 15];
    }
    out[64] = 0;
}

static uint8_t *chunk_buffer(void) {
    return heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);  // the SHA peripheral reads it
}

esp_err_t yk_app_recovery_hash(uint32_t image_len, char sha256[65]) {
    const esp_partition_t *p = factory();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (!image_len || image_len > p->size) return ESP_ERR_INVALID_SIZE;
    uint8_t *buf = chunk_buffer();
    if (!buf) return ESP_ERR_NO_MEM;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    esp_err_t err = mbedtls_sha256_starts(&ctx, 0) ? ESP_FAIL : ESP_OK;
    if (err == ESP_OK) err = hash_range(p, 0, image_len, &ctx, buf);
    uint8_t digest[32];
    if (err == ESP_OK && mbedtls_sha256_finish(&ctx, digest)) err = ESP_FAIL;
    mbedtls_sha256_free(&ctx);
    heap_caps_free(buf);
    if (err != ESP_OK) return err;
    to_hex(digest, sha256);
    return ESP_OK;
}

esp_err_t yk_app_recovery_verify(const yk_recovery_t *r, char sha256[65]) {
    sha256[0] = 0;
    const esp_partition_t *p = factory();
    if (!p) return ESP_ERR_NOT_FOUND;
    const uint32_t len = r->size;
    if (len <= sizeof(esp_image_header_t) + 32 || len > p->size) return ESP_ERR_INVALID_SIZE;
    esp_image_header_t head;
    esp_err_t err = esp_partition_read(p, 0, &head, sizeof(head));
    if (err != ESP_OK) return err;
    if (head.magic != ESP_IMAGE_HEADER_MAGIC || !head.hash_appended) return ESP_ERR_INVALID_CRC;  // nothing to match
    uint8_t *buf = chunk_buffer();
    if (!buf) return ESP_ERR_NO_MEM;
    // One pass: the image up to its appended SHA-256 (what that SHA covers), then a copy of that state takes the
    // appended 32 bytes too for the whole image's sha256 (the package .bin's, the table's).
    mbedtls_sha256_context body, whole;
    mbedtls_sha256_init(&body);
    mbedtls_sha256_init(&whole);
    uint8_t tail[32], digest[32], digest_whole[32];
    err = mbedtls_sha256_starts(&body, 0) ? ESP_FAIL : ESP_OK;
    if (err == ESP_OK) err = hash_range(p, 0, len - 32, &body, buf);
    if (err == ESP_OK) err = esp_partition_read(p, len - 32, tail, sizeof(tail));
    if (err == ESP_OK) {
        mbedtls_sha256_clone(&whole, &body);
        if (mbedtls_sha256_update(&whole, tail, sizeof(tail)) || mbedtls_sha256_finish(&whole, digest_whole) ||
            mbedtls_sha256_finish(&body, digest))
            err = ESP_FAIL;
    }
    mbedtls_sha256_free(&body);
    mbedtls_sha256_free(&whole);
    heap_caps_free(buf);
    if (err != ESP_OK) return err;
    to_hex(digest_whole, sha256);
    if (memcmp(digest, tail, sizeof(tail))) return ESP_ERR_INVALID_CRC;
    // Known (the table, NVS, hashed before): the whole image must be that one too, so a hash once taken of a bad image
    // and kept in NVS would not pass on the appended SHA alone. Not known: the appended SHA is all there is.
    if (r->sha256[0] && strcmp(r->sha256, sha256)) return ESP_ERR_INVALID_CRC;
    return ESP_OK;
}
