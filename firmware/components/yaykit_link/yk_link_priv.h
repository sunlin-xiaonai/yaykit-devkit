// Inside yaykit_link: what its files share with each other (not for apps).
#pragma once

#include <stdbool.h>

/** The long mode (yk_link_keep.c) runs NimBLE: the provisioning link must not start meanwhile. A flag here rather
 *  than a call, so firmware without the long mode never links yk_link_keep.c. */
extern volatile bool yk_link_keep_running;
