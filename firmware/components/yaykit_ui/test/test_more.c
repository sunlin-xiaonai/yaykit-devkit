// Host tests for 「我的应用」 (yk_more_logic.c): the honeycomb's places, sizes, what shows, touches, drags, the words.
// Run: bash test/run.sh
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yk_apps_logic.h"
#include "yk_more_logic.h"
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

/** 16 px: a Chinese character 16, a Latin letter 9 (as test_apps.c), a space 4 and 「·」 15 (measured on the device:
 *  「在圆屏上 · 点一下打开」 is 167 px); `ctx` the size. */
static int width_at(uint32_t cp, void *ctx) {
    const int px = (int)(intptr_t)ctx;
    if (cp == ' ') return px / 4;
    if (cp == 0xB7) return px * 15 / 16;
    return cp < 0x80 ? (px * 9 + 8) / 16 : px;
}

static int hex_dist(yk_hex_t h) {
    const int a = abs(h.q), b = abs(h.r), c = abs(h.q + h.r);
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

static void test_places(void) {
    // The prototype's first ring: right, upper right, upper left, left, lower left, lower right.
    static const int ring1[7][2] = {{0, 0}, {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
    for (int i = 0; i < 7; i++) CHECK(yk_more_place(i).q == ring1[i][0] && yk_more_place(i).r == ring1[i][1]);
    // Every place once, ring by ring outwards, each step to a neighbour along the ring.
    for (int i = 0; i < YK_MORE_MAX; i++) {
        const yk_hex_t a = yk_more_place(i);
        for (int j = 0; j < i; j++) {
            const yk_hex_t b = yk_more_place(j);
            CHECK(a.q != b.q || a.r != b.r);
        }
        if (i > 0) CHECK(hex_dist(a) >= hex_dist(yk_more_place(i - 1)));
    }
    CHECK(hex_dist(yk_more_place(7)) == 2 && hex_dist(yk_more_place(18)) == 2 && hex_dist(yk_more_place(19)) == 3);
    // On the screen: the middle at (233, 196); neighbours a step apart.
    float x, y, x2, y2;
    yk_more_at(yk_more_place(0), 0, 0, &x, &y);
    CHECK(fabsf(x - 233) < 0.01f && fabsf(y - 196) < 0.01f);
    for (int i = 1; i <= 6; i++) {
        yk_more_at(yk_more_place(i), 0, 0, &x2, &y2);
        CHECK(fabsf(hypotf(x2 - x, y2 - y) - YK_MORE_STEP) < 0.6f);
    }
    // The design's board 3: the first ring's lower pair at (188, 274), (278, 274).
    yk_more_at(yk_more_place(5), 0, 0, &x2, &y2);
    CHECK(lroundf(x2) == 188 && lroundf(y2) == 274);
}

static void test_sizes(void) {
    CHECK(lroundf(yk_more_size(0)) == 100 && lroundf(yk_more_size(YK_MORE_STEP)) == 72);
    CHECK(lroundf(yk_more_size(YK_MORE_STEP * 1.7320508f)) == 54 && lroundf(yk_more_size(YK_MORE_STEP * 2)) == 40);
    CHECK(lroundf(yk_more_size(400)) == 40);
    float last = yk_more_size(0);
    for (float d = 0.5f; d < 300; d += 0.5f) {
        const float s = yk_more_size(d);
        CHECK(s <= last && last - s < 0.5f);  // smaller outwards, smoothly
        last = s;
    }
    CHECK(yk_more_opa(0) == 255 && yk_more_opa(150) == 255 && yk_more_opa(300) == YK_MORE_DIM_OPA);
    CHECK(yk_more_opa(170) < 255 && yk_more_opa(170) > YK_MORE_DIM_OPA);
    CHECK(yk_more_glyph_px(100) == 36 && yk_more_glyph_px(72) == 28 && yk_more_glyph_px(54) == 22 && yk_more_glyph_px(40) == 16);
}

/** Wherever it rests (each app in the middle in turn), every app drawn is on the glass, clear of the title and the
 *  two lines; about 19 at most are drawn. */
/** Autofocus: sizes and light by the distance from the middle on the screen. At rest it is the old look (each app's
 *  distance from the one in the middle); halfway between two, both are mid-sized and neither fully lit. */
static void test_autofocus(void) {
    for (int focus = 0; focus < 19; focus++) {
        float ox, oy;
        yk_more_offset_for(focus, &ox, &oy);
        for (int j = 0; j < 37; j++) {
            float x, y;
            yk_more_at(yk_more_place(j), ox, oy, &x, &y);
            CHECK(fabsf(yk_more_from_middle(x, y) - yk_more_apart(j, focus)) < 0.01f);
        }
    }
    CHECK(yk_more_lit(0) == 1.0f && yk_more_lit(YK_MORE_LIT_R) == 0 && yk_more_lit(200) == 0 && yk_more_lit(-3) == 1.0f);
    float last = yk_more_lit(0), last_size = yk_more_size(0);
    for (float d = 0.5f; d <= 200; d += 0.5f) {  // never brighter nor bigger farther out, no jumps
        const float l = yk_more_lit(d), size = yk_more_size(d);
        CHECK(l <= last && last - l < 0.03f);
        CHECK(size <= last_size && last_size - size < 0.5f);
        last = l, last_size = size;
    }
    // Dragged halfway from the middle one to its right neighbour: the two at the same distance, the same size between
    // 100 and 72, lit a little less than half.
    float ox0, oy0, ox1, oy1, x0, y0, x1, y1;
    yk_more_offset_for(0, &ox0, &oy0);
    yk_more_offset_for(1, &ox1, &oy1);
    const float ox = (ox0 + ox1) / 2, oy = (oy0 + oy1) / 2;
    yk_more_at(yk_more_place(0), ox, oy, &x0, &y0);
    yk_more_at(yk_more_place(1), ox, oy, &x1, &y1);
    const float d0 = yk_more_from_middle(x0, y0), d1 = yk_more_from_middle(x1, y1);
    CHECK(fabsf(d0 - d1) < 0.01f && fabsf(yk_more_size(d0) - 86) < 0.5f && yk_more_lit(d0) == 0);
}

/** Eight sizes of first characters and icons: the four at rest exactly as before, the others between them only near
 *  the middle; a tap opens at once what is on the device. */
static void test_size_classes(void) {
    CHECK(yk_more_size_class(100) == 0 && yk_more_size_class(72) == 2 && yk_more_size_class(54) == 4);
    CHECK(yk_more_size_class(40) == 7 && yk_more_size_class(30) == 7 && yk_more_size_class(120) == 0);
    CHECK(yk_more_size_class(86) == 1 && yk_more_size_class(63) == 3 && yk_more_size_class(49) == 5 && yk_more_size_class(44) == 6);
    CHECK(yk_more_class_at_rest(0) && !yk_more_class_at_rest(1) && yk_more_class_at_rest(2) && !yk_more_class_at_rest(3));
    CHECK(yk_more_class_at_rest(4) && !yk_more_class_at_rest(5) && !yk_more_class_at_rest(6) && yk_more_class_at_rest(7));
    // The rest sizes' classes are at rest; growing, the class never goes back up, and never skips more than one.
    for (int i = 0; i < 37; i++) CHECK(yk_more_class_at_rest(yk_more_size_class(yk_more_size(yk_more_apart(i, 0)))));
    int last = yk_more_size_class(20);
    for (float size = 20.5f; size <= 110; size += 0.5f) {
        const int c = yk_more_size_class(size);
        CHECK(c <= last && last - c <= 1);
        last = c;
    }
    // First characters: the old four at rest; between them sizes Yay's pages already use (no new font size).
    static const int USED[] = {36, 32, 28, 24, 22, 20, 18, 16};
    for (float size = 30; size <= 110; size += 1) {
        const int px = yk_more_glyph_px(size);
        bool known = false;
        for (size_t k = 0; k < sizeof(USED) / sizeof(USED[0]); k++) known |= px == USED[k];
        CHECK(known);
    }
    CHECK(yk_more_opens_at_once(YK_MORE_SLOT) && yk_more_opens_at_once(YK_MORE_CACHE));
    CHECK(!yk_more_opens_at_once(YK_MORE_CLOUD) && !yk_more_opens_at_once(YK_MORE_SOON));
    CHECK(YK_MORE_HALO_OPA == 77);  // 30 % (BOSS-2 10-26)
}

static void test_shown(void) {
    const int n = 30;
    int most = 0;
    for (int f = 0; f < n; f++) {
        float ox, oy;
        yk_more_offset_for(f, &ox, &oy);
        CHECK(yk_more_nearest(n, ox, oy) == f);
        int shown = 0;
        for (int i = 0; i < n; i++) {
            float x, y;
            yk_more_at(yk_more_place(i), ox, oy, &x, &y);
            if (!yk_more_shown(x, y)) continue;
            shown++;
            const float r = yk_more_size(yk_more_apart(i, f)) / 2;
            CHECK(fabsf(yk_more_apart(i, f) - hypotf(x - YK_MORE_CX, y - YK_MORE_CY)) < 0.01f);  // at rest: on screen
            CHECK(hypotf(x - YK_CX, y - YK_CY) + r <= YK_R_BAND);  // inside the rim's band
            CHECK(y - r >= YK_MORE_TITLE_Y + 18 - 4 && y + r <= YK_MORE_NAME_Y + 4);  // clear of the words
            if (i == f) CHECK(lroundf(r * 2) == 100);
        }
        CHECK(shown >= 3);  // an app at the edge has fewer around it
        if (shown > most) most = shown;
    }
    printf("  most shown at rest: %d\n", most);
    CHECK(most <= 19);
    // Far from the middle, not drawn.
    CHECK(!yk_more_shown(233, 40) && !yk_more_shown(233, 340) && !yk_more_shown(20, 196) && yk_more_shown(233, 196));
}

static void test_touch_and_drag(void) {
    const int n = 30;
    float ox = 0, oy = 0;
    CHECK(yk_more_apart(0, 1) == YK_MORE_STEP && yk_more_apart(3, 3) == 0 && yk_more_apart(2, 9) == yk_more_apart(9, 2));
    CHECK(yk_more_hit(n, 0, ox, oy, 233, 196) == 0);
    CHECK(yk_more_hit(n, 0, ox, oy, 233 + 90, 196) == 1);   // right of the middle
    CHECK(yk_more_hit(n, 0, ox, oy, 233, 196 + 60) == -1);  // between the lower pair
    CHECK(yk_more_hit(n, 0, ox, oy, 233, 420) == -1);       // the words below
    CHECK(yk_more_hit(3, 0, ox, oy, 233 - 90, 196) == -1);  // only three apps: nothing to the left
    // Dragged a step left with app 0 still the middle one: app 1 under the middle, at its own size (72, not 100).
    CHECK(yk_more_hit(n, 0, -YK_MORE_STEP, 0, 233 + 30, 196) == 1 && yk_more_hit(n, 0, -YK_MORE_STEP, 0, 233 + 40, 196) == -1);
    CHECK(yk_more_hit(n, 1, -YK_MORE_STEP, 0, 233 + 40, 196) == 1);  // ... with it the middle one: 100
    // A drag past the honeycomb's edge is held within reach; a small one is not.
    ox = 2000, oy = -1500;
    yk_more_clamp(n, &ox, &oy);
    CHECK(hypotf(ox, oy) < 400 && hypotf(ox, oy) > 200);
    ox = 30, oy = 20;
    yk_more_clamp(n, &ox, &oy);
    CHECK(ox == 30 && oy == 20);
    ox = 300, oy = 0;
    yk_more_clamp(1, &ox, &oy);  // one app: it stays near the middle
    CHECK(hypotf(ox, oy) <= YK_MORE_STEP / 2 + 0.01f);
    // Let go: the nearest app comes to the middle.
    yk_more_offset_for(4, &ox, &oy);
    CHECK(yk_more_nearest(n, ox + 20, oy - 25) == 4);
    float x, y;
    CHECK(!yk_more_move(0, 0, 90, -78, 160, &x, &y) && x > 45 && x < 90);
    CHECK(yk_more_move(0, 0, 90, -78, YK_MORE_MOVE_MS, &x, &y) && x == 90 && y == -78);
    CHECK(yk_more_ease(0) == 0 && yk_more_ease(YK_MORE_MOVE_MS) == 1);
    // In and out: half way at half the time; never back, and no frame (~30 ms) a fifth of the way.
    CHECK(fabsf(yk_more_ease(YK_MORE_MOVE_MS / 2) - 0.5f) < 0.01f && yk_more_ease(YK_MORE_MOVE_MS / 4) < 0.16f);
    float last = 0;
    for (int t = 0; t <= YK_MORE_MOVE_MS; t += 5) {
        const float k = yk_more_ease(t);
        CHECK(k >= last && k - yk_more_ease(t > 30 ? t - 30 : 0) < 0.2f);
        last = k;
    }
}

static void test_words(void) {
    // The lines below and the title: their glyphs in the content circle at their sizes.
    const int name_room = yk_more_line_room(YK_MORE_NAME_Y, 30), state_room = yk_more_line_room(YK_MORE_STATE_Y, 22);
    const int title_room = yk_more_line_room(YK_MORE_TITLE_Y, 25);
    printf("  rooms: title %d, name %d, state %d px\n", title_room, name_room, state_room);
    CHECK(title_room >= 110 && name_room >= 250 && state_room >= 200);
    char out[96];
    for (int en = 0; en < 2; en++)
        for (int w = 0; w <= YK_MORE_SOON; w++)
            for (int cur = 0; cur < 2; cur++)
                for (int pv = 0; pv < 2; pv++) {
                    const char *t = yk_more_state_text((yk_more_where_t)w, cur, pv, en);
                    CHECK(t[0] && yk_text_fit(t, width_at, (void *)(intptr_t)16, 1000, out, sizeof(out)) <= state_room);
                }
    CHECK(yk_text_fit("在圆屏上 · 点一下打开", width_at, (void *)(intptr_t)16, 1000, out, sizeof(out)) == 167);
    CHECK(!strcmp(yk_more_state_text(YK_MORE_CACHE, false, false, false), "下载过 · 几秒就好"));
    CHECK(!strcmp(yk_more_state_text(YK_MORE_CLOUD, false, false, false), "要下载 · 大约二十秒"));
    CHECK(!strcmp(yk_more_state_text(YK_MORE_SLOT, true, false, false), "正在用"));
    // A preview says so first (A2); one coming soon says only that.
    CHECK(!strcmp(yk_more_state_text(YK_MORE_CLOUD, false, true, false), "预览版 · 要下载 · 约二十秒"));
    CHECK(!strcmp(yk_more_state_text(YK_MORE_CACHE, false, true, false), "预览版 · 下载过 · 几秒就好"));
    CHECK(!strcmp(yk_more_state_text(YK_MORE_SLOT, false, true, false), "预览版 · 点一下打开"));
    CHECK(!strcmp(yk_more_state_text(YK_MORE_SOON, false, false, false), "即将推出"));
    CHECK(!strcmp(yk_more_state_text(YK_MORE_SOON, false, false, true), "Coming soon"));
    // The shake: back where it was, never farther than YK_MORE_SHAKE_PX.
    CHECK(yk_more_shake(0) == 0 && yk_more_shake(YK_MORE_SHAKE_MS) == 0 && yk_more_shake(-5) == 0);
    float most = 0;
    for (int ms = 0; ms <= YK_MORE_SHAKE_MS; ms += 4) most = fmaxf(most, fabsf(yk_more_shake(ms)));
    CHECK(most > YK_MORE_SHAKE_PX / 2 && most <= YK_MORE_SHAKE_PX);
    CHECK(yk_text_fit("我的应用 · 64", width_at, (void *)(intptr_t)18, 1000, out, sizeof(out)) <= title_room);
    CHECK(yk_text_fit("My apps · 64", width_at, (void *)(intptr_t)18, 1000, out, sizeof(out)) <= title_room);
    CHECK(yk_more_line_room(0, 20) == 0 && yk_more_line_room(YK_CY - 10, 20) == YK_MORE_LINE_W);
}

static void test_pick(void) {
    yk_catalog_t cat = {.count = 5, .lang = "zh"};
    const char *slugs[] = {"language-buddy", "breathe", "party-play", "translator", "yay"};
    const char *names[] = {"口语陪练", "呼吸之间", "聚会玩", "面对面翻译", "Yay"};
    for (int i = 0; i < cat.count; i++) strcpy(cat.app[i].slug, slugs[i]), strcpy(cat.app[i].name, names[i]);
    // In a slot (Party Play used last), in the cache (one of them also in a slot), then the catalog's on neither.
    const yk_apps_slot_t inst[] = {{"breathe", 100}, {"party-play", 200}, {"yay", 999}};
    const char *const cached[] = {"homework-buddy", "breathe", "+more", "", "yay"};
    yk_more_item_t out[YK_MORE_MAX];
    int n = yk_more_pick(inst, 3, cached, 5, &cat, NULL, 0, out, YK_MORE_MAX);
    CHECK(n == 5);
    CHECK(!strcmp(out[0].slug, "party-play") && out[0].where == YK_MORE_SLOT && !strcmp(out[0].name, "聚会玩"));
    CHECK(!strcmp(out[1].slug, "breathe") && out[1].where == YK_MORE_SLOT);
    CHECK(!strcmp(out[2].slug, "homework-buddy") && out[2].where == YK_MORE_CACHE && !out[2].name);  // not in the catalog
    CHECK(!strcmp(out[3].slug, "language-buddy") && out[3].where == YK_MORE_CLOUD && !strcmp(out[3].name, "口语陪练"));
    CHECK(!strcmp(out[4].slug, "translator") && out[4].where == YK_MORE_CLOUD);
    for (int i = 0; i < n; i++) CHECK(!out[i].current);
    // A limit; nothing at all (never online, nothing installed).
    CHECK(yk_more_pick(inst, 3, cached, 5, &cat, NULL, 0, out, 2) == 2 && !strcmp(out[1].slug, "breathe"));
    CHECK(yk_more_pick(NULL, 0, NULL, 0, NULL, NULL, 0, out, YK_MORE_MAX) == 0);
    // The planned ones last (A2), as given; one the store or the device has already is theirs.
    const yk_more_item_t planned[] = {
        {.slug = "party-play", .name = "聚会玩", .where = YK_MORE_CLOUD},
        {.slug = "homework-buddy", .name = "作业伙伴", .where = YK_MORE_SOON},
        {.slug = "translator", .name = "面对面翻译", .where = YK_MORE_SOON},  // in the catalog now: the catalog's
        {.slug = "ai-remote", .name = "AI 遥控器", .where = YK_MORE_SOON, .glyph = "遥"},
    };
    cat.app[3].channel = YK_CHANNEL_PREVIEW;  // translator: a preview only
    n = yk_more_pick(inst, 3, cached, 5, &cat, planned, 4, out, YK_MORE_MAX);
    CHECK(n == 6);
    if (n == 6) {
        CHECK(!strcmp(out[2].slug, "homework-buddy") && out[2].where == YK_MORE_CACHE);  // downloaded: not coming soon
        CHECK(!strcmp(out[4].slug, "translator") && out[4].where == YK_MORE_CLOUD && out[4].preview);
        CHECK(!out[0].preview && !out[3].preview);
        CHECK(!strcmp(out[5].slug, "ai-remote") && out[5].where == YK_MORE_SOON && out[5].glyph && !strcmp(out[5].glyph, "遥"));
    }
    for (int i = 0; i < n; i++) CHECK(strcmp(out[i].slug, "party-play") || out[i].where == YK_MORE_SLOT);  // in a slot
    // Nothing on the device, never online: the planned ones alone.
    CHECK(yk_more_pick(NULL, 0, NULL, 0, NULL, planned, 4, out, YK_MORE_MAX) == 4 && out[0].where == YK_MORE_CLOUD);
}

/** Yay 0.1.4's delivery 2.3: how long, or the size, for one to download. */
static void test_state_line(void) {
    char out[96], fit[96];
    // The rounding table at each edge, both languages.
    static const struct {
        int s;
        const char *zh, *en;
    } ETA[] = {{1, "约 10 秒", "~10 s"},          {10, "约 10 秒", "~10 s"},         {11, "约 20 秒", "~20 s"},
               {20, "约 20 秒", "~20 s"},         {21, "约 30 秒", "~30 s"},         {36, "约 40 秒", "~40 s"},
               {50, "约 50 秒", "~50 s"},         {55, "约 50 秒", "~50 s"},         {56, "约 1 分钟", "~1 min"},
               {89, "约 1 分钟", "~1 min"},       {90, "约 2 分钟", "~2 min"},       {149, "约 2 分钟", "~2 min"},
               {150, "约 3 分钟", "~3 min"},      {569, "约 9 分钟", "~9 min"},      {570, "10 分钟以上", "10+ min"},
               {7200, "10 分钟以上", "10+ min"}};
    for (size_t i = 0; i < sizeof(ETA) / sizeof(ETA[0]); i++) {
        CHECK(!strcmp(yk_more_eta_text(ETA[i].s, false, out, sizeof(out)), ETA[i].zh));
        CHECK(!strcmp(yk_more_eta_text(ETA[i].s, true, out, sizeof(out)), ETA[i].en));
    }
    // Sizes by 1000: whole KB under 1 MB, one decimal from there.
    CHECK(!strcmp(yk_more_size_text(860000, out, sizeof(out)), "860 KB"));
    CHECK(!strcmp(yk_more_size_text(860499, out, sizeof(out)), "860 KB"));
    CHECK(!strcmp(yk_more_size_text(200, out, sizeof(out)), "1 KB"));
    CHECK(!strcmp(yk_more_size_text(999499, out, sizeof(out)), "999 KB"));
    CHECK(!strcmp(yk_more_size_text(999500, out, sizeof(out)), "1.0 MB"));
    CHECK(!strcmp(yk_more_size_text(3100000, out, sizeof(out)), "3.1 MB"));
    CHECK(!strcmp(yk_more_size_text(3149999, out, sizeof(out)), "3.1 MB"));
    CHECK(!strcmp(yk_more_size_text(3150000, out, sizeof(out)), "3.2 MB"));
    // The whole line: measured, only the size, neither; a preview's.
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 3100000, 36, false, out, sizeof(out)), "要下载 · 约 40 秒"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 3100000, 36, true, out, sizeof(out)), "To download · ~40 s"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 3100000, 0, false, out, sizeof(out)), "要下载 · 3.1 MB"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 3100000, 0, true, out, sizeof(out)), "To download · 3.1 MB"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 860000, 0, false, out, sizeof(out)), "要下载 · 860 KB"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 0, 0, false, out, sizeof(out)), "要下载"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, false, 0, 0, true, out, sizeof(out)), "To download"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, true, 3100000, 36, false, out, sizeof(out)), "预览版 · 约 40 秒"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, true, 3100000, 36, true, out, sizeof(out)), "Preview · ~40 s download"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, true, 3100000, 0, false, out, sizeof(out)), "预览版 · 3.1 MB"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, true, 3100000, 0, true, out, sizeof(out)), "Preview · 3.1 MB download"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CLOUD, false, true, 0, 0, false, out, sizeof(out)), "预览版 · 要下载"));
    // In the package cache (Yay 0.1.5's delivery 3): the copying's seconds, nothing of the network
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 3100000, 10, false, out, sizeof(out)), "下载过 · 约 10 秒"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 3100000, 10, true, out, sizeof(out)), "Downloaded · ~10 s"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 2300000, 18, false, out, sizeof(out)), "下载过 · 约 20 秒"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 2300000, 18, true, out, sizeof(out)), "Downloaded · ~20 s"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, true, 2300000, 18, false, out, sizeof(out)), "预览版 · 下载过 · 约 20 秒"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, true, 2300000, 18, true, out, sizeof(out)), "Preview · ~20 s"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 0, 0, false, out, sizeof(out)), "下载过"));  // no size
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 0, 0, true, out, sizeof(out)), "Downloaded"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, true, 0, 0, false, out, sizeof(out)), "预览版 · 下载过"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, true, 0, 0, true, out, sizeof(out)), "Preview · downloaded"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, true, false, 2300000, 18, false, out, sizeof(out)), "正在用"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, true, 3100000, 570, false, out, sizeof(out)), "预览版 · 下载过"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_CACHE, false, false, 3100000, 570, false, out, sizeof(out)), "下载过 · 10 分钟以上"));
    // Not to download nor downloaded: as before, whatever the size and estimate.
    CHECK(!strcmp(yk_more_state_line(YK_MORE_SLOT, false, false, 3100000, 36, false, out, sizeof(out)), "在圆屏上 · 点一下打开"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_SLOT, true, false, 0, 0, false, out, sizeof(out)), "正在用"));
    CHECK(!strcmp(yk_more_state_line(YK_MORE_SOON, false, false, 3100000, 36, false, out, sizeof(out)), "即将推出"));
    // Every one fits the line (222 px at 16), the longest a preview's 10+ minutes; 预览版 · 要下载 · 10 分钟以上 would not.
    const int state_room = yk_more_line_room(YK_MORE_STATE_Y, 22);
    static const uint32_t BYTES[] = {0, 1, 860000, 999499, 999500, 3100000, 3500000, 9999999};  // a slot holds 3.5 MB: one figure before the point
    static const int SECS[] = {0, 1, 10, 11, 36, 55, 56, 89, 90, 569, 570, 100000};
    int widest = 0;
    for (int en = 0; en < 2; en++)
        for (int w = 0; w <= YK_MORE_SOON; w++)
            for (int pv = 0; pv < 2; pv++)
                for (size_t b = 0; b < sizeof(BYTES) / sizeof(BYTES[0]); b++)
                    for (size_t e = 0; e < sizeof(SECS) / sizeof(SECS[0]); e++) {
                        yk_more_state_line((yk_more_where_t)w, false, pv, BYTES[b], BYTES[b] ? SECS[e] : 0, en, out,
                                           sizeof(out));
                        const int px = yk_text_fit(out, width_at, (void *)(intptr_t)16, 1000, fit, sizeof(fit));
                        CHECK(out[0] && px <= state_room);
                        if (px > state_room) printf("  too wide: %s (%d)\n", out, px);
                        if (px > widest) widest = px;
                    }
    printf("  state line: widest %d of %d px\n", widest, state_room);
    CHECK(yk_text_fit("预览版 · 10 分钟以上", width_at, (void *)(intptr_t)16, 1000, fit, sizeof(fit)) <= state_room);
    CHECK(yk_text_fit("预览版 · 要下载 · 10 分钟以上", width_at, (void *)(intptr_t)16, 1000, fit, sizeof(fit)) > state_room);
    // Too small a buffer is cut, never overrun; none at all is left alone.
    char tiny[8] = "xxxxxxx";
    yk_more_state_line(YK_MORE_CLOUD, false, false, 3100000, 36, false, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
    CHECK(yk_more_state_line(YK_MORE_CLOUD, false, false, 3100000, 36, false, tiny, 0) == tiny);
}

