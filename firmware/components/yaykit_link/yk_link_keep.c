// Yaykit Link's long mode (yk_link_keep.h). Grown from the selfie lab's sl_ble.c (exp/selfie-lab, c527e06c), with
// what the lab could not do yet: keys in the app's own namespace (not NimBLE's nimble_bond), a static random
// address of its own, and the remembered phone re-pairing asked about like a new one.
//
// The store: NimBLE runs on its RAM store (CONFIG_BT_NIMBLE_NVS_PERSIST=n). Its write and delete callbacks are
// wrapped: what yk_link_bond.c keeps goes through and is mirrored into one NVS blob in the app's namespace ("bond");
// a second phone's writes wait in `s_held`. At start the blob is written back into the RAM store before the host
// runs (so the phone's IRK is in the resolving list from the first advertisement).
#include "yk_link_keep.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "yaykit_link.h"
#include "yk_link_bond.h"
#include "yk_link_priv.h"

void ble_store_config_init(void);

static const char *TAG = "yk_keep";

#define APPEARANCE 0x03C0  // generic HID
#define CCCD_MAX 8
#define HELD_MAX 8
#define MIRROR_MAGIC 0x31424B59u  // "YKB1"

// ---------------------------------------------------------------- state (changed on the host task unless noted)

static yk_link_keep_config_t s_cfg;
static volatile bool s_active;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_enc, s_trusted, s_sub[YK_KEEP_CHR_MAX], s_sub_report;
static uint8_t s_addr[6];      // our static random address
static bool s_addr_ok;
static yk_bond_t s_bond;
static char s_peer_name[48];
static uint8_t s_report;       // HID report 1: bit 0 volume up
static uint8_t s_battery = 100;
static esp_timer_handle_t s_release_timer, s_ask_timer;
static ble_store_write_fn *s_write;    // NimBLE's own (RAM)
static ble_store_delete_fn *s_delete;
static bool s_bypass;                  // the store's own changes (restoring, forgetting): no mirror update

typedef struct {
    int type;
    union ble_store_value v;
} held_t;
static held_t s_held[HELD_MAX];
static int s_held_n;

/** The remembered phone, as kept in NVS. */
typedef struct {
    uint32_t magic;
    uint16_t sec_size, cccd_size;
    uint8_t have_our, have_peer, n_cccd, pad;
    struct ble_store_value_sec our, peer;
    struct ble_store_value_cccd cccd[CCCD_MAX];
} mirror_t;
static mirror_t s_mirror;

// GATT
static ble_uuid128_t s_svc_uuid, s_chr_uuid[YK_KEEP_CHR_MAX];
static uint16_t s_chr_handle[YK_KEEP_CHR_MAX], s_report_handle;
static struct ble_gatt_chr_def s_chr_defs[YK_KEEP_CHR_MAX + 1];
static struct ble_gatt_svc_def s_svcs[5];

enum { H_INFO = 100, H_MAP, H_CTRL, H_REPORT, H_PROTO, B_LEVEL, D_PNP, D_MANUF };

// Consumer Control, report 1: one bit, Volume Increment (0xE9), seven bits padding.
static const uint8_t REPORT_MAP[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x01, 0x09, 0xE9, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0xC0,
};
static const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x02};  // HID 1.11, no country, normally connectable
static const uint8_t PNP_ID[] = {0x02, 0x3A, 0x30, 0x01, 0x80, 0x01, 0x00};  // USB vendor 0x303A (Espressif)

bool yk_link_keep_active(void) { return s_active; }
bool yk_link_keep_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool yk_link_keep_trusted(void) { return s_trusted && yk_link_keep_connected(); }
bool yk_link_keep_remembered(void) { return s_bond.have; }
bool yk_link_keep_asking(void) { return s_bond.asking; }
bool yk_link_keep_subscribed(int chr) { return chr >= 0 && chr < YK_KEEP_CHR_MAX && s_sub[chr]; }

void yk_link_keep_peer_name(char *out, size_t size) {
    if (size) snprintf(out, size, "%s", s_peer_name);
}

