// Yaykit Link's long mode (docs/design/selfie.md 3, 自拍屏): Bluetooth on for as long as the app runs, one phone
// remembered, the round screen a HID key (Consumer Control: volume up, which the phone's camera takes as its
// shutter) and the app's own service on the same peripheral. Pairing happens here, in the app, as the exception the
// design agreed to (selfie.md 8): only in this app, one phone, gone when the app is left.
//
// Added beside yaykit_link.h; the provisioning link (yk_link_ble_start) is untouched. The two never run together:
// while the long mode runs, yk_link_ble_start() returns ESP_ERR_INVALID_STATE (and the other way round). Built only
// with CONFIG_BT_NIMBLE_ENABLED: firmware without NimBLE (Party Play) has none of these functions.
//
// - The phone's keys and subscriptions are kept in the app's own NVS namespace (`nvs_ns`), never NimBLE's own
//   (`nimble_bond`) nor any other firmware's: set CONFIG_BT_NIMBLE_NVS_PERSIST=n; NimBLE's RAM store is filled from
//   it at start. Yay and the other apps never see them.
// - A static random address of its own, made once and kept in `nvs_ns`: phones that paired with another firmware
//   at the chip's public address (the lab saw a tablet retry every 7-10 s) do not take it for theirs.
// - One phone (yk_link_bond.h): a second phone that pairs is held back and YK_KEEP_EV_ASK says so; the app asks on
//   the screen and calls yk_link_keep_answer(). While asked about, the new phone reads and writes nothing of the
//   app's service (insufficient authorisation) and gets no key presses.
// - Undirected advertising (100-150 ms) whenever nobody is connected; one connection, so nobody else finds it while
//   the phone is connected.
// - The phone's name (its GAP Device Name, read once connected) for the screen; it needs the GATT client
//   (CONFIG_BT_NIMBLE_ROLE_CENTRAL, CONFIG_BT_NIMBLE_GATT_CLIENT), else it stays "".
//
// Every characteristic of the app's service needs an encrypted link: the first read brings up the phone's pairing
// alert. Callbacks run on NimBLE's host task: keep them short, do not block, and write NVS elsewhere.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_KEEP_CHR_MAX 4
#define YK_KEEP_NAME_MAX 20  // the Bluetooth name, UTF-8 bytes: it goes in the 31-byte scan response

enum { YK_KEEP_READ = 1, YK_KEEP_WRITE = 2, YK_KEEP_NOTIFY = 4 };

typedef struct {
    uint8_t uuid[16];  // 128-bit, least significant byte first (as NimBLE's BLE_UUID128_INIT)
    uint8_t flags;     // YK_KEEP_READ | YK_KEEP_WRITE | YK_KEEP_NOTIFY; always encrypted
} yk_keep_chr_t;

typedef enum {
    YK_KEEP_EV_CONNECTED,     // a phone connected (nothing trusted yet)
    YK_KEEP_EV_TRUSTED,       // the remembered phone is in, encrypted: reads, writes, notifications, key presses
    YK_KEEP_EV_PAIRED,        // the first phone paired and is remembered now (TRUSTED follows)
    YK_KEEP_EV_ASK,           // another phone paired: ask 「换成这台手机？」, then yk_link_keep_answer()
    YK_KEEP_EV_REPLACED,      // the answer was yes: the new phone is the remembered one (TRUSTED follows)
    YK_KEEP_EV_REFUSED,       // no, the time ran out, or the new phone left: the old one stays
    YK_KEEP_EV_PAIR_FAILED,   // no phone remembered yet and pairing did not finish (cancelled, timed out)
    YK_KEEP_EV_UNKNOWN,       // a phone with keys this device no longer has (replaced, forgotten): let go
    YK_KEEP_EV_NAME,          // the connected phone's name was read (yk_link_keep_peer_name())
    YK_KEEP_EV_SUBSCRIBED,    // notifications on a characteristic turned on or off (yk_link_keep_subscribed())
    YK_KEEP_EV_DISCONNECTED,
} yk_keep_event_t;

typedef struct {
    const char *nvs_ns;  // the app's namespace: the phone's keys ("bond"), this device's address ("bt_addr")
    const char *name;    // the Bluetooth name (in the owner's language), at most YK_KEEP_NAME_MAX bytes
    uint8_t service[16]; // the app's service, in the advertisement (phones find the device by it)
    yk_keep_chr_t chrs[YK_KEEP_CHR_MAX];
    int n_chrs;
    /** A read of chrs[chr]: up to `size` bytes into `out`; returns the length (-1: refuse). */
    int (*on_read)(int chr, char *out, size_t size, void *ctx);
    /** A write to chrs[chr] (only from the trusted phone). */
    void (*on_write)(int chr, const uint8_t *data, size_t len, void *ctx);
    void (*on_event)(yk_keep_event_t ev, void *ctx);
    void *ctx;
    bool hid;            // the HID key (volume up) beside the app's service
    int ask_seconds;     // 「换成这台手机？」 times out as a no after this (0: 30)
} yk_link_keep_config_t;

/** Starts NimBLE in the long mode and advertises. From a task with an internal-RAM stack (it reads and may write
 *  NVS). ESP_ERR_INVALID_STATE while the provisioning link runs. */
esp_err_t yk_link_keep_start(const yk_link_keep_config_t *cfg);
/** Leaving the app (a restart follows): stops advertising and lets the phone go, waiting up to `wait_ms` for it to
 *  be gone, so it keeps no Bluetooth keyboard. NimBLE itself stays up. Any task. */
void yk_link_keep_end(int wait_ms);
bool yk_link_keep_active(void);
bool yk_link_keep_connected(void);
/** The remembered phone is connected and encrypted (and not being asked about). */
bool yk_link_keep_trusted(void);
/** A phone is remembered. */
bool yk_link_keep_remembered(void);
bool yk_link_keep_asking(void);
/** The connected (or asked-about) phone's name, "" when not read. */
void yk_link_keep_peer_name(char *out, size_t size);
bool yk_link_keep_subscribed(int chr);

/** A notification on chrs[chr] to the trusted phone, when it subscribed. Cut at the link's MTU - 3. */
esp_err_t yk_link_keep_notify(int chr, const void *data, size_t len);
/** The HID key: volume up pressed, released after 40 ms (a longer press makes an iPhone record). Trusted only. */
esp_err_t yk_link_keep_press(void);
/** The battery service's level (0..100). */
void yk_link_keep_battery(int percent);
/** 「换成这台手机？」's answer (any task). */
void yk_link_keep_answer(bool yes);
/** Forgets the remembered phone (development: `keep forget`). */
void yk_link_keep_forget(void);

#ifdef __cplusplus
}
#endif
