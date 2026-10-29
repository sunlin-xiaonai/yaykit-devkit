// The round-screen design spec v1 (yaykit/docs/product/round-screen-design.md) as constants, for every firmware on
// the 466 x 466 round panel: colours (spec 3), type sizes (4), geometry (2), the rim (5), Yay's sizes (7), timing
// (11). Plain C, no LVGL: host tests and tools can include it. The spec wins; change it there first.
// (yk_theme.h is the app's generated palette; on the round screen these are the ones to use.)
#pragma once

// ---------------------------------------------------------------- colours, 0xRRGGBB (lv_color_hex)
#define YK_PAPER      0x171C19  // background
#define YK_NIGHT      0x000000  // background of the screens used at night (Breathe's sleep scene)
#define YK_INK        0xFAF7EF  // main text, Yay's body
#define YK_INK2       0xA5AA9E  // secondary text
#define YK_INK3       0x737A70  // hints, disabled, the connecting arc
#define YK_PANEL      0x30372F  // panels, not selected
#define YK_CORAL      0xDF795D  // the main action, the talk button, countdowns, corrections, attention
#define YK_CORAL_TEXT 0xEE9A80  // small coral text on the dark background
#define YK_SAGE       0x9FB38C  // listening, progress, selected, success
#define YK_SAGE_TEXT  0xB9CAA8  // small sage text on the dark background
#define YK_AMBER      0xE3A857  // reminders, battery low, points running out
#define YK_CREAM      0xF3E6C8  // accents, highlights (never a large area, nor ink)

// Party Play's seat colours, clockwise from the side key's side; odd seats dark, even light. Each seat also has
// 1..8 dots (colour-blind players tell them apart by the dots).
#define YK_SEATS 8
#define YK_SEAT_COLORS {0xDF795D, 0xF3E6C8, 0xB7654A, 0x9FB38C, 0xB98AA6, 0xE3A857, 0x808B4F, 0xCDB18E}

// ---------------------------------------------------------------- type sizes, px (one weight; never smaller than 16)
#define YK_SIZE_HERO_MIN      96  // the hero's big characters: 96..160 by app (a character in the grid, dice, a countdown)
#define YK_SIZE_HERO_MAX      160
#define YK_SIZE_SUBTITLE      24  // translations, subtitles
#define YK_SIZE_SUBTITLE_SMALL 20 // when three lines at 24 do not fit
#define YK_SIZE_HEADING       24  // page titles, language names (22..24)
#define YK_SIZE_STATUS        18  // status line, button text, "I heard"
#define YK_SIZE_MIN           16  // points, notes: the smallest there is
#define YK_NOTICE_MAX_CHARS   8   // a notice is one line of at most 8 characters (subtitles excepted)

// ---------------------------------------------------------------- geometry
#define YK_CX 233
#define YK_CY 233
#define YK_R_EDGE    233  // the glass
#define YK_R_BAND    214  // the rim band: 214..233 (ticks, the dial's touch ring: 65 px from the edge)
#define YK_R_CONTENT 200  // every text inside this
#define YK_R_CORE    140  // the hero inside this
#define YK_DIAL_TOUCH 65  // touches this close to the edge are on the dial
#define YK_TOUCH_MIN_W 96 // a touch target at least 96 x 64 (the drawing may be smaller), 12 apart
#define YK_TOUCH_MIN_H 64
#define YK_TOUCH_GAP   12
#define YK_BUTTON_MIN_D 92  // a main round button

// ---------------------------------------------------------------- the rim (spec 5)
#define YK_RIM_R       226  // outer radius
#define YK_RIM_W       6
#define YK_RIM_W_HERO  14   // a hero dial may be 12..16 wide (Homework Buddy's 60-minute timer)

// ---------------------------------------------------------------- Yay (spec 7)
#define YK_YAY_HERO      264
#define YK_YAY_COMPANION 176
#define YK_YAY_CORNER    132

// ---------------------------------------------------------------- timing (spec 11)
#define YK_PAGE_MS         250   // page changes, ease out
#define YK_CAPTION_FADE_MS 150
#define YK_RESPONSE_MS     100   // every touch and key shows something within this
#define YK_DIM_AFTER_S     60    // no input: down to YK_DIM_PERCENT ...
#define YK_DIM_PERCENT     30
#define YK_NIGHT_MAX_PERCENT 40  // 22:00..7:00
