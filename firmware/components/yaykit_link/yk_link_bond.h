// Yaykit Link's long mode remembers one phone (docs/design/selfie.md 1 「换手机」): what to do with the keys and
// subscriptions NimBLE asks the store to keep. Pure C, host tested (test/run.sh); yk_link_keep.c applies it.
//
// NimBLE has no "a phone asks to pair" callback that could say no before pairing: by the time the store hears of a
// phone, it has paired. So a phone other than the remembered one is paired with but not kept: its keys wait here
// until the screen's answer (「换成这台手机？」). Yes: the old phone is forgotten, the new one kept. No (or the time
// is up, or it left): its keys are dropped. That phone still believes it paired; the next time it comes, its keys do
// not work here, and it is let go (YK_BOND_UNKNOWN in yk_link_keep.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A phone's identity address (NimBLE's ble_addr_t: type, then 6 bytes least significant first). */
typedef struct {
    uint8_t type;
    uint8_t val[6];
} yk_bond_addr_t;

typedef struct {
    bool have;              // a phone is remembered
    yk_bond_addr_t phone;   // which
    bool asking;            // 「换成这台手机？」 is up
    yk_bond_addr_t asked;   // for this phone
    bool repairing;         // the remembered phone itself pairs again (it forgot this device): asked as well
} yk_bond_t;

typedef enum {
    YK_BOND_KEEP,      // write it through: the remembered phone, or the first one (have is now true)
    YK_BOND_HOLD,      // keep it back until the answer (asking is true)
    YK_BOND_ASK,       // keep it back and ask now: a new question (asking became true with this write)
    YK_BOND_DROP,      // a third phone while asking about another: never kept
} yk_bond_action_t;

void yk_bond_init(yk_bond_t *b);
bool yk_bond_same(const yk_bond_addr_t *a, const yk_bond_addr_t *b);
/** NimBLE asks to keep something (keys or a subscription) of `peer`. `first` (may be NULL): set when this write made
 *  the first phone the remembered one. */
yk_bond_action_t yk_bond_write(yk_bond_t *b, const yk_bond_addr_t *peer, bool *first);
/** The remembered phone pairs again (NimBLE's repeat pairing: it forgot this device). Its old keys stay remembered
 *  until the answer; the new ones are asked about like another phone's. False when `peer` is not the remembered
 *  phone (nothing changes). */
bool yk_bond_repair(yk_bond_t *b, const yk_bond_addr_t *peer);
/** The answer. Yes: the asked phone is the one now. No: as before. False when nothing was asked. */
bool yk_bond_answer(yk_bond_t *b, bool yes);
/** Forget the remembered phone (the console's `keep forget`, or its keys were deleted). */
void yk_bond_forget(yk_bond_t *b);

#ifdef __cplusplus
}
#endif
