// Host tests for the app ring, the app catalog and the hint arrow (yk_apps_logic.c). Run: bash test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yk_apps_logic.h"
#include "yk_round.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

/** Like the 16 px font: CJK 16, ASCII 9 (wide, to be safe), the ellipsis 16. */
static int width16(uint32_t cp, void *ctx) {
    (void)ctx;
    return cp < 0x80 ? 9 : 16;
}

static bool corner_in(int x, int y, int r) {
    const double dx = x - YK_CX, dy = y - YK_CY;
    return dx * dx + dy * dy <= (double)r * r;
}

static bool box_in(const yk_box_t *b, int r) {
    return corner_in(b->x, b->y, r) && corner_in(b->x + b->w, b->y, r) && corner_in(b->x, b->y + b->h, r) &&
           corner_in(b->x + b->w, b->y + b->h, r);
}

static bool meet(const yk_box_t *a, const yk_box_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

/** Whether a disc and a box share a pixel (the box's closest point to the centre is inside the disc). */
static bool disc_meets(int cx, int cy, int r, const yk_box_t *b) {
    const int x = cx < b->x ? b->x : cx > b->x + b->w ? b->x + b->w : cx;
    const int y = cy < b->y ? b->y : cy > b->y + b->h ? b->y + b->h : cy;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r;
}

static void test_slots(void) {
    int s[YK_APPS_MAX];
    yk_apps_slots(5, s);
    for (int i = 0; i < 5; i++) CHECK(s[i] == i);
    yk_apps_slots(1, s);
    CHECK(s[0] == 0 && s[1] == -1);
    yk_apps_slots(2, s);
    CHECK(s[0] == 1 && s[1] == 4 && s[2] == -1);  // upper right, upper left
    yk_apps_slots(3, s);
    CHECK(s[0] == 0 && s[1] == 2 && s[2] == 3);   // top, lower right, lower left
    yk_apps_slots(4, s);
    CHECK(s[0] == 1 && s[1] == 2 && s[2] == 3 && s[3] == 4);
    yk_apps_slots(0, s);
    CHECK(s[0] == -1);
    yk_apps_slots(9, s);
    CHECK(s[4] == 4);
    // Symmetric about the vertical for every count: the slots' x offsets sum to 0.
    for (int n = 1; n <= 5; n++) {
        yk_apps_layout_t L;
        yk_apps_layout(n, NULL, 24, 60, &L);
        int sum = 0;
        for (int i = 0; i < n; i++) sum += L.sat[i].cx - YK_CX;
        CHECK(abs(sum) <= 1);
    }
}

static void test_layout(void) {
    yk_apps_layout_t L;
    // Five apps, 12 o'clock first, every 72 degrees at r 142.
    const int w5[5] = {64, 48, 80, 64, 80};
    yk_apps_layout(5, w5, 24, 61, &L);
    CHECK(L.n == 5);
    CHECK(L.sat[0].cx == 233 && L.sat[0].cy == 91);
    CHECK(L.sat[1].cx == 233 + 135 && L.sat[1].cy == 233 - 44);
    CHECK(L.sat[2].cx == 233 + 83 && L.sat[2].cy == 233 + 115);
    CHECK(L.sat[3].cx == 233 - 83 && L.sat[4].cx == 233 - 135);
    for (int i = 0; i < 5; i++) {
        const double dx = L.sat[i].cx - YK_CX, dy = L.sat[i].cy - YK_CY;
        CHECK(fabs(sqrt(dx * dx + dy * dy) - YK_APPS_R) < 1.0);
    }
    // Every name under its disc; the lower two moved towards the middle to stay in the circle, the others centred.
    for (int i = 0; i < 5; i++) CHECK(L.sat[i].name.y == L.sat[i].cy + YK_APPS_SAT_R + YK_APPS_NAME_GAP);
    CHECK(L.sat[0].name_shift == 0 && L.sat[1].name_shift == 0 && L.sat[4].name_shift == 0);
    CHECK(L.sat[0].name.x + L.sat[0].name.w / 2 == L.sat[0].cx);
    CHECK(L.sat[2].name_shift > 10 && L.sat[2].name_shift < 40 && L.sat[2].name.x + L.sat[2].name.w / 2 < L.sat[2].cx);
    CHECK(L.sat[3].name_shift > 10 && L.sat[3].name_shift < 40 && L.sat[3].name.x + L.sat[3].name.w / 2 > L.sat[3].cx);
    // ... still mostly under it: its middle within the disc.
    CHECK(L.sat[2].cx - (L.sat[2].name.x + L.sat[2].name.w / 2) < YK_APPS_SAT_R);

    // Every count, the widest names and labels, two line heights: every text inside the content circle (r 200),
    // nothing overlapping, every disc (with the in-use ring) inside too.
    const int name_ws[] = {16, 48, 80, 96, 140};
    const int label_ws[] = {28, 61, 96, 130};
    const int hs[] = {22, 24};
    int combos = 0;
    for (int n = 0; n <= 5; n++)
        for (size_t a = 0; a < sizeof(name_ws) / sizeof(name_ws[0]); a++)
            for (size_t b = 0; b < sizeof(label_ws) / sizeof(label_ws[0]); b++)
                for (size_t c = 0; c < 2; c++) {
                    int w[5];
                    for (int i = 0; i < 5; i++) w[i] = name_ws[(a + i) % 5];
                    yk_apps_layout(n, w, hs[c], label_ws[b], &L);
                    bool ok = box_in(&L.label, YK_R_CONTENT) && L.label.w <= YK_APPS_LABEL_MAX_W;
                    // what must not meet: the texts (the label, every name), the discs (the middle's, every app's,
                    // with the in-use ring around them)
                    yk_box_t texts[1 + YK_APPS_MAX];
                    int nt = 0, discs[1 + YK_APPS_MAX][3], nd = 0;
                    texts[nt++] = L.label;
                    discs[nd][0] = YK_CX, discs[nd][1] = YK_CY, discs[nd++][2] = YK_APPS_CENTER_R + YK_APPS_RING_W;
                    for (int i = 0; i < L.n; i++) {
                        // the glyphs (the line box less its empty top and bottom) in the content circle
                        const yk_box_t *nb = &L.sat[i].name;
                        const yk_box_t ink = {nb->x, (int16_t)(nb->y + YK_APPS_INK_TOP), nb->w,
                                              (int16_t)(nb->h - YK_APPS_INK_TOP - YK_APPS_INK_BOTTOM)};
                        ok = ok && box_in(&ink, YK_R_CONTENT) && nb->w <= yk_apps_name_room(L.sat[i].slot, hs[c]);
                        ok = ok && nb->x + nb->w / 2 - L.sat[i].cx < YK_APPS_SAT_R && L.sat[i].cx - (nb->x + nb->w / 2) < YK_APPS_SAT_R;
                        ok = ok && corner_in(L.sat[i].cx + YK_APPS_SAT_R + YK_APPS_RING_W, L.sat[i].cy, YK_R_CONTENT);
                        texts[nt++] = L.sat[i].name;
                        discs[nd][0] = L.sat[i].cx, discs[nd][1] = L.sat[i].cy, discs[nd++][2] = YK_APPS_SAT_R + YK_APPS_RING_W;
                    }
                    for (int i = 0; i < nt; i++) {
                        for (int j = i + 1; j < nt; j++)
                            if (meet(&texts[i], &texts[j])) ok = false, printf("  texts %d/%d meet (n=%d w=%d label=%d)\n", i, j, n, name_ws[a], label_ws[b]);
                        for (int d = 0; d < nd; d++)
                            if (disc_meets(discs[d][0], discs[d][1], discs[d][2], &texts[i]))
                                ok = false, printf("  text %d meets disc %d (n=%d w=%d label=%d)\n", i, d, n, name_ws[a], label_ws[b]);
                    }
                    for (int d = 0; d < nd; d++)
                        for (int e = d + 1; e < nd; e++) {
                            const int dx = discs[d][0] - discs[e][0], dy = discs[d][1] - discs[e][1], r = discs[d][2] + discs[e][2];
                            if (dx * dx + dy * dy < r * r) ok = false;
                        }
                    CHECK(ok);
                    combos++;
                }
    CHECK(combos == 6 * 5 * 4 * 2);

    // How wide a name may be: 96 at the top and the sides, less for the lower two (they share the bottom).
    CHECK(yk_apps_name_room(0, 23) == YK_APPS_NAME_MAX_W && yk_apps_name_room(1, 23) == YK_APPS_NAME_MAX_W);
    CHECK(yk_apps_name_room(4, 24) == YK_APPS_NAME_MAX_W);
    CHECK(yk_apps_name_room(2, 23) >= 80 && yk_apps_name_room(2, 23) < YK_APPS_NAME_MAX_W);  // five Han characters fit
    CHECK(yk_apps_name_room(2, 24) == yk_apps_name_room(3, 24));
    // The widest names: the lower two move further in, and still keep apart.
    const int wide[5] = {96, 96, 96, 96, 96};
    yk_apps_layout(5, wide, 24, 96, &L);
    CHECK(L.sat[0].name.x + L.sat[0].name.w / 2 == L.sat[0].cx);
    CHECK(L.sat[3].name.x + L.sat[3].name.w + YK_APPS_NAME_APART <= L.sat[2].name.x);
    CHECK(L.sat[2].name.w == yk_apps_name_room(2, 24));
}

static void test_motion(void) {
    const int w5[5] = {64, 64, 64, 64, 64};
    yk_apps_layout_t L;
    yk_apps_layout(5, w5, 24, 61, &L);
    yk_apps_pose_t p[YK_APPS_MAX + 1];
    // Start: the middle at 0.6 and clear; every app at the centre, 0.2, clear.
    CHECK(!yk_apps_frame(&L, 0, false, p));
    CHECK(p[0].scale == lroundf(256 * 0.6f) && p[0].opa == 0 && p[0].cx == YK_CX);
    for (int i = 1; i <= 5; i++) CHECK(p[i].cx == YK_CX && p[i].cy == YK_CY && p[i].scale == lroundf(256 * 0.2f) && p[i].opa == 0);
    // One after another: at 100 ms the first has gone further than the third, the fifth has not left yet.
    yk_apps_frame(&L, 100, false, p);
    const int d1 = abs(p[1].cy - YK_CY) + abs(p[1].cx - YK_CX), d3 = abs(p[3].cy - YK_CY) + abs(p[3].cx - YK_CX);
    CHECK(d1 > d3 && d3 > 0);
    CHECK(p[5].cx == YK_CX && p[5].cy == YK_CY && p[5].opa == 0);
    // The middle is grown at 240 ms.
    yk_apps_frame(&L, YK_APPS_CENTER_IN_MS, false, p);
    CHECK(p[0].scale == 256 && p[0].opa == 255);
    // Five in place before 300 ms (each flies 160 ms since the ring has 6 places).
    CHECK(yk_apps_frame(&L, YK_APPS_OPEN_MS, false, p));
    for (int i = 1; i <= 5; i++) CHECK(p[i].cx == L.sat[i - 1].cx && p[i].cy == L.sat[i - 1].cy && p[i].scale == 256 && p[i].opa == 255);
    CHECK(YK_APPS_FLY_MS == 160);
    // A full ring (6 places with 「设置」): everything in place by 300 ms, not before.
    {
        const yk_apps_item_t six[6] = {{.slug = "a", .installed = true}, {.slug = "b", .installed = true},
                                       {.slug = "c", .installed = true}, {.slug = "d", .installed = true},
                                       {.slug = YK_APPS_MORE}, {.slug = YK_APPS_SETTINGS}};
        int slots[YK_APPS_MAX];
        yk_apps_item_slots(six, 6, slots);
        yk_apps_layout_at(6, slots, w5, 24, 61, &L);
        CHECK(!yk_apps_frame(&L, YK_APPS_OPEN_MS - 1, false, p));
        CHECK(yk_apps_frame(&L, YK_APPS_OPEN_MS, false, p));
        for (int i = 1; i <= 6; i++) CHECK(p[i].cx == L.sat[i - 1].cx && p[i].cy == L.sat[i - 1].cy && p[i].opa == 255);
    }
    // Fewer apps: done sooner, never later than 300.
    yk_apps_layout(2, w5, 24, 61, &L);
    CHECK(yk_apps_frame(&L, YK_APPS_CENTER_IN_MS, false, p));
    // Closing: 180 ms back into the centre, gone.
    yk_apps_layout(5, w5, 24, 61, &L);
    CHECK(!yk_apps_frame(&L, 0, true, p));
    CHECK(p[1].cx == L.sat[0].cx && p[1].opa == 255 && p[0].scale == 256);
    CHECK(!yk_apps_frame(&L, 90, true, p));
    CHECK(p[1].opa > 0 && p[1].opa < 255);
    CHECK(yk_apps_frame(&L, YK_APPS_CLOSE_MS, true, p));
    for (int i = 1; i <= 5; i++) CHECK(p[i].cx == YK_CX && p[i].cy == YK_CY && p[i].opa == 0);
    CHECK(p[0].opa == 0 && p[0].scale == lroundf(256 * 0.8f));
}

static void test_hit(void) {
    const int w5[5] = {64, 64, 80, 64, 64};
    yk_apps_layout_t L;
    yk_apps_layout(5, w5, 24, 61, &L);
    CHECK(yk_apps_hit(&L, YK_CX, YK_CY) == 0);
    CHECK(yk_apps_hit(&L, YK_CX, YK_CY + 70) == 0);  // the label
    for (int i = 0; i < 5; i++) {
        CHECK(yk_apps_hit(&L, L.sat[i].cx, L.sat[i].cy) == i + 1);
        CHECK(yk_apps_hit(&L, L.sat[i].cx + 40, L.sat[i].cy + 20) == i + 1);  // the 96-wide target, past the disc
        CHECK(yk_apps_hit(&L, L.sat[i].name.x + 2, L.sat[i].name.y + 2) == i + 1);
    }
    CHECK(yk_apps_hit(&L, YK_CX, YK_CY + 150) == -1);  // between the lower two
    CHECK(yk_apps_hit(&L, 20, 20) == -1);
    CHECK(yk_apps_hit(&L, YK_CX + 100, YK_CY - 120) == -1);  // between the top and the upper right
    // No touch hits two things: the targets never overlap (centre first, every pixel).
    for (int y = 0; y < 466; y += 2)
        for (int x = 0; x < 466; x += 2) {
            int hits = 0;
            for (int i = 0; i < L.n; i++) {
                const int dx = x - L.sat[i].cx, dy = y - L.sat[i].cy;
                hits += dx * dx + dy * dy <= YK_APPS_HIT_R * YK_APPS_HIT_R;
            }
            const int dx = x - YK_CX, dy = y - YK_CY;
            hits += dx * dx + dy * dy <= YK_APPS_CENTER_HIT_R * YK_APPS_CENTER_HIT_R;
            if (hits > 1) {
                CHECK(hits <= 1);
                y = 466;
                break;
            }
        }
}

static yk_catalog_t demo_catalog(void) {
    yk_catalog_t c = {.count = 6, .written_ms = 1792549233000LL, .lang = "zh", .seq = 7};
    const char *slugs[] = {"language-buddy", "breathe", "party-play", "homework-buddy", "translator", "yay"};
    const char *names[] = {"口语陪练", "呼吸之间", "聚会玩", "作业伙伴", "面对面翻译", "Yay"};
    for (int i = 0; i < c.count; i++) {
        strcpy(c.app[i].slug, slugs[i]);
        strcpy(c.app[i].name, names[i]);
        strcpy(c.app[i].version, "1.0.0");
        c.app[i].size = 2800000 + i;
    }
    return c;
}

static void test_pick(void) {
    yk_catalog_t cat = demo_catalog();
    yk_apps_item_t out[YK_APPS_MAX];
    // Installed: Party Play used before Breathe; Yay and an empty slot are skipped.
    const yk_apps_slot_t inst[] = {{"party-play", 100}, {"yay", 999}, {"breathe", 200}, {"", 300}, {NULL, 0}};
    int n = yk_apps_pick(inst, 5, &cat, out, YK_APPS_MAX);
    CHECK(n == 5);
    CHECK(!strcmp(out[0].slug, "breathe") && out[0].installed && !strcmp(out[0].name, "呼吸之间"));
    CHECK(!strcmp(out[1].slug, "party-play") && out[1].installed);
    CHECK(!strcmp(out[2].slug, "language-buddy") && !out[2].installed && !strcmp(out[2].name, "口语陪练"));
    CHECK(!strcmp(out[3].slug, "homework-buddy") && !strcmp(out[4].slug, "translator"));
    for (int i = 0; i < n; i++) CHECK(strcmp(out[i].slug, "yay") && !out[i].icon);
    // No catalog (never online): only what is installed, names left to the ring.
    n = yk_apps_pick(inst, 5, NULL, out, YK_APPS_MAX);
    CHECK(n == 2 && !strcmp(out[0].slug, "breathe") && out[0].name == NULL && !strcmp(out[1].slug, "party-play"));
    // A limit, and the same app twice in the slots (an old copy) counted once.
    const yk_apps_slot_t twice[] = {{"breathe", 5}, {"breathe", 9}};
    n = yk_apps_pick(twice, 2, &cat, out, 3);
    CHECK(n == 3 && !strcmp(out[0].slug, "breathe") && !strcmp(out[1].slug, "language-buddy") && !strcmp(out[2].slug, "party-play"));
    CHECK(yk_apps_pick(NULL, 0, NULL, out, 5) == 0);
    CHECK(yk_catalog_find(&cat, "party-play") == &cat.app[2] && !yk_catalog_find(&cat, "nope") && !yk_catalog_find(NULL, "x"));
}

/** The ring with 「更多」 (app-ecosystem.md 4.5): at most 4 apps, those in a slot, then those in the package cache,
 *  then the catalog's; 「更多」, always, then 「设置」 (settings-u.md 5) last: the 6-place ring. */
static void test_pick_ring(void) {
    yk_catalog_t cat = demo_catalog();
    yk_apps_item_t out[YK_APPS_MAX];
    const yk_apps_slot_t inst[] = {{"party-play", 100}, {"yay", 999}, {"breathe", 200}};
    const char *const cached[] = {"homework-buddy", "breathe", "yay", "+more", ""};
    int n = yk_apps_pick_ring(inst, 3, cached, 5, &cat, out, YK_APPS_MAX);
    CHECK(n == 6);
    CHECK(!strcmp(out[0].slug, "breathe") && out[0].installed && !out[0].cached && !strcmp(out[0].name, "呼吸之间"));
    CHECK(!strcmp(out[1].slug, "party-play") && out[1].installed);
    CHECK(!strcmp(out[2].slug, "homework-buddy") && !out[2].installed && out[2].cached && out[2].name);
    CHECK(!strcmp(out[3].slug, "language-buddy") && !out[3].installed && !out[3].cached);  // the catalog's first
    CHECK(!strcmp(out[4].slug, YK_APPS_MORE) && !out[4].installed && !out[4].cached && !out[4].name);
    CHECK(!strcmp(out[5].slug, YK_APPS_SETTINGS) && !out[5].installed && !out[5].cached && !out[5].name);
    // Nothing anywhere (a new device, never online): 「更多」 and 「设置」.
    n = yk_apps_pick_ring(NULL, 0, NULL, 0, NULL, out, YK_APPS_MAX);
    CHECK(n == 2 && !strcmp(out[0].slug, YK_APPS_MORE) && !strcmp(out[1].slug, YK_APPS_SETTINGS));
    // Only what is on the device, no catalog: names left to the ring.
    n = yk_apps_pick_ring(inst, 3, cached, 2, NULL, out, YK_APPS_MAX);
    CHECK(n == 5 && !strcmp(out[2].slug, "homework-buddy") && out[2].cached && !out[2].name && !strcmp(out[3].slug, YK_APPS_MORE));
    // A limit counts 「更多」 and 「设置」; 「更多」 alone when only one fits.
    n = yk_apps_pick_ring(inst, 3, cached, 5, &cat, out, 3);
    CHECK(n == 3 && !strcmp(out[0].slug, "breathe") && !strcmp(out[1].slug, YK_APPS_MORE) && !strcmp(out[2].slug, YK_APPS_SETTINGS));
    n = yk_apps_pick_ring(inst, 3, cached, 5, &cat, out, 1);
    CHECK(n == 1 && !strcmp(out[0].slug, YK_APPS_MORE));
    CHECK(yk_apps_pick_ring(inst, 3, NULL, 0, NULL, out, 0) == 0);
    // Places: the 6-place ring, the apps from the top, 「更多」 lower left, 「设置」 upper left; rings without 「设置」 as
    // before.
    int slots[YK_APPS_MAX];
    n = yk_apps_pick_ring(inst, 3, cached, 5, &cat, out, YK_APPS_MAX);
    yk_apps_item_slots(out, n, slots);
    CHECK(slots[0] == YK_APPS_SIX && slots[1] == YK_APPS_SIX + 1 && slots[2] == YK_APPS_SIX + 2 && slots[3] == YK_APPS_SIX + 3);
    CHECK(slots[4] == YK_APPS_MORE_PLACE6 && slots[5] == YK_APPS_SETTINGS_PLACE);
    yk_apps_item_slots(out, 2, slots);  // the first two alone: no 「更多」 among them
    CHECK(slots[0] == 1 && slots[1] == 4);
    const yk_apps_item_t two[] = {{.slug = "breathe", .installed = true}, {.slug = YK_APPS_MORE}};
    yk_apps_item_slots(two, 2, slots);
    CHECK(slots[0] == 0 && slots[1] == YK_APPS_MORE_PLACE && slots[2] == -1);
    // Laid out there: 「更多」 upper left, every name in the content circle.
    for (int k = 1; k <= 5; k++) {
        yk_apps_item_t items[YK_APPS_MAX];
        for (int i = 0; i < k - 1; i++) items[i] = (yk_apps_item_t){.slug = "breathe", .installed = true};
        items[k - 1] = (yk_apps_item_t){.slug = YK_APPS_MORE};
        yk_apps_item_slots(items, k, slots);
        int name_w[YK_APPS_MAX];
        for (int i = 0; i < k; i++) name_w[i] = yk_apps_name_room(slots[i], 22);
        yk_apps_layout_t L;
        yk_apps_layout_at(k, slots, name_w, 22, 30, &L);
        const yk_apps_sat_t *m = &L.sat[k - 1];
        CHECK(L.n == k && m->slot == YK_APPS_MORE_PLACE && m->cx < YK_CX - 100 && m->cy < YK_CY);
        for (int i = 0; i < k; i++) {
            const yk_box_t ink = {L.sat[i].name.x, (int16_t)(L.sat[i].name.y + YK_APPS_INK_TOP), L.sat[i].name.w,
                                  (int16_t)(L.sat[i].name.h - YK_APPS_INK_TOP - YK_APPS_INK_BOTTOM)};
            CHECK(box_in(&ink, YK_R_CONTENT));
        }
        CHECK(yk_apps_hit(&L, m->cx, m->cy) == k);  // a tap on it reports 「更多」's index
    }
    // The 6-place ring, 0 to 4 apps: 「设置」 upper left, 「更多」 lower left, at r 128; names in the circle; a tap on each
    // reports its index.
    for (int apps = 0; apps <= 4; apps++) {
        yk_apps_item_t items[YK_APPS_MAX];
        const int k = apps + 2;
        for (int i = 0; i < apps; i++) items[i] = (yk_apps_item_t){.slug = "breathe", .installed = true};
        items[apps] = (yk_apps_item_t){.slug = YK_APPS_MORE};
        items[apps + 1] = (yk_apps_item_t){.slug = YK_APPS_SETTINGS};
        yk_apps_item_slots(items, k, slots);
        int name_w[YK_APPS_MAX];
        for (int i = 0; i < k; i++) name_w[i] = yk_apps_name_room(slots[i], 22);
        yk_apps_layout_t L;
        yk_apps_layout_at(k, slots, name_w, 22, 30, &L);
        const yk_apps_sat_t *m = &L.sat[k - 2], *g = &L.sat[k - 1];
        CHECK(L.n == k && m->slot == YK_APPS_MORE_PLACE6 && m->cx < YK_CX - 100 && m->cy > YK_CY);
        CHECK(g->slot == YK_APPS_SETTINGS_PLACE && g->cx < YK_CX - 100 && g->cy < YK_CY);
        for (int i = 0; i < k; i++) {
            const double dx = L.sat[i].cx - YK_CX, dy = L.sat[i].cy - YK_CY;
            CHECK(fabs(sqrt(dx * dx + dy * dy) - YK_APPS_R6) < 1.0);
            const yk_box_t ink = {L.sat[i].name.x, (int16_t)(L.sat[i].name.y + YK_APPS_INK_TOP), L.sat[i].name.w,
                                  (int16_t)(L.sat[i].name.h - YK_APPS_INK_TOP - YK_APPS_INK_BOTTOM)};
            CHECK(box_in(&ink, YK_R_CONTENT));
            CHECK(yk_apps_hit(&L, L.sat[i].cx, L.sat[i].cy) == i + 1);
        }
        CHECK(apps < 1 || (L.sat[0].cx == YK_CX && L.sat[0].cy == YK_CY - YK_APPS_R6));  // the most recent at 12
    }
    // Every count, the widest names and labels, both line heights: nothing meets, everything in the circle. The
    // bottom one's disc (with an in-use ring) clears the middle's label, and its name stays in the circle (BOSS-2 10-25:
    // 「中间 Yay 名字底到正下方圆盘顶」, an English name like Recording at the bottom).
    {
        const int name_ws[] = {16, 48, 80, 96, 140};
        const int label_ws[] = {28, 61, 96, 130};
        const int hs[] = {22, 24};
        int combos = 0, gap_min = 99;
        for (int apps = 0; apps <= 4; apps++)
            for (size_t a = 0; a < 5; a++)
                for (size_t b = 0; b < 4; b++)
                    for (size_t c = 0; c < 2; c++) {
                        yk_apps_item_t items[YK_APPS_MAX];
                        const int k = apps + 2;
                        for (int i = 0; i < apps; i++) items[i] = (yk_apps_item_t){.slug = "x", .installed = true};
                        items[apps] = (yk_apps_item_t){.slug = YK_APPS_MORE};
                        items[apps + 1] = (yk_apps_item_t){.slug = YK_APPS_SETTINGS};
                        yk_apps_item_slots(items, k, slots);
                        int w[YK_APPS_MAX];
                        for (int i = 0; i < k; i++) w[i] = name_ws[(a + i) % 5];
                        yk_apps_layout_t L;
                        yk_apps_layout_at(k, slots, w, hs[c], label_ws[b], &L);
                        bool ok = box_in(&L.label, YK_R_CONTENT);
                        yk_box_t texts[1 + YK_APPS_MAX];
                        int nt = 0, discs[1 + YK_APPS_MAX][3], nd = 0;
                        texts[nt++] = L.label;
                        discs[nd][0] = YK_CX, discs[nd][1] = YK_CY, discs[nd++][2] = YK_APPS_CENTER_R + YK_APPS_RING_W;
                        for (int i = 0; i < L.n; i++) {
                            const yk_box_t *nb = &L.sat[i].name;
                            const yk_box_t ink = {nb->x, (int16_t)(nb->y + YK_APPS_INK_TOP), nb->w,
                                                  (int16_t)(nb->h - YK_APPS_INK_TOP - YK_APPS_INK_BOTTOM)};
                            ok = ok && box_in(&ink, YK_R_CONTENT) && nb->w <= yk_apps_name_room(L.sat[i].slot, hs[c]);
                            ok = ok && corner_in(L.sat[i].cx + YK_APPS_SAT_R + YK_APPS_RING_W, L.sat[i].cy, YK_R_CONTENT);
                            texts[nt++] = *nb;
                            discs[nd][0] = L.sat[i].cx, discs[nd][1] = L.sat[i].cy, discs[nd++][2] = YK_APPS_SAT_R + YK_APPS_RING_W;
                            if (L.sat[i].slot == YK_APPS_SIX + 3) {
                                const int gap = L.sat[i].cy - YK_APPS_SAT_R - YK_APPS_RING_W - (L.label.y + L.label.h);
                                if (gap < gap_min) gap_min = gap;
                            }
                        }
                        for (int i = 0; i < nt; i++) {
                            for (int j = i + 1; j < nt; j++)
                                if (meet(&texts[i], &texts[j])) ok = false, printf("  six: texts %d/%d meet (apps=%d)\n", i, j, apps);
                            for (int d = 0; d < nd; d++)
                                if (disc_meets(discs[d][0], discs[d][1], discs[d][2], &texts[i]))
                                    ok = false, printf("  six: text %d meets disc %d (apps=%d w=%d h=%d)\n", i, d, apps, name_ws[a], hs[c]);
                        }
                        for (int d = 0; d < nd; d++)
                            for (int e = d + 1; e < nd; e++) {
                                const int dx = discs[d][0] - discs[e][0], dy = discs[d][1] - discs[e][1], r = discs[d][2] + discs[e][2];
                                if (dx * dx + dy * dy < r * r) ok = false;
                            }
                        CHECK(ok);
                        combos++;
                    }
        CHECK(combos == 5 * 5 * 4 * 2);
        printf("  six-place ring: the middle's label to the bottom disc (in-use ring included): %d px at least\n", gap_min);
        CHECK(gap_min >= 1);
    }
    // The bottom place's name: as wide as elsewhere (a long English name is cut with … to it, inside the circle).
    CHECK(yk_apps_name_room(YK_APPS_SIX + 3, 23) >= 80);
    // The cached mark and 「更多」's dots stay on the disc's own area (what each frame repaints).
    CHECK(YK_APPS_CACHE_D + YK_APPS_CACHE_R <= YK_APPS_BADGE_D + YK_APPS_BADGE_R);
    CHECK(YK_APPS_DOT_GAP + YK_APPS_DOT_R < YK_APPS_SAT_R);
    CHECK(9.2f * YK_APPS_GEAR_UNIT + YK_APPS_GEAR_UNIT < YK_APPS_SAT_R);  // 「设置」's gear too
}

static void test_text(void) {
    char out[64];
    CHECK(yk_text_fit("面对面翻译", width16, NULL, 96, out, sizeof(out)) == 80 && !strcmp(out, "面对面翻译"));
    // Too long: cut on a character, "…" at the end, within the width.
    const int w = yk_text_fit("Language Buddy Plus", width16, NULL, 96, out, sizeof(out));
    CHECK(w <= 96 && w > 60);
    CHECK(strlen(out) > 3 && !strcmp(out + strlen(out) - 3, "\xE2\x80\xA6"));
    CHECK(!strncmp(out, "Language", 8));
    CHECK(yk_text_fit("一二三四五六七八", width16, NULL, 96, out, sizeof(out)) == 96 && !strcmp(out, "一二三四五\xE2\x80\xA6"));
    CHECK(yk_text_fit(NULL, width16, NULL, 96, out, sizeof(out)) == 0 && out[0] == 0);
    char c[5];
    yk_first_char("呼吸之间", c);
    CHECK(!strcmp(c, "呼"));
    yk_first_char("breathe", c);
    CHECK(!strcmp(c, "B"));
    yk_first_char("", c);
    CHECK(c[0] == 0);
    yk_first_char(NULL, c);
    CHECK(c[0] == 0);
}

static void test_catalog(void) {
    yk_catalog_t cat = demo_catalog();
    cat.count = 5;
    static uint8_t a8[4096], argb[16384], argb2[16384];
    for (int i = 0; i < 4096; i++) a8[i] = (uint8_t)i;
    for (int i = 0; i < 16384; i++) argb[i] = (uint8_t)(i * 7), argb2[i] = (uint8_t)(i * 13);
    cat.app[0].icon_format = YK_ICON_ARGB8888;
    cat.app[1].icon_format = YK_ICON_A8;
    cat.app[1].icon_color = YK_SAGE;
    cat.app[2].icon_format = YK_ICON_NONE;
    cat.app[3].icon_format = YK_ICON_ARGB8888;
    cat.app[4].icon_format = YK_ICON_A8;
    const uint8_t *icons[YK_CATALOG_MAX] = {argb, a8, NULL, argb2, a8};
    static uint8_t img[YK_CATALOG_REGION];
    const size_t used = yk_catalog_encode(&cat, icons, img, sizeof(img));
    CHECK(used == 4096 + 5 * 128 + 2 * 16384 + 2 * 4096);
    // Read back like the device does: the header, then the table.
    yk_catalog_t back;
    uint32_t len, crc;
    CHECK(yk_catalog_decode_header(img, &back, &len, &crc));
    CHECK(back.count == 5 && len == 5 * 128 && back.seq == 7 && back.written_ms == 1792549233000LL && !strcmp(back.lang, "zh"));
    CHECK(yk_catalog_decode_table(img + YK_CATALOG_TABLE_AT, len, crc, &back));
    for (int i = 0; i < 5; i++) {
        CHECK(!strcmp(back.app[i].slug, cat.app[i].slug) && !strcmp(back.app[i].name, cat.app[i].name));
        CHECK(!strcmp(back.app[i].version, "1.0.0") && back.app[i].size == cat.app[i].size);
        CHECK(back.app[i].icon_format == cat.app[i].icon_format);
        const size_t bytes = yk_catalog_icon_bytes(back.app[i].icon_format);
        if (!bytes) continue;
        CHECK(back.app[i].icon_offset % 4 == 0 && back.app[i].icon_offset >= YK_CATALOG_TABLE_AT + len);
        CHECK(!memcmp(img + back.app[i].icon_offset, icons[i], bytes));
        CHECK(yk_catalog_crc32(0, img + back.app[i].icon_offset, bytes) == back.app[i].icon_crc);
    }
    CHECK(back.app[1].icon_color == YK_SAGE);
    // The channel (10-24) in byte 116 of the entry: stable is 0, as every older catalog reads.
    for (int i = 0; i < 5; i++) CHECK(back.app[i].channel == YK_CHANNEL_STABLE && !img[YK_CATALOG_TABLE_AT + i * 128 + 116]);
    {
        yk_catalog_t pv = cat;
        pv.app[2].channel = YK_CHANNEL_PREVIEW;
        pv.app[3].channel = 7;  // a value this writer does not know: stable
        static uint8_t img3[YK_CATALOG_REGION];
        CHECK(yk_catalog_encode(&pv, icons, img3, sizeof(img3)) == used);
        CHECK(img3[YK_CATALOG_TABLE_AT + 2 * 128 + 116] == 1 && img3[YK_CATALOG_TABLE_AT + 3 * 128 + 116] == 0);
        yk_catalog_t pb;
        CHECK(yk_catalog_decode_header(img3, &pb, &len, &crc) && yk_catalog_decode_table(img3 + YK_CATALOG_TABLE_AT, len, crc, &pb));
        CHECK(pb.app[2].channel == YK_CHANNEL_PREVIEW && pb.app[1].channel == YK_CHANNEL_STABLE && !strcmp(pb.app[2].slug, cat.app[2].slug));
        // Everything else in the bytes as before: an older reader (it reads no channel) gets the same entries.
        CHECK(!memcmp(img3 + YK_CATALOG_TABLE_AT + 2 * 128, img + YK_CATALOG_TABLE_AT + 2 * 128, 116));
    }
    // The icon's sha256 (Yay 0.1.5) in bytes 117..124: only with an icon; zero in older catalogs (fetched again)
    for (int i = 0; i < 5; i++) {
        CHECK(!back.app[i].icon_sha[0] && !back.app[i].icon_sha[7]);
        for (int b = 117; b < 128; b++) CHECK(!img[YK_CATALOG_TABLE_AT + i * 128 + b]);
    }
    {
        yk_catalog_t sh = cat;
        static const uint8_t sha[YK_CATALOG_ICON_SHA] = {0x6a, 0x87, 0x63, 0x64, 0xcb, 0x36, 0xb4, 0x72};
        memcpy(sh.app[1].icon_sha, sha, sizeof(sha));  // an A8
        memcpy(sh.app[2].icon_sha, sha, sizeof(sha));  // no icon: not written
        static uint8_t img4[YK_CATALOG_REGION];
        CHECK(yk_catalog_encode(&sh, icons, img4, sizeof(img4)) == used);
        CHECK(!memcmp(img4 + YK_CATALOG_TABLE_AT + 128 + 117, sha, sizeof(sha)));
        CHECK(!img4[YK_CATALOG_TABLE_AT + 2 * 128 + 117] && !img4[YK_CATALOG_TABLE_AT + 128 + 125]);
        yk_catalog_t sb;
        CHECK(yk_catalog_decode_header(img4, &sb, &len, &crc) && yk_catalog_decode_table(img4 + YK_CATALOG_TABLE_AT, len, crc, &sb));
        CHECK(!memcmp(sb.app[1].icon_sha, sha, sizeof(sha)) && !sb.app[2].icon_sha[0]);
        // All else as before: an older reader gets the same entry and icon
        CHECK(!memcmp(img4 + YK_CATALOG_TABLE_AT + 128, img + YK_CATALOG_TABLE_AT + 128, 117));
        CHECK(sb.app[1].icon_crc == back.app[1].icon_crc && sb.app[1].icon_offset == back.app[1].icon_offset);
    }
    // The same content encodes to the same bytes (the writer compares before erasing).
    static uint8_t img2[YK_CATALOG_REGION];
    CHECK(yk_catalog_encode(&cat, icons, img2, sizeof(img2)) == used && !memcmp(img, img2, used));
    // A changed table or header is not read; an erased region (0xFF) neither.
    img[YK_CATALOG_TABLE_AT + 40] ^= 1;
    CHECK(yk_catalog_decode_header(img, &back, &len, &crc) && !yk_catalog_decode_table(img + YK_CATALOG_TABLE_AT, len, crc, &back));
    img[YK_CATALOG_TABLE_AT + 40] ^= 1;
    img[9] ^= 1;
    CHECK(!yk_catalog_decode_header(img, &back, &len, &crc));
    memset(img2, 0xFF, 64);
    CHECK(!yk_catalog_decode_header(img2, &back, &len, &crc));
    // What cannot be written: a name too long for its 48 bytes, an icon missing, too many apps.
    yk_catalog_t bad = cat;
    memset(bad.app[2].name, 'x', sizeof(bad.app[2].name));  // no room for its NUL
    CHECK(yk_catalog_encode(&bad, icons, img2, sizeof(img2)) == 0);
    bad = cat;
    bad.app[2].slug[0] = 0;
    CHECK(yk_catalog_encode(&bad, icons, img2, sizeof(img2)) == 0);
    bad = cat;
    const uint8_t *missing[YK_CATALOG_MAX] = {NULL};
    CHECK(yk_catalog_encode(&bad, missing, img2, sizeof(img2)) == 0);
    bad.count = YK_CATALOG_MAX + 1;
    CHECK(yk_catalog_encode(&bad, icons, img2, sizeof(img2)) == 0);
    // The most there can be: 8 apps with ARGB icons fit the region.
    yk_catalog_t full = demo_catalog();
    full.count = YK_CATALOG_MAX;
    const uint8_t *all[YK_CATALOG_MAX];
    for (int i = 0; i < YK_CATALOG_MAX; i++) {
        snprintf(full.app[i].slug, sizeof(full.app[i].slug), "app-%d", i);
        full.app[i].icon_format = YK_ICON_ARGB8888;
        all[i] = argb;
    }
    const size_t most = yk_catalog_encode(&full, all, img2, sizeof(img2));
    CHECK(most > 0 && most <= YK_CATALOG_REGION);
    // An icon format from a later writer: read as none (the first character), the rest still read.
    CHECK(yk_catalog_encode(&cat, icons, img, sizeof(img)) == used);
    img[YK_CATALOG_TABLE_AT + 2 * 128 + 100] = 9;  // the third app's format
    const uint32_t c2 = yk_catalog_crc32(0, img + YK_CATALOG_TABLE_AT, len);
    img[16] = (uint8_t)c2, img[17] = (uint8_t)(c2 >> 8), img[18] = (uint8_t)(c2 >> 16), img[19] = (uint8_t)(c2 >> 24);
    const uint32_t hc = yk_catalog_crc32(0, img, 60);
    img[60] = (uint8_t)hc, img[61] = (uint8_t)(hc >> 8), img[62] = (uint8_t)(hc >> 16), img[63] = (uint8_t)(hc >> 24);
    CHECK(yk_catalog_decode_header(img, &back, &len, &crc) && yk_catalog_decode_table(img + YK_CATALOG_TABLE_AT, len, crc, &back));
    CHECK(back.app[2].icon_format == YK_ICON_NONE && back.app[3].icon_format == YK_ICON_ARGB8888);
    CHECK(yk_catalog_crc32(0, "123456789", 9) == 0xCBF43926u);  // the standard check value
}

static uint16_t to565(uint32_t c) { return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3)); }

