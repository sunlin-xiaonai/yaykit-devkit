#include "yk_mem.h"

#ifndef YK_MEM_HOST
#include "esp_heap_caps.h"

static volatile bool s_ready;
static size_t s_at_ready;  // the boot figure when the count started afresh

void yk_mem_ready(void) {
    if (s_ready) return;
    s_at_ready = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    // ESP_FAIL: something counts already (a development command): that count is the one read.
    heap_caps_monitor_local_minimum_free_size_start();
    s_ready = true;
}

size_t yk_mem_min_since_ready(void) { return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); }

size_t yk_mem_min_ever(void) { return yk_mem_ever(s_ready, s_at_ready, yk_mem_min_since_ready()); }
#endif

size_t yk_mem_ever(bool ready, size_t at_ready, size_t since_ready) {
    return ready && at_ready < since_ready ? at_ready : since_ready;
}
