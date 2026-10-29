// Host tests for the pure C parts of yaykit_board. Run: bash firmware/components/yaykit_board/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../rtc_regs.h"
#include "../yk_board_logic.h"
#include "../yk_font_pack.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

static void test_round_area(void) {
    int32_t x1 = 7, y1 = 10, x2 = 100, y2 = 465;
    yk_board_round_area(&x1, &y1, &x2, &y2);
    CHECK(x1 == 6 && y1 == 10);    // starts move down to even
    CHECK(x2 == 101 && y2 == 465); // ends move up to odd
    // An area that is already aligned stays as it is, and the full screen stays inside 466 px.
    x1 = 0, y1 = 0, x2 = 465, y2 = 465;
    yk_board_round_area(&x1, &y1, &x2, &y2);
    CHECK(x1 == 0 && y1 == 0 && x2 == 465 && y2 == 465);
    // A one-pixel area grows to the 2x2 block around it.
    x1 = x2 = 233, y1 = y2 = 120;
    yk_board_round_area(&x1, &y1, &x2, &y2);
    CHECK(x1 == 232 && x2 == 233 && y1 == 120 && y2 == 121);
}

static void test_touch_point(void) {
    int32_t x = -1, y = -1;
    // No finger: released, point untouched (LVGL keeps the last one).
    const uint8_t none[6] = {0, 0x00, 0x01, 0x10, 0x00, 0x20};
    CHECK(!yk_board_touch_point(none, 466, 466, &x, &y));
    CHECK(x == -1 && y == -1);
    // One finger at (272, 32); the high nibble of the finger byte and of the coordinate high bytes is not data.
    const uint8_t one[6] = {0, 0xF1, 0xF1, 0x10, 0x80, 0x20};
    CHECK(yk_board_touch_point(one, 466, 466, &x, &y));
    CHECK(x == 272 && y == 32);
    // Reports past the edge are clamped to the last pixel.
    const uint8_t edge[6] = {0, 0x01, 0x01, 0xD2, 0x0F, 0xFF};
    CHECK(yk_board_touch_point(edge, 466, 466, &x, &y));
    CHECK(x == 465 && y == 465);
}

static void test_audio_rates(void) {
    CHECK(yk_board_audio_rate_ok(16000));
    CHECK(yk_board_audio_rate_ok(24000));
    CHECK(!yk_board_audio_rate_ok(0));
    CHECK(!yk_board_audio_rate_ok(22050));
    CHECK(!yk_board_audio_rate_ok(48000));
}

static void test_rtc_regs(void) {
    // 2026-10-18 12:34:56 UTC, a Sunday: BCD seconds .. years, weekday 0 = Sunday.
    uint8_t r[RTC_TIME_REGS];
    rtc_regs_from_unix(1792326896, r);
    const uint8_t want[RTC_TIME_REGS] = {0x56, 0x34, 0x12, 0x18, 0, 0x10, 0x26};
    int same = 1;
    for (int i = 0; i < RTC_TIME_REGS; i++) same &= r[i] == want[i];
    CHECK(same);
    int64_t t = 0;
    CHECK(rtc_regs_to_unix(r, &t) && t == 1792326896);
    // Round trips: the first trusted second, a leap day, the last two-digit year.
    const int64_t times[] = {1767225600, 1835481599, 4102444799};
    for (int k = 0; k < 3; k++) {
        rtc_regs_from_unix(times[k], r);
        CHECK(rtc_regs_to_unix(r, &t) && t == times[k]);
    }
    rtc_regs_from_unix(1835481599, r);
    CHECK(r[3] == 0x29 && r[5] == 0x02 && r[4] == 2); // 2028-02-29, a Tuesday
    // Not trusted: oscillator stopped, before 2026, impossible or non-BCD values.
    rtc_regs_from_unix(1792326896, r);
    r[0] |= RTC_OS;
    CHECK(!rtc_regs_to_unix(r, &t));
    rtc_regs_from_unix(1767225599, r);
    CHECK(!rtc_regs_to_unix(r, &t));
    rtc_regs_from_unix(1792326896, r);
    r[5] = 0x13;
    CHECK(!rtc_regs_to_unix(r, &t));
    rtc_regs_from_unix(1792326896, r);
    r[1] = 0x1A;
    CHECK(!rtc_regs_to_unix(r, &t));
}

