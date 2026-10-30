#include "stations.h"

#include <stdio.h>

// Live MP3 streams, checked from a Mac on 2026-10-30: 32 kbps, 44.1 or 48 kHz stereo. Plenty for a mono 24 kHz
// speaker, and half the load of the 64 kbps streams, which the device could not always keep ahead of real time.
// For testing on our own device; the rights to these streams are not cleared for a store release. Sorted by
// frequency: the next station is the next one to the right on the dial.
static const station_t STATIONS[] = {
    {"北京", "北京文艺广播", 876, "http://lhttp.qingting.fm/live/333/24k.mp3"},
    {"杭州", "杭州交通广播", 918, "http://lhttp.qingting.fm/live/1133/24k.mp3"},
    {"深圳", "深圳飞扬971", 971, "http://lhttp.qingting.fm/live/1271/24k.mp3"},
    {"北京", "北京音乐广播", 974, "http://lhttp.qingting.fm/live/332/24k.mp3"},
    {"北京", "北京新闻广播", 1006, "http://lhttp.qingting.fm/live/339/24k.mp3"},
    {"北京", "北京交通广播", 1039, "http://lhttp.qingting.fm/live/336/24k.mp3"},
};
#define COUNT ((int)(sizeof(STATIONS) / sizeof(STATIONS[0])))

int stations_count(void) { return COUNT; }

static int wrap(int i) { return ((i % COUNT) + COUNT) % COUNT; }

const station_t *station_at(int i) { return &STATIONS[wrap(i)]; }

int station_step(int i, int delta) { return wrap(i + delta); }

void station_freq_text(const station_t *st, char *out, size_t size) {
    snprintf(out, size, "%d.%d", st->mhz10 / 10, st->mhz10 % 10);
}

int dial_permille(int mhz10) {
    if (mhz10 < FM_MIN_10) mhz10 = FM_MIN_10;
    if (mhz10 > FM_MAX_10) mhz10 = FM_MAX_10;
    return (mhz10 - FM_MIN_10) * 1000 / (FM_MAX_10 - FM_MIN_10);
}

void listen_sentence(const char *name, int seconds, char *out, size_t size) {
    if (seconds < 60) snprintf(out, size, "听了一会儿%s", name);
    else snprintf(out, size, "听了%s %d 分钟", name, (seconds + 30) / 60);
}