static void test_icon(void) {
    static uint8_t a8[4096], argb[16384], px[YK_ICON_RGB565A8_BYTES];
    const uint8_t *alpha = px + 2 * 4096;
#define PX(i) ((uint16_t)(px[2 * (i)] | (px[2 * (i) + 1] << 8)))
    a8[0] = 255, a8[1] = 0, a8[2] = 128;
    yk_icon_rgb565a8(YK_ICON_A8, a8, YK_SAGE, 255, px);
    CHECK(PX(0) == to565(YK_SAGE) && alpha[0] == 255 && alpha[1] == 0 && alpha[2] == 128);
    // Not installed: 42 % of the icon's own alpha.
    yk_icon_rgb565a8(YK_ICON_A8, a8, YK_SAGE, YK_APPS_DIM_OPA, px);
    CHECK(PX(0) == to565(YK_SAGE) && alpha[0] == YK_APPS_DIM_OPA && alpha[1] == 0 && alpha[2] == 128 * YK_APPS_DIM_OPA / 255);
    // ARGB8888 is B, G, R, A.
    argb[0] = 0x5D, argb[1] = 0x79, argb[2] = 0xDF, argb[3] = 255;  // coral
    argb[4 + 3] = 0;
    yk_icon_rgb565a8(YK_ICON_ARGB8888, argb, 0, 255, px);
    CHECK(PX(0) == to565(YK_CORAL) && alpha[0] == 255 && alpha[1] == 0);
    yk_icon_rgb565a8(YK_ICON_NONE, argb, 0, 255, px);
    CHECK(alpha[0] == 0);
#undef PX
}