static void test_battery(void) {
    // The factory firmware's own log: raw 3344 is 4012.8 mV (0x14 = 0x10, 0x15 = 0x0D; high nibble of 0x15 ignored).
    CHECK(yk_board_battery_mv(0x10, 0x0D) == 4012);
    CHECK(yk_board_battery_mv(0x10, 0xFD) == 4012);
    CHECK(yk_board_battery_mv(0x00, 0x00) == 0 && yk_board_battery_mv(0xFF, 0x0F) == 4914);
    CHECK(yk_board_battery_percent(3000) == 0 && yk_board_battery_percent(3300) == 0);
    CHECK(yk_board_battery_percent(4200) == 100 && yk_board_battery_percent(4400) == 100);
    CHECK(yk_board_battery_percent(3800) == 50 && yk_board_battery_percent(4012) == 81);
    // Unplugged, the registers read 0: not a battery at 0 %.
    CHECK(!yk_board_battery_known(yk_board_battery_mv(0x00, 0x00)) && !yk_board_battery_known(1200));
    CHECK(yk_board_battery_known(yk_board_battery_mv(0x6C, 0x5E)) && yk_board_battery_known(3300));
    CHECK(!yk_board_battery_known(yk_board_battery_mv(0xFF, 0x0F)));  // 4.9 V: not a cell
    for (int mv = 3300, last = 0; mv <= 4200; mv += 5) {  // never goes down as the voltage goes up
        const int p = yk_board_battery_percent(mv);
        CHECK(p >= last && p >= 0 && p <= 100);
        last = p;
    }
}

/** A pack's index as gen_font_pack.py writes it. */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static void make_index(uint8_t tail[YK_FONT_PACK_TAIL], int count, const char *const langs[],
                       const uint32_t offsets[], const uint32_t sizes[]) {
    memset(tail, 0, YK_FONT_PACK_TAIL);
    memcpy(tail, "YKFP", 4);
    tail[4] = 1;
    tail[5] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        strncpy((char *)tail + 8 + 12 * i, langs[i], 4);
        put32(tail + 12 + 12 * i, offsets[i]);
        put32(tail + 16 + 12 * i, sizes[i]);
    }
    put32(tail + 56, yk_crc32(tail, 56));
}

static void test_battery_adc_and_prefs(void) {
    // V_BAT / 2 on the pin; 1.9 V at the pin is a 3.8 V cell.
    CHECK(yk_board_vbat_mv(1900) == 3800);
    CHECK(yk_board_vbus_present(2480) && !yk_board_vbus_present(40) && !yk_board_vbus_present(1599));
    // On battery the resting curve; charging takes the rise off and never says 100 % while still charging.
    CHECK(yk_board_battery_level(3800, false) == 50);
    CHECK(yk_board_battery_level(3920, true) == 50);
    CHECK(yk_board_battery_level(4390, true) == 99);
    CHECK(yk_board_battery_level(4200, false) == 100);
    CHECK(yk_board_battery_level(0, false) == -1 && yk_board_battery_level(5000, true) == -1);
    CHECK(yk_board_battery_full(4180) && !yk_board_battery_full(4000));
    // Preferences: defaults, ranges, bad stored values read as the default.
    CHECK(yk_board_pref_default(YK_PREF_VOLUME) == 6 && yk_board_pref_default(YK_PREF_BRIGHTNESS) == 80 &&
          yk_board_pref_default(YK_PREF_SCREEN_OFF) == 30);
    CHECK(yk_board_pref_clamp(YK_PREF_VOLUME, 14) == 10 && yk_board_pref_clamp(YK_PREF_VOLUME, -2) == 0);
    CHECK(yk_board_pref_clamp(YK_PREF_BRIGHTNESS, 0) == 5 && yk_board_pref_clamp(YK_PREF_BRIGHTNESS, 101) == 100);
    CHECK(yk_board_pref_clamp(YK_PREF_SCREEN_OFF, 0) == 0 && yk_board_pref_clamp(YK_PREF_SCREEN_OFF, 3) == 15 &&
          yk_board_pref_clamp(YK_PREF_SCREEN_OFF, 99999) == 3600);
    CHECK(yk_board_pref_read(YK_PREF_VOLUME, -1) == 6 && yk_board_pref_read(YK_PREF_VOLUME, 3) == 3 &&
          yk_board_pref_read(YK_PREF_VOLUME, 200) == 6);
    CHECK(yk_board_pref_read(YK_PREF_BRIGHTNESS, 2) == 80 && yk_board_pref_read(YK_PREF_SCREEN_OFF, 0) == 0);
}

