// A copy of a server message for yaykit_talk's queue (practice.turn, hello.ok): the session frees its own as soon as
// on_message returns, and the app hears of it later, from the loop. Plain C over yk_proto, host tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "yk_proto.h"

/** Copies *from into *to with its own strings (the text, a practice.turn's said/better/note/words/meanings),
 *  allocated with `alloc` (NULL: malloc); an action's or a records batch's (params, url, signature, ids) are not
 *  taken. Free with yk_proto_free(). False when out of memory (*to is then freed already). */
bool yk_talk_msg_copy(yk_srv_msg_t *to, const yk_srv_msg_t *from, void *(*alloc)(size_t));
