// Host tests only (test_me.c).
#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, ...) (printf("W %s: ", tag), printf(__VA_ARGS__), printf("\n"))