void yk_link_keep_battery(int percent) { s_battery = percent < 0 ? 100 : percent > 100 ? 100 : (uint8_t)percent; }

static void emit(yk_keep_event_t ev) {
    if (s_cfg.on_event) s_cfg.on_event(ev, s_cfg.ctx);
}

static void addr_text(const ble_addr_t *a, char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static yk_bond_addr_t bond_addr(const ble_addr_t *a) {
    yk_bond_addr_t b = {.type = a->type};
    memcpy(b.val, a->val, 6);
    return b;
}

static ble_addr_t nimble_addr(const yk_bond_addr_t *b) {
    ble_addr_t a = {.type = b->type};
    memcpy(a.val, b->val, 6);
    return a;
}

// ---------------------------------------------------------------- the mirror in NVS

static void mirror_save(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(s_cfg.nvs_ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (s_mirror.have_peer) err = nvs_set_blob(h, "bond", &s_mirror, sizeof(s_mirror));
        else if ((err = nvs_erase_key(h, "bond")) == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "phone not kept: %s", esp_err_to_name(err));
}

static void mirror_clear(void) {
    memset(&s_mirror, 0, sizeof(s_mirror));
    s_mirror.magic = MIRROR_MAGIC;
    s_mirror.sec_size = sizeof(struct ble_store_value_sec);
    s_mirror.cccd_size = sizeof(struct ble_store_value_cccd);
}

static void mirror_load(void) {
    mirror_clear();
    nvs_handle_t h;
    if (nvs_open(s_cfg.nvs_ns, NVS_READONLY, &h) != ESP_OK) return;
    mirror_t m;
    size_t n = sizeof(m);
    const bool ok = nvs_get_blob(h, "bond", &m, &n) == ESP_OK && n == sizeof(m) && m.magic == MIRROR_MAGIC &&
                    m.sec_size == sizeof(struct ble_store_value_sec) &&
                    m.cccd_size == sizeof(struct ble_store_value_cccd) && m.have_peer && m.n_cccd <= CCCD_MAX;
    nvs_close(h);
    if (ok) s_mirror = m;  // else (another NimBLE's layout): the phone pairs again
}

/** Mirror one write the store took. */
static void mirror_write(int type, const union ble_store_value *v) {
    if (type == BLE_STORE_OBJ_TYPE_OUR_SEC) {
        s_mirror.our = v->sec;
        s_mirror.have_our = 1;
    } else if (type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        s_mirror.peer = v->sec;
        s_mirror.have_peer = 1;
    } else if (type == BLE_STORE_OBJ_TYPE_CCCD) {
        int i = 0;
        while (i < s_mirror.n_cccd && s_mirror.cccd[i].chr_val_handle != v->cccd.chr_val_handle) i++;
        if (i == s_mirror.n_cccd) {
            if (i == CCCD_MAX) return;
            s_mirror.n_cccd++;
        }
        s_mirror.cccd[i] = v->cccd;
    } else {
        return;
    }
    mirror_save();
}

/** The remembered phone's keys back into NimBLE's RAM store (at start, and when a re-pairing was refused). */
static void mirror_restore(void) {
    if (!s_mirror.have_peer) return;
    s_bypass = true;
    if (s_mirror.have_our) s_write(BLE_STORE_OBJ_TYPE_OUR_SEC, (const union ble_store_value *)&s_mirror.our);
    s_write(BLE_STORE_OBJ_TYPE_PEER_SEC, (const union ble_store_value *)&s_mirror.peer);
    for (int i = 0; i < s_mirror.n_cccd; i++)
        s_write(BLE_STORE_OBJ_TYPE_CCCD, (const union ble_store_value *)&s_mirror.cccd[i]);
    s_bypass = false;
}

// ---------------------------------------------------------------- the store's callbacks

static const ble_addr_t *peer_of(int type, const union ble_store_value *v) {
    if (type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC) return &v->sec.peer_addr;
    if (type == BLE_STORE_OBJ_TYPE_CCCD) return &v->cccd.peer_addr;
    return NULL;
}

static void ask_timeout(void *arg);

static int store_write(int type, const union ble_store_value *val) {
    const ble_addr_t *peer = peer_of(type, val);
    if (s_bypass || !peer) return s_write(type, val);
    const yk_bond_addr_t p = bond_addr(peer);
    bool first = false;
    char a[18];
    addr_text(peer, a);
    switch (yk_bond_write(&s_bond, &p, &first)) {
    case YK_BOND_KEEP: {
        const int rc = s_write(type, val);
        if (rc == 0) mirror_write(type, val);
        if (first) {
            printf("YK_KEEP bond first phone=%s\n", a);
            emit(YK_KEEP_EV_PAIRED);
        }
        return rc;
    }
    case YK_BOND_ASK:
        printf("YK_KEEP bond ask new=%s\n", a);
        esp_timer_start_once(s_ask_timer, (uint64_t)(s_cfg.ask_seconds > 0 ? s_cfg.ask_seconds : 30) * 1000000ULL);
        emit(YK_KEEP_EV_ASK);
        __attribute__((fallthrough));  // held like the rest of its writes
    case YK_BOND_HOLD:
        if (s_held_n < HELD_MAX) s_held[s_held_n++] = (held_t){.type = type, .v = *val};
        return 0;  // NimBLE goes on as if kept; nothing is until the answer says yes
    case YK_BOND_DROP:
    default:
        printf("YK_KEEP bond drop third=%s\n", a);
        return 0;
    }
}

static int store_delete(int type, const union ble_store_key *key) {
    const int rc = s_delete(type, key);
    if (s_bypass || rc != 0) return rc;
    if (type == BLE_STORE_OBJ_TYPE_CCCD) {
        for (int i = 0; i < s_mirror.n_cccd; i++) {
            if (s_mirror.cccd[i].chr_val_handle != key->cccd.chr_val_handle) continue;
            s_mirror.cccd[i] = s_mirror.cccd[--s_mirror.n_cccd];
            mirror_save();
            break;
        }
    } else if (type == BLE_STORE_OBJ_TYPE_PEER_SEC && s_mirror.have_peer &&
               (ble_addr_cmp(&key->sec.peer_addr, BLE_ADDR_ANY) == 0 ||
                ble_addr_cmp(&key->sec.peer_addr, &s_mirror.peer.peer_addr) == 0)) {
        mirror_clear();
        mirror_save();
        yk_bond_forget(&s_bond);
    }
    return rc;
}

// ---------------------------------------------------------------- the answer, on the host task

static struct ble_npl_event s_answer_ev;
static volatile bool s_answer_yes;

static void answer_on_host(struct ble_npl_event *ev) {
    (void)ev;
    if (!s_bond.asking) return;
    esp_timer_stop(s_ask_timer);
    const bool yes = s_answer_yes;
    const bool repairing = s_bond.repairing;
    const yk_bond_addr_t old = s_bond.phone, asked = s_bond.asked;
    yk_bond_answer(&s_bond, yes);
    if (yes) {
        // The old phone out of the store (and the mirror), then the new one's held writes in.
        const ble_addr_t o = nimble_addr(&old);
        s_bypass = true;
        ble_store_util_delete_peer(&o);
        s_bypass = false;
        mirror_clear();
        for (int i = 0; i < s_held_n; i++) {
            if (s_write(s_held[i].type, &s_held[i].v) == 0) mirror_write(s_held[i].type, &s_held[i].v);
        }
        mirror_save();
        const ble_addr_t n = nimble_addr(&asked);
        char a[18], b[18];
        addr_text(&o, a);
        addr_text(&n, b);
        printf("YK_KEEP bond replace old=%s new=%s\n", a, b);
        s_trusted = s_enc && yk_link_keep_connected();
        emit(YK_KEEP_EV_REPLACED);
        if (s_trusted) emit(YK_KEEP_EV_TRUSTED);
    } else {
        if (repairing) mirror_restore();  // its old keys were taken out for the re-pairing
        printf("YK_KEEP bond refuse\n");
        emit(YK_KEEP_EV_REFUSED);
        const uint16_t conn = s_conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    memset(s_held, 0, sizeof(s_held));
    s_held_n = 0;
}

void yk_link_keep_answer(bool yes) {
    if (!s_active || !s_bond.asking) return;
    s_answer_yes = yes;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_answer_ev);
}

static void ask_timeout(void *arg) {
    (void)arg;
    printf("YK_KEEP bond ask_timeout\n");
    yk_link_keep_answer(false);
}

static struct ble_npl_event s_forget_ev;

static void forget_on_host(struct ble_npl_event *ev) {
    (void)ev;
    s_bypass = true;
    ble_store_clear();
    s_bypass = false;
    mirror_clear();
    mirror_save();
    yk_bond_forget(&s_bond);
    printf("YK_KEEP bond forget\n");
    const uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

void yk_link_keep_forget(void) {
    if (s_active) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_ev);
}

// ---------------------------------------------------------------- notifications and the key

static void notify_handle(uint16_t handle, const void *data, size_t len) {
    const uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    const uint16_t mtu = ble_att_mtu(conn);
    if (mtu > 3 && len > (size_t)mtu - 3) len = mtu - 3;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om) ble_gatts_notify_custom(conn, handle, om);
}

esp_err_t yk_link_keep_notify(int chr, const void *data, size_t len) {
    if (chr < 0 || chr >= s_cfg.n_chrs) return ESP_ERR_INVALID_ARG;
    if (!yk_link_keep_trusted() || !s_sub[chr]) return ESP_ERR_INVALID_STATE;
    notify_handle(s_chr_handle[chr], data, len);
    return ESP_OK;
}

static void release(void *arg) {
    (void)arg;
    s_report = 0;
    if (yk_link_keep_trusted()) notify_handle(s_report_handle, &s_report, 1);
}

esp_err_t yk_link_keep_press(void) {
    if (!s_cfg.hid || !yk_link_keep_trusted()) return ESP_ERR_INVALID_STATE;
    s_report = 0x01;
    notify_handle(s_report_handle, &s_report, 1);
    esp_timer_stop(s_release_timer);
    esp_timer_start_once(s_release_timer, 40000);  // 40 ms: no long press (an iPhone would start a video)
    return ESP_OK;
}

// ---------------------------------------------------------------- GATT

static int append(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len) {
    return os_mbuf_append(ctxt->om, data, len) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int app_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn, (void)attr;
    const int chr = (int)(intptr_t)arg;
    if (!yk_link_keep_trusted()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;  // a phone being asked about
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char buf[256];
        const int n = s_cfg.on_read ? s_cfg.on_read(chr, buf, sizeof(buf), s_cfg.ctx) : -1;
        const int rc = n < 0 ? BLE_ATT_ERR_UNLIKELY : append(ctxt, buf, (size_t)n);
        memset(buf, 0, sizeof(buf));  // it may have held a password
        return rc;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (s_cfg.on_write) s_cfg.on_write(chr, buf, n, s_cfg.ctx);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int std_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn, (void)attr;
    const bool read = ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR;
    switch ((int)(intptr_t)arg) {
    case H_INFO: return append(ctxt, HID_INFO, sizeof(HID_INFO));
    case H_MAP: return append(ctxt, REPORT_MAP, sizeof(REPORT_MAP));
    case H_CTRL: return 0;  // suspend / exit suspend: nothing to do
    case H_REPORT: return read ? append(ctxt, &s_report, 1) : 0;
    case H_PROTO: {
        const uint8_t report_protocol = 1;
        return read ? append(ctxt, &report_protocol, 1) : 0;
    }
    case B_LEVEL: return append(ctxt, &s_battery, 1);
    case D_PNP: return append(ctxt, PNP_ID, sizeof(PNP_ID));
    case D_MANUF: return append(ctxt, "Yaykit", 6);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int report_ref(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn, (void)attr, (void)arg;
    static const uint8_t ref[] = {0x01, 0x01};  // report 1, input
    return append(ctxt, ref, sizeof(ref));
}

#define ENC_R (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
static const struct ble_gatt_chr_def HID_CHRS[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = std_access, .arg = (void *)(intptr_t)H_INFO, .flags = ENC_R},
    {.uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = std_access, .arg = (void *)(intptr_t)H_MAP, .flags = ENC_R},
    {.uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = std_access, .arg = (void *)(intptr_t)H_CTRL,
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
    {.uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = std_access, .arg = (void *)(intptr_t)H_REPORT,
     .flags = ENC_R | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_report_handle,
     .descriptors = (struct ble_gatt_dsc_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = report_ref},
         {0}}},
    {.uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = std_access, .arg = (void *)(intptr_t)H_PROTO,
     .flags = ENC_R | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
    {0},
};
static const struct ble_gatt_chr_def BATTERY_CHRS[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = std_access, .arg = (void *)(intptr_t)B_LEVEL, .flags = BLE_GATT_CHR_F_READ},
    {0},
};
static const struct ble_gatt_chr_def INFO_CHRS[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = std_access, .arg = (void *)(intptr_t)D_PNP, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = std_access, .arg = (void *)(intptr_t)D_MANUF, .flags = BLE_GATT_CHR_F_READ},
    {0},
};

static void build_gatt(void) {
    memset(s_chr_defs, 0, sizeof(s_chr_defs));
    memset(s_svcs, 0, sizeof(s_svcs));
    s_svc_uuid.u.type = BLE_UUID_TYPE_128;
    memcpy(s_svc_uuid.value, s_cfg.service, 16);
    for (int i = 0; i < s_cfg.n_chrs; i++) {
        s_chr_uuid[i].u.type = BLE_UUID_TYPE_128;
        memcpy(s_chr_uuid[i].value, s_cfg.chrs[i].uuid, 16);
        uint16_t f = 0;
        if (s_cfg.chrs[i].flags & YK_KEEP_READ) f |= BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC;
        if (s_cfg.chrs[i].flags & YK_KEEP_WRITE) f |= BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
        if (s_cfg.chrs[i].flags & YK_KEEP_NOTIFY) f |= BLE_GATT_CHR_F_NOTIFY;
        s_chr_defs[i] = (struct ble_gatt_chr_def){.uuid = &s_chr_uuid[i].u, .access_cb = app_access,
                                                  .arg = (void *)(intptr_t)i, .flags = f, .val_handle = &s_chr_handle[i]};
    }
    int k = 0;
    s_svcs[k++] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_svc_uuid.u,
                                            .characteristics = s_chr_defs};
    if (s_cfg.hid)
        s_svcs[k++] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
                                                .characteristics = HID_CHRS};
    s_svcs[k++] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180F),
                                            .characteristics = BATTERY_CHRS};
    s_svcs[k++] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180A),
                                            .characteristics = INFO_CHRS};
}

