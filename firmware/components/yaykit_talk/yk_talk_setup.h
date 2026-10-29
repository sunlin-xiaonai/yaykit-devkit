// The mode's setup the next hello carries (practice: the language and level; translate: the pair). yk_talk_start's
// config gives it and the app's choice (yk_talk_set) changes it, whenever that came: before yk_talk_start too, since
// the app read its config a moment earlier, and a language chosen in between would be lost (#509, 10-25: the screen
// in English, the hello in Japanese). Plain C, host tested (test/run.sh).
#pragma once

#include <stdbool.h>

typedef struct {
    char language[8], level[12], pair[8];
    bool chose_language, chose_level, chose_pair;  // the app's (yk_talk_set): yk_talk_start's config does not undo it
} yk_talk_setup_t;

/** The app's choice (yk_talk_set); NULL leaves that field as it is. */
void yk_talk_setup_choose(yk_talk_setup_t *s, const char *language, const char *level, const char *pair);
/** yk_talk_start's config: each field the app has not chosen already. */
void yk_talk_setup_start(yk_talk_setup_t *s, const char *language, const char *level, const char *pair);
