#include "yk_talk_msg.h"

#include <stdlib.h>
#include <string.h>

static char *dup(const char *s, void *(*alloc)(size_t), bool *ok) {
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    char *d = alloc(n);
    if (!d) {
        *ok = false;
        return NULL;
    }
    memcpy(d, s, n);
    return d;
}

bool yk_talk_msg_copy(yk_srv_msg_t *to, const yk_srv_msg_t *from, void *(*alloc)(size_t)) {
    if (!alloc) alloc = malloc;
    bool ok = true;
    *to = *from;
    to->params = to->url = to->signature = NULL;
    to->ids = NULL;
    to->id_count = 0;
    to->text = dup(from->text, alloc, &ok);
    to->said = dup(from->said, alloc, &ok);
    to->better = dup(from->better, alloc, &ok);
    to->note = dup(from->note, alloc, &ok);
    for (int i = 0; i < 2; i++) {
        to->words[i] = dup(from->words[i], alloc, &ok);
        to->meanings[i] = dup(from->meanings[i], alloc, &ok);
    }
    if (!ok) yk_proto_free(to);
    return ok;
}
