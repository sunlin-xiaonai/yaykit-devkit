// One phone remembered (yk_link_bond.h). Pure C.
#include "yk_link_bond.h"

#include <string.h>

void yk_bond_init(yk_bond_t *b) { memset(b, 0, sizeof(*b)); }

bool yk_bond_same(const yk_bond_addr_t *a, const yk_bond_addr_t *b) {
    return a->type == b->type && !memcmp(a->val, b->val, sizeof(a->val));
}

yk_bond_action_t yk_bond_write(yk_bond_t *b, const yk_bond_addr_t *peer, bool *first) {
    if (first) *first = false;
    if (b->asking) return yk_bond_same(peer, &b->asked) ? YK_BOND_HOLD : YK_BOND_DROP;
    if (!b->have) {
        b->have = true;
        b->phone = *peer;
        if (first) *first = true;
        return YK_BOND_KEEP;
    }
    if (yk_bond_same(peer, &b->phone) && !b->repairing) return YK_BOND_KEEP;
    b->asking = true;
    b->asked = *peer;
    return YK_BOND_ASK;
}

bool yk_bond_repair(yk_bond_t *b, const yk_bond_addr_t *peer) {
    if (!b->have || !yk_bond_same(peer, &b->phone)) return false;
    b->repairing = true;
    return true;
}

bool yk_bond_answer(yk_bond_t *b, bool yes) {
    if (!b->asking) return false;
    if (yes) {
        b->phone = b->asked;
        b->have = true;
    }
    b->asking = false;
    b->repairing = false;
    memset(&b->asked, 0, sizeof(b->asked));
    return true;
}

void yk_bond_forget(yk_bond_t *b) { yk_bond_init(b); }