static void test_hint_geometry(void) {
    yk_hint_geo_t g;
    // The prototype: key at 3 o'clock, "按住这里说话" (6 x 18 px).
    yk_hint_geometry(90, 108, &g);
    CHECK(fabsf(g.ux - 1) < 1e-4f && fabsf(g.uy) < 1e-4f);
    CHECK(lroundf(g.tip_x) == 434 && lroundf(g.tip_y) == 233);
    CHECK(lroundf(g.a1_x) == 418 && lroundf(g.a1_y) == 219 && lroundf(g.a2_x) == 418 && lroundf(g.a2_y) == 247);
    CHECK(g.pill.x + g.pill.w == 404 && g.pill.h == YK_HINT_PILL_H && g.pill.w == 108 + 2 * YK_HINT_PILL_PAD);
    CHECK(g.pill.y + g.pill.h / 2 == 233);
    // Any key angle, any line up to 300 px: the pill in the content circle, on the key's side, short of the V
    // (which stays inside the rim band with its nudge).
    for (int deg = 0; deg < 360; deg += 5)
        for (int w = 0; w <= 300; w += 20) {
            yk_hint_geometry((float)deg, w, &g);
            bool ok = box_in(&g.pill, YK_R_CONTENT);
            const float reach = (g.pill.x + g.pill.w / 2.0f - YK_CX) * g.ux + (g.pill.y + g.pill.h / 2.0f - YK_CY) * g.uy +
                                fabsf(g.ux) * g.pill.w / 2.0f + fabsf(g.uy) * g.pill.h / 2.0f;
            ok = ok && reach <= YK_HINT_PILL_R + 1;
            const float back = YK_HINT_TIP_R - YK_HINT_V_DEPTH - YK_HINT_STROKE / 2.0f;
            ok = ok && reach < back;
            ok = ok && YK_HINT_TIP_R + YK_HINT_NUDGE + YK_HINT_STROKE / 2.0f < YK_R_BAND;
            if (w <= 160) ok = ok && reach > 120;  // a usual line sits next to the arrow, not in the middle
            if (!ok) printf("  hint deg=%d w=%d reach=%.1f pill=%d,%d %dx%d\n", deg, w, reach, g.pill.x, g.pill.y, g.pill.w, g.pill.h);
            CHECK(ok);
        }
}