// ---------------------------------------------------------------- GAP

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void) {
    if (!s_active || yk_link_keep_connected()) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;  // the phone finds it by this
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    f.appearance = APPEARANCE;
    f.appearance_is_present = 1;
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)s_cfg.name;
    r.name_len = strlen(s_cfg.name);
    r.name_is_complete = 1;
    ble_uuid16_t hid = BLE_UUID16_INIT(0x1812);
    if (s_cfg.hid) {
        r.uuids16 = &hid;
        r.num_uuids16 = 1;
        r.uuids16_is_complete = 0;
    }
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = 160, .itvl_max = 240};  // 100-150 ms
    const uint8_t own = s_addr_ok ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
    if (!rc) rc = ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "advertising: %d", rc);
}

#if MYNEWT_VAL(BLE_GATTC)
static int on_name(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    (void)conn, (void)arg;
    if (err->status != 0 || !attr || s_peer_name[0]) return 0;
    char name[sizeof(s_peer_name)];
    uint16_t n = 0;
    ble_hs_mbuf_to_flat(attr->om, name, sizeof(name) - 1, &n);
    name[n] = 0;
    for (char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20) *p = ' ';
    // A UTF-8 character cut at the end by the buffer: drop its leading bytes.
    size_t len = strlen(name);
    size_t back = 0;
    while (back < len && back < 4 && ((unsigned char)name[len - 1 - back] & 0xC0) == 0x80) back++;
    if (back < len && ((unsigned char)name[len - 1 - back] & 0xC0) == 0xC0) {
        const unsigned char lead = (unsigned char)name[len - 1 - back];
        const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (back + 1 < need) name[len - 1 - back] = 0;
    }
    snprintf(s_peer_name, sizeof(s_peer_name), "%s", name);
    printf("YK_KEEP peer name_len=%u\n", (unsigned)strlen(s_peer_name));  // the name is the user's: not logged
    emit(YK_KEEP_EV_NAME);
    return 0;
}
#endif

