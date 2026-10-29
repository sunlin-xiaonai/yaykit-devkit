// Host tests for the long mode's one-phone rule (yk_link_bond.c): the cases the selfie lab's 实测 7 walked through.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../yk_link_bond.h"

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static yk_bond_addr_t addr(uint8_t last) { return (yk_bond_addr_t){.type = 0, .val = {last, 2, 3, 4, 5, 6}}; }

int main(void) {
    const yk_bond_addr_t a = addr(0xA1), b = addr(0xB2), c = addr(0xC3);
    yk_bond_t s;
    bool first;

    // The first phone is kept without asking; its later writes too.
    yk_bond_init(&s);
    CHECK(yk_bond_write(&s, &a, &first) == YK_BOND_KEEP && first && s.have && yk_bond_same(&s.phone, &a));
    CHECK(yk_bond_write(&s, &a, &first) == YK_BOND_KEEP && !first);

    // A second phone: held and asked once; its later writes held; a third one dropped.
    CHECK(yk_bond_write(&s, &b, &first) == YK_BOND_ASK && s.asking && yk_bond_same(&s.asked, &b));
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_HOLD);
    CHECK(yk_bond_write(&s, &c, NULL) == YK_BOND_DROP);
    // While asking, the remembered phone's own writes wait too (only the asked one is connected anyway).
    CHECK(yk_bond_write(&s, &a, NULL) == YK_BOND_DROP);

    // No: the first phone stays.
    CHECK(yk_bond_answer(&s, false) && !s.asking && yk_bond_same(&s.phone, &a));
    CHECK(!yk_bond_answer(&s, true));  // nothing asked
    CHECK(yk_bond_write(&s, &a, NULL) == YK_BOND_KEEP);

    // Asked again, yes: the second phone is the one, the first is a stranger now.
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_ASK);
    CHECK(yk_bond_answer(&s, true) && s.have && yk_bond_same(&s.phone, &b) && !s.asking);
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_KEEP);
    CHECK(yk_bond_write(&s, &a, NULL) == YK_BOND_ASK);
    CHECK(yk_bond_answer(&s, false));

    // The remembered phone forgot this device and pairs again: asked like a new one.
    CHECK(!yk_bond_repair(&s, &a));  // not the remembered phone: nothing
    CHECK(!s.repairing);
    CHECK(yk_bond_repair(&s, &b) && s.repairing);
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_ASK);
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_HOLD);
    CHECK(yk_bond_answer(&s, true) && !s.repairing && yk_bond_same(&s.phone, &b));
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_KEEP);
    // ... and refused: still the same phone, not repairing any more.
    CHECK(yk_bond_repair(&s, &b));
    CHECK(yk_bond_write(&s, &b, NULL) == YK_BOND_ASK);
    CHECK(yk_bond_answer(&s, false) && !s.repairing && s.have && yk_bond_same(&s.phone, &b));

    // Forgotten: the next phone is a first one again.
    yk_bond_forget(&s);
    CHECK(!s.have);
    CHECK(yk_bond_write(&s, &c, &first) == YK_BOND_KEEP && first);

    // The address type counts: same bytes, another type, another phone.
    yk_bond_addr_t c2 = c;
    c2.type = 1;
    CHECK(!yk_bond_same(&c, &c2));
    CHECK(yk_bond_write(&s, &c2, NULL) == YK_BOND_ASK);

    if (failures) {
        fprintf(stderr, "test_bond: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_bond: ok\n");
    return 0;
}