static void test_font_pack(void) {
    CHECK(yk_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);  // the standard check value
    const uint32_t part = 0x300000;
    uint8_t tail[YK_FONT_PACK_TAIL];
    const char *const langs[] = {"zh", "ja"};
    const uint32_t offs[] = {0, 1300000}, sizes[] = {1290000, 1100000};
    make_index(tail, 2, langs, offs, sizes);
    yk_font_pack_t p;
    CHECK(yk_font_pack_parse(tail, part, &p) && p.count == 2);
    CHECK(!strcmp(p.fonts[0].lang, "zh") && p.fonts[0].offset == 0 && p.fonts[0].size == 1290000);
    CHECK(!strcmp(p.fonts[1].lang, "ja") && p.fonts[1].offset == 1300000 && p.fonts[1].size == 1100000);
    // Which font first: the language's own, the others after it as fallbacks.
    int order[YK_FONT_PACK_MAX];
    CHECK(yk_font_pack_order(&p, "ja", order) == 2 && order[0] == 1 && order[1] == 0);
    CHECK(yk_font_pack_order(&p, "ja-JP", order) == 2 && order[0] == 1);
    CHECK(yk_font_pack_order(&p, "JA", order) == 2 && order[0] == 1);
    CHECK(yk_font_pack_order(&p, "zh-CN", order) == 2 && order[0] == 0 && order[1] == 1);
    CHECK(yk_font_pack_order(&p, "en", order) == 2 && order[0] == 0 && order[1] == 1);  // Latin is in both
    CHECK(yk_font_pack_order(&p, "es-419", order) == 2 && order[0] == 0);
    CHECK(yk_font_pack_order(&p, NULL, order) == 2 && order[0] == 0);
    CHECK(yk_font_pack_order(&p, "jav", order) == 2 && order[0] == 0);  // not "ja"
    // What an old partition (one font, the rest erased) or a damaged index reads as: no pack.
    uint8_t erased[YK_FONT_PACK_TAIL];
    memset(erased, 0xFF, sizeof(erased));
    CHECK(!yk_font_pack_parse(erased, part, &p) && p.count == 0);
    uint8_t bad[YK_FONT_PACK_TAIL];
    memcpy(bad, tail, sizeof(bad));
    bad[20] ^= 1;  // a changed offset: the CRC no longer matches
    CHECK(!yk_font_pack_parse(bad, part, &p));
    const uint32_t over[] = {0, part - YK_FONT_PACK_TAIL - 100};  // the second font would run into the index
    make_index(tail, 2, langs, over, sizes);
    CHECK(!yk_font_pack_parse(tail, part, &p));
    const uint32_t far[] = {0, 0xFFFFFFF0u};  // an offset past the end: no wrap-around
    make_index(tail, 2, langs, far, sizes);
    CHECK(!yk_font_pack_parse(tail, part, &p));
    make_index(tail, 2, langs, offs, sizes);
    tail[5] = 5;  // more fonts than an index holds
    CHECK(!yk_font_pack_parse(tail, part, &p));
    const char *const unnamed[] = {"", "ja"};
    make_index(tail, 2, unnamed, offs, sizes);
    CHECK(!yk_font_pack_parse(tail, part, &p));
    // One font is a pack too.
    make_index(tail, 1, langs, offs, sizes);
    CHECK(yk_font_pack_parse(tail, part, &p) && p.count == 1);
    CHECK(yk_font_pack_order(&p, "ja", order) == 1 && order[0] == 0);
}

int main(void) {
    test_font_pack();
    test_battery();
    test_battery_adc_and_prefs();
    test_round_area();
    test_touch_point();
    test_audio_rates();
    test_rtc_regs();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