/** The icons' four sizes (#531 4): the same picture, its ink kept (area) and its edges where they were. */
static void test_icon_scale(void) {
    // Eight sizes since autofocus; the four at rest as before (84, 61, 45, 34 in discs of 100, 72, 54, 40).
    CHECK(yk_more_icon_px(0) == 84 && yk_more_icon_px(2) == 61 && yk_more_icon_px(4) == 45 && yk_more_icon_px(7) == 34);
    CHECK(yk_more_icon_px(1) == 72 && yk_more_icon_px(3) == 53 && yk_more_icon_px(5) == 41 && yk_more_icon_px(6) == 37);
    CHECK(yk_more_icon_px(-1) == 84 && yk_more_icon_px(9) == 34);
    for (int g = 1; g < YK_MORE_GLYPHS; g++) CHECK(yk_more_icon_px(g) < yk_more_icon_px(g - 1));
    static uint8_t src[64 * 64], dst[128 * 128];
    static float tmp[YK_MORE_ICON_TMP];
    const int sizes[] = {84, 61, 45, 34, 64, 1, 128};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        const int n = sizes[k];
        memset(src, 255, sizeof(src));  // all ink: all ink
        yk_more_icon_scale(src, n, dst, tmp);
        int lo = 255;
        for (int i = 0; i < n * n; i++) lo = dst[i] < lo ? dst[i] : lo;
        CHECK(lo == 255);
        memset(src, 0, sizeof(src));  // none: none
        yk_more_icon_scale(src, n, dst, tmp);
        int hi = 0;
        for (int i = 0; i < n * n; i++) hi = dst[i] > hi ? dst[i] : hi;
        CHECK(hi == 0);
    }
    // A disc of radius 24: its share of the square stays (within 1 %) at every size, and its middle stays full
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const float dx = x + 0.5f - 32, dy = y + 0.5f - 32;
            src[y * 64 + x] = dx * dx + dy * dy <= 24 * 24 ? 255 : 0;
        }
    double share64 = 0;
    for (int i = 0; i < 64 * 64; i++) share64 += src[i];
    share64 /= 64.0 * 64 * 255;
    for (int g = 0; g < YK_MORE_GLYPHS; g++) {
        const int n = yk_more_icon_px(g);
        yk_more_icon_scale(src, n, dst, tmp);
        double share = 0;
        for (int i = 0; i < n * n; i++) share += dst[i];
        share /= (double)n * n * 255;
        CHECK(fabs(share - share64) < 0.01);
        CHECK(dst[(n / 2) * n + n / 2] == 255 && dst[0] == 0 && dst[n * n - 1] == 0);
        // symmetric left to right
        int asym = 0;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n / 2; x++) {
                const int d = dst[y * n + x] - dst[y * n + n - 1 - x];
                asym = d > asym ? d : -d > asym ? -d : asym;
            }
        CHECK(asym <= 1);
    }
    // The left half inked: halved where it falls between pixels (34: 64 / 34 per pixel, the edge at 17)
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) src[y * 64 + x] = x < 32 ? 255 : 0;
    yk_more_icon_scale(src, 34, dst, tmp);
    CHECK(dst[5 * 34 + 16] == 255 && dst[5 * 34 + 17] == 0);
    yk_more_icon_scale(src, 45, dst, tmp);
    CHECK(dst[5 * 45 + 21] == 255 && dst[5 * 45 + 22] > 100 && dst[5 * 45 + 22] < 155 && dst[5 * 45 + 23] == 0);
}

int main(void) {
    test_state_line();
    test_icon_scale();
    test_pick();
    test_places();
    test_sizes();
    test_autofocus();
    test_size_classes();
    test_shown();
    test_touch_and_drag();
    test_words();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
