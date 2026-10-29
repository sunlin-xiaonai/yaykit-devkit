// The next hello's setup (yk_talk_setup.h).
#include "yk_talk_setup.h"

#include <stddef.h>

static void put(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (; in && in[n] && n + 1 < size; n++) out[n] = in[n];
    out[n] = 0;
}

void yk_talk_setup_choose(yk_talk_setup_t *s, const char *language, const char *level, const char *pair) {
    if (language) put(s->language, sizeof(s->language), language), s->chose_language = true;
    if (level) put(s->level, sizeof(s->level), level), s->chose_level = true;
    if (pair) put(s->pair, sizeof(s->pair), pair), s->chose_pair = true;
}

void yk_talk_setup_start(yk_talk_setup_t *s, const char *language, const char *level, const char *pair) {
    if (!s->chose_language) put(s->language, sizeof(s->language), language);
    if (!s->chose_level) put(s->level, sizeof(s->level), level);
    if (!s->chose_pair) put(s->pair, sizeof(s->pair), pair);
}