/** 18 px text (YK_HINT_TEXT_SIZE), roughly: a Chinese character 18 px, a Latin letter 10. */
static int width18(uint32_t cp, void *ctx) {
    (void)ctx;
    return cp < 0x80 ? 10 : 18;
}

/** The key where it is on the tikpal-r1 (YK_KEY_ANGLE_DEG): half past one, 45 degrees clockwise from 12. Every line in
 *  both languages: the arrow points up and to the right, the pill and its text are in the 200 px content circle and
 *  on the screen, and the V (with its nudge and its stroke) is on the glass short of the rim band's ticks. */
static void test_hint_key_angle(void) {
    CHECK(YK_KEY_ANGLE_DEG == 45);
    for (int k = 0; k < YK_HINT_KIND_COUNT; k++)
        for (int en = 0; en < 2; en++) {
            char line[96];
            const int w = yk_text_fit(yk_hint_text((yk_hint_kind_t)k, en), width18, NULL, 300, line, sizeof(line));
            yk_hint_geo_t g;
            yk_hint_geometry(YK_KEY_ANGLE_DEG, w, &g);
            bool ok = fabsf(g.ux - 0.7071068f) < 1e-4f && fabsf(g.uy + 0.7071068f) < 1e-4f;  // towards half past one
            ok = ok && g.tip_x > YK_CX && g.tip_y < YK_CY;
            ok = ok && box_in(&g.pill, YK_R_CONTENT);
            ok = ok && g.pill.x >= 0 && g.pill.y >= 0 && g.pill.x + g.pill.w <= 2 * YK_CX && g.pill.y + g.pill.h <= 2 * YK_CY;
            const float xs[3] = {g.a1_x, g.tip_x, g.a2_x}, ys[3] = {g.a1_y, g.tip_y, g.a2_y};
            for (int i = 0; i < 3; i++) {
                const float x = xs[i] + g.ux * YK_HINT_NUDGE, y = ys[i] + g.uy * YK_HINT_NUDGE;
                const float r = sqrtf((x - YK_CX) * (x - YK_CX) + (y - YK_CY) * (y - YK_CY)) + YK_HINT_STROKE / 2.0f;
                ok = ok && r < YK_R_BAND && x >= 0 && y >= 0;
            }
            // The pill short of the V's arms.
            const float reach = (g.pill.x + g.pill.w / 2.0f - YK_CX) * g.ux + (g.pill.y + g.pill.h / 2.0f - YK_CY) * g.uy +
                                fabsf(g.ux) * g.pill.w / 2.0f + fabsf(g.uy) * g.pill.h / 2.0f;
            ok = ok && reach < YK_HINT_TIP_R - YK_HINT_V_DEPTH - YK_HINT_STROKE / 2.0f;
            if (!ok)
                printf("  hint at the key kind=%d en=%d w=%d pill=%d,%d %dx%d tip=%.1f,%.1f\n", k, en, w, g.pill.x, g.pill.y,
                       g.pill.w, g.pill.h, g.tip_x, g.tip_y);
            CHECK(ok);
        }
}

