// Host tests only: the LVGL names yaykit_ui's headers mention (the pure parts never call LVGL).
#pragma once
#include <stdint.h>
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_layer_t lv_layer_t;
typedef struct _lv_event_t lv_event_t;
typedef struct _lv_image_dsc_t lv_image_dsc_t;
typedef void (*lv_event_cb_t)(lv_event_t *e);