static void set_untrusted(void) {
    s_enc = s_trusted = s_sub_report = false;
    memset((void *)s_sub, 0, sizeof(s_sub));
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    struct ble_gap_conn_desc d;
    char a[18];
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        set_untrusted();
        s_peer_name[0] = 0;
        printf("YK_KEEP connected\n");
        emit(YK_KEEP_EV_CONNECTED);
        {
            // 30-50 ms: key presses stay quick and Wi-Fi keeps most of the air while the viewfinder streams.
            const struct ble_gap_upd_params up = {.itvl_min = 24, .itvl_max = 40, .latency = 0, .supervision_timeout = 400};
            ble_gap_update_params(s_conn, &up);
        }
#if MYNEWT_VAL(BLE_GATTC)
        ble_gattc_read_by_uuid(s_conn, 1, 0xFFFF, BLE_UUID16_DECLARE(0x2A00), on_name, NULL);
#endif
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (s_bond.asking) {  // the new phone went away before an answer: nothing kept
            printf("YK_KEEP bond refuse why=gone\n");
            esp_timer_stop(s_ask_timer);
            const bool repairing = s_bond.repairing;
            yk_bond_answer(&s_bond, false);
            if (repairing) mirror_restore();
            memset(s_held, 0, sizeof(s_held));
            s_held_n = 0;
            emit(YK_KEEP_EV_REFUSED);
        } else if (s_bond.repairing) {  // re-pairing that never got to its keys: the old ones back
            s_bond.repairing = false;
            mirror_restore();
        }
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        set_untrusted();
        printf("YK_KEEP disconnected reason=0x%x\n", ev->disconnect.reason);
        emit(YK_KEEP_EV_DISCONNECTED);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_enc = ev->enc_change.status == 0;
        if (!s_enc) {
            if (s_bond.have) {
                // Keys this device no longer has (a replaced or forgotten phone): let it go, never wait on it.
                printf("YK_KEEP bond unknown status=%d\n", ev->enc_change.status);
                emit(YK_KEEP_EV_UNKNOWN);
            } else {
                printf("YK_KEEP pair failed status=%d\n", ev->enc_change.status);
                emit(YK_KEEP_EV_PAIR_FAILED);
            }
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            const yk_bond_addr_t id = bond_addr(&d.peer_id_addr);
            s_trusted = !s_bond.asking && s_bond.have && yk_bond_same(&id, &s_bond.phone);
            addr_text(&d.peer_id_addr, a);
            printf("YK_KEEP encrypted bonded=%d trusted=%d id=%s\n", d.sec_state.bonded, s_trusted, a);
        }
        if (s_trusted) emit(YK_KEEP_EV_TRUSTED);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        const uint16_t h = ev->subscribe.attr_handle;
        const bool on = ev->subscribe.cur_notify;
        if (h == s_report_handle) s_sub_report = on;
        for (int i = 0; i < s_cfg.n_chrs; i++)
            if (h == s_chr_handle[i]) s_sub[i] = on;
        emit(YK_KEEP_EV_SUBSCRIBED);
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The remembered phone forgot this device and pairs again. Its old keys come out of the store (NimBLE needs
        // that to go on) but stay remembered until the screen's answer; anything else is let go.
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            const yk_bond_addr_t id = bond_addr(&d.peer_id_addr);
            if (yk_bond_repair(&s_bond, &id)) {
                s_bypass = true;
                ble_store_util_delete_peer(&d.peer_id_addr);
                s_bypass = false;
                printf("YK_KEEP bond repairing\n");
                return BLE_GAP_REPEAT_PAIRING_RETRY;
            }
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void) {
    if (s_addr_ok && ble_hs_id_set_rnd(s_addr) != 0) s_addr_ok = false;  // then the public address
    ble_hs_util_ensure_addr(0);
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset %d", reason); }

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/** Our static random address: made once (the two top bits set, not all ones), kept in the app's namespace. */
static void address_load(void) {
    nvs_handle_t h;
    size_t n = sizeof(s_addr);
    if (nvs_open(s_cfg.nvs_ns, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "bt_addr", s_addr, &n) == ESP_OK && n == sizeof(s_addr) && (s_addr[5] & 0xC0) == 0xC0) {
        s_addr_ok = true;
    } else {
        do {
            esp_fill_random(s_addr, sizeof(s_addr));
            s_addr[5] |= 0xC0;
        } while (s_addr[0] == 0xFF && s_addr[1] == 0xFF && s_addr[2] == 0xFF && s_addr[3] == 0xFF && s_addr[4] == 0xFF);
        s_addr_ok = nvs_set_blob(h, "bt_addr", s_addr, sizeof(s_addr)) == ESP_OK && nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
}

esp_err_t yk_link_keep_start(const yk_link_keep_config_t *cfg) {
    if (s_active) return ESP_OK;
    if (!cfg || !cfg->nvs_ns || !cfg->name || cfg->n_chrs < 1 || cfg->n_chrs > YK_KEEP_CHR_MAX ||
        strlen(cfg->name) > YK_KEEP_NAME_MAX)
        return ESP_ERR_INVALID_ARG;
    if (yk_link_ble_active()) return ESP_ERR_INVALID_STATE;
    s_cfg = *cfg;
    yk_link_keep_running = true;
    yk_bond_init(&s_bond);
    address_load();
    mirror_load();
    if (s_mirror.have_peer) {
        s_bond.have = true;
        s_bond.phone = bond_addr(&s_mirror.peer.peer_addr);
    }
    const esp_timer_create_args_t rel = {.callback = release, .name = "yk_keep_hid"};
    const esp_timer_create_args_t ask = {.callback = ask_timeout, .name = "yk_keep_ask"};
    if (!s_release_timer && esp_timer_create(&rel, &s_release_timer) != ESP_OK) goto fail;
    if (!s_ask_timer && esp_timer_create(&ask, &s_ask_timer) != ESP_OK) goto fail;
    if (nimble_port_init() != ESP_OK) goto fail;
    ble_npl_event_init(&s_answer_ev, answer_on_host, NULL);
    ble_npl_event_init(&s_forget_ev, forget_on_host, NULL);
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;  // Just Works, as a shutter remote
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    build_gatt();
    if (ble_gatts_count_cfg(s_svcs) || ble_gatts_add_svcs(s_svcs)) {
        ESP_LOGE(TAG, "GATT table refused");
        goto fail;
    }
    ble_svc_gap_device_name_set(s_cfg.name);
    ble_svc_gap_device_appearance_set(APPEARANCE);
    ble_store_config_init();
    s_write = ble_hs_cfg.store_write_cb;
    s_delete = ble_hs_cfg.store_delete_cb;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
    mirror_restore();  // before the host runs: the phone's IRK is resolved from the first connection
    s_active = true;
    nimble_port_freertos_init(host_task);
    printf("YK_KEEP start remembered=%d addr=%s\n", s_bond.have, s_addr_ok ? "random" : "public");
    return ESP_OK;
fail:
    yk_link_keep_running = false;
    return ESP_FAIL;
}

void yk_link_keep_end(int wait_ms) {
    if (!s_active) return;
    s_active = false;  // no advertising from here on
    ble_gap_adv_stop();
    const uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int t = 0; t < wait_ms && yk_link_keep_connected(); t += 10) vTaskDelay(pdMS_TO_TICKS(10));
    printf("YK_KEEP end connected=%d\n", yk_link_keep_connected());
}