static void test_hint_rules(void) {
    yk_hint_state_t s = {0};
    s.app_bit = yk_hint_app_bit("breathe");
    // Just added: TALK for the first three wakes, then no more.
    for (int i = 0; i < 3; i++) {
        CHECK(yk_hint_rule(&s, YK_HINT_TALK));
        CHECK(yk_hint_shown(&s, YK_HINT_TALK));  // saved
    }
    CHECK(!yk_hint_rule(&s, YK_HINT_TALK) && !yk_hint_shown(&s, YK_HINT_TALK) && s.talk_shown == YK_HINT_CAP);
    // ... or after three turns, whichever comes first.
    yk_hint_state_t t = {0};
    CHECK(yk_hint_apply(&t, YK_HINT_EV_TALKED) && yk_hint_apply(&t, YK_HINT_EV_TALKED) && yk_hint_rule(&t, YK_HINT_TALK));
    CHECK(yk_hint_apply(&t, YK_HINT_EV_TALKED) && !yk_hint_rule(&t, YK_HINT_TALK));
    CHECK(!yk_hint_apply(&t, YK_HINT_EV_TALKED) && t.talked == YK_HINT_CAP);  // capped: no more writes
    // TAP: once per wake, never saved.
    CHECK(yk_hint_rule(&s, YK_HINT_TAP));
    CHECK(!yk_hint_shown(&s, YK_HINT_TAP) && !yk_hint_rule(&s, YK_HINT_TAP));
    CHECK(!yk_hint_apply(&s, YK_HINT_EV_WAKE) && yk_hint_rule(&s, YK_HINT_TAP));
    // APPS: the first time in this extension, until the ring was opened once.
    CHECK(yk_hint_rule(&s, YK_HINT_APPS));
    CHECK(yk_hint_shown(&s, YK_HINT_APPS) && !yk_hint_rule(&s, YK_HINT_APPS));
    CHECK(!yk_hint_shown(&s, YK_HINT_APPS));
    s.app_bit = yk_hint_app_bit("party-play");
    CHECK(s.app_bit != yk_hint_app_bit("breathe") && yk_hint_rule(&s, YK_HINT_APPS));
    CHECK(yk_hint_apply(&s, YK_HINT_EV_RING_OPENED) && !yk_hint_rule(&s, YK_HINT_APPS));
    CHECK(!yk_hint_apply(&s, YK_HINT_EV_RING_OPENED));  // once is enough: no second write
    // Never in Yay itself; BACK always.
    yk_hint_state_t y = {0};
    y.app_bit = yk_hint_app_bit("yay");
    CHECK(y.app_bit == 0 && !yk_hint_rule(&y, YK_HINT_APPS) && !yk_hint_shown(&y, YK_HINT_APPS));
    CHECK(yk_hint_rule(&s, YK_HINT_BACK) && !yk_hint_shown(&s, YK_HINT_BACK));
    CHECK(!yk_hint_rule(&s, YK_HINT_KIND_COUNT));
    // The five extensions have bits of their own.
    const char *ext[] = {"breathe", "party-play", "language-buddy", "homework-buddy", "translator"};
    uint32_t all = 0;
    for (int i = 0; i < 5; i++) {
        const uint32_t b = yk_hint_app_bit(ext[i]);
        CHECK(b && !(all & b));
        all |= b;
    }
    CHECK(yk_hint_app_bit(NULL) == 0 && yk_hint_app_bit("") == 0);
    // The lines, in the content circle at 18 px: the longest is short enough for the pill.
    for (int k = 0; k < YK_HINT_KIND_COUNT; k++) {
        CHECK(yk_hint_text((yk_hint_kind_t)k, false)[0] && yk_hint_text((yk_hint_kind_t)k, true)[0]);
        char out[64];
        CHECK(yk_text_fit(yk_hint_text((yk_hint_kind_t)k, true), width16, NULL, 300, out, sizeof(out)) <= 300);
    }
}

int main(void) {
    test_slots();
    test_layout();
    test_motion();
    test_hit();
    test_pick();
    test_pick_ring();
    test_text();
    test_catalog();
    test_icon();
    test_hint_geometry();
    test_hint_key_angle();
    test_hint_rules();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
