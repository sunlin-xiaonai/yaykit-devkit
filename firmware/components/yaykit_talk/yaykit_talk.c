// yaykit_talk's session (the main firmware's yay_run.c, its conversation part): the session's messages and audio
// go into a queue in the order they came, the side key into one of its own (always handled first: a press must stop
// the reply and open the microphones at once); one loop feeds the turn state machine (yk_talk_sm.c), carries out its
// effects and tells the app. The device's settings by voice are done here (internal-commands.md 5.4).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "board.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yaykit_account.h"
#include "yaykit_app.h"
#include "yaykit_session.h"
#include "yaykit_speak.h"
#include "yaykit_talk.h"
#include "yk_talk_close.h"
#include "yk_talk_hello.h"
#include "yk_talk_internal.h"
#include "yk_talk_msg.h"
#include "yk_mem.h"
#include "yk_talk_setting.h"
#include "yk_talk_setup.h"

static const char *TAG = "yk_talk";

#define INPUT_LEN 16
#define CTL_LEN 48
#define PACKET_MAX 400
#define TEXT_MAX 512  // a stt or reply.line text (reply.line is at most 120 characters, ~360 bytes in Chinese), cut
                      // before a character
#define TICK_MS 100
#define STACK 5120  // internal: the settings write NVS (no PSRAM stack then); the app's on_event runs here too

typedef enum { IN_KEY_DOWN, IN_KEY_HOLD, IN_KEY_UP, IN_KEY_SHORT, IN_CANCEL, IN_STOP_REPLY, IN_PREFS } in_t;
typedef struct {
    in_t type;
    int64_t at_ms;  // keys: yaykit_key's at_ms (a delayed TAP's is its own press's end)
} input_t;

typedef enum { Q_SRV, Q_NET, Q_AUDIO, Q_ACTION, Q_MSG } q_kind_t;
typedef struct {
    q_kind_t kind;
    yk_tsm_input_type_t in;  // Q_SRV, Q_NET
    char turn[YK_TALK_TURN_LEN + 1], text[TEXT_MAX], lang[8], muted[12], code[32], id[41];
    int line, credits;
    bool fix;
    int act_kind, level, step, mute, seconds;  // Q_ACTION (yk_srv_msg_t)
    yk_srv_msg_t *msg;  // Q_NET hello.ok, Q_MSG practice.turn: a copy in PSRAM, the item's own (yk_talk_msg_copy)
    uint16_t bytes;
    uint8_t packet[PACKET_MAX];
} q_item_t;

static struct {
    yk_talk_config_t cfg;
    char slug[32], version[24], mode[12], locale[8], url[96], fw[64];
    yk_talk_setup_t setup;  // the next hello's language, level, pair (under `lock`): the config's, or yk_talk_set's
    char hello_language[8], hello_level[12];  // what the last hello carried (under `lock`)
    portMUX_TYPE lock;  // language, level, pair: yk_talk_set() (the app) against make_hello() (the session task)
    bool production;
    QueueHandle_t input, ctl;
    TaskHandle_t task;
    q_item_t *in_msg, *in_audio, *loop_item;  // one each for the session task, its WebSocket client, the loop (PSRAM)
    q_item_t *in_state;                       // on_state's (either task), under state_lock: too big for their stacks
    SemaphoreHandle_t state_lock;
    yk_tsm_t sm;
    bool speak_ended;  // the reply's end has been given to the speaker for this turn
    int credits;       // the account's points left, as last heard (hello.ok, reply.end, an error); -1 not yet
    char muted[12];    // this turn's reply.end's, until its END
    int unmute;        // the volume before a mute (RAM only)
    int64_t prefs_due_ms;  // the loop's
    volatile bool held;
    volatile bool going_home;  // app.home: leaving for Yay because the phone asked (the app's wrap-up reads it)
#if CONFIG_YAY_DEV
    char dev_say[256];  // `yay say`: the words it stands for, sent as dev.say to a development server
#endif
} T = {.unmute = -1, .credits = -1, .lock = portMUX_INITIALIZER_UNLOCKED};

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void copy(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (; in && in[n] && n + 1 < size; n++) out[n] = in[n];
    out[n] = 0;
}

static const char *str(const char *s) { return s && s[0] ? s : NULL; }

static void emit(yk_talk_event_t *ev) {
    ev->state = T.sm.state;
    if (!ev->turn) ev->turn = T.sm.turn;
    if (!ev->text) ev->text = "";
    if (!ev->lang) ev->lang = "";
    if (!ev->muted) ev->muted = "";
    if (T.cfg.on_event) T.cfg.on_event(ev, T.cfg.ctx);
}

/** internal_min: the lowest since the app was ready (yk_mem.h, the 40 KB line); largest: the biggest block one
 *  allocation can still get. */
static void mem_line(const char *why) {
    printf("YK_TALK mem internal=%u internal_min=%u largest=%u psram=%u why=%s\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)yk_mem_min_since_ready(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), why);
}

// ---------------------------------------------------------------- inputs

static void post_input_at(in_t type, int64_t at_ms) {
    const input_t in = {.type = type, .at_ms = at_ms};
    if (!T.input || xQueueSend(T.input, &in, 0) != pdTRUE) ESP_LOGE(TAG, "key queue full");
    if (T.task) xTaskNotifyGive(T.task);
}

static void post_input(in_t type) { post_input_at(type, now_ms()); }

/** Control messages are never dropped while the loop lives: the sender waits (a second at most). Audio waits less:
 *  a frame lost is better than holding up the session. */
static void post_ctl(const q_item_t *item) {
    const TickType_t wait = item->kind == Q_AUDIO ? pdMS_TO_TICKS(200) : pdMS_TO_TICKS(1000);
    if (!T.ctl || xQueueSend(T.ctl, item, wait) != pdTRUE) {
        if (item->kind != Q_AUDIO) ESP_LOGE(TAG, "event queue stuck: a message was dropped");
        if (item->msg) {
            yk_proto_free(item->msg);
            free(item->msg);
        }
    }
    if (T.task) xTaskNotifyGive(T.task);
}

bool yk_talk_key(const yk_key_event_t *ev) {
    if (!T.task) return false;
    switch (ev->type) {
    case YK_KEY_DOWN: post_input_at(IN_KEY_DOWN, ev->at_ms); return true;
    case YK_KEY_HOLD: post_input_at(IN_KEY_HOLD, ev->at_ms); return true;
    case YK_KEY_UP: post_input_at(IN_KEY_UP, ev->at_ms); return true;
    default: post_input_at(IN_KEY_SHORT, ev->at_ms); return false;  // TAP, DOUBLE: the app's (and yaykit_app's ring)
    }
}

void yk_talk_cancel(void) { post_input(IN_CANCEL); }
void yk_talk_stop_reply(void) { post_input(IN_STOP_REPLY); }
void yk_talk_prefs_changed(void) { post_input(IN_PREFS); }
yk_talk_state_t yk_talk_state(void) { return T.sm.state; }
bool yk_talk_going_home(void) { return T.going_home; }

bool yk_talk_hold(bool hold) {
    if (!T.task) return false;
    T.held = hold;
    if (!yk_session_hold(hold)) return false;  // the USB bridge is never held
    if (!hold) return true;
    // Its on_state (down) ends whatever goes on, in the loop; the TLS is closed once it says down. 3 s at most (a
    // connection being torn down), as Yay waits before the phone link.
    for (int i = 0; i < 300; i++) {
        const yk_session_state_t state = yk_session_state();
        if (state == YK_SESSION_DOWN || state == YK_SESSION_IDLE) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

esp_err_t yk_talk_send(char *json) {
    if (!T.task) {
        free(json);
        return ESP_ERR_INVALID_STATE;
    }
    return yk_talk_mic_post_send(json) ? ESP_OK : ESP_FAIL;
}

esp_err_t yk_talk_flush(uint32_t timeout_ms) { return T.task ? yk_talk_mic_flush(timeout_ms) : ESP_ERR_INVALID_STATE; }

static bool close_up(void *ctx) { return (void)ctx, yk_session_state() == YK_SESSION_UP; }

static yk_flush_result_t close_flush(uint32_t timeout_ms, void *ctx) {
    (void)ctx;
    const esp_err_t err = yk_talk_mic_flush(timeout_ms);
    return err == ESP_OK ? YK_FLUSH_SENT : err == ESP_ERR_TIMEOUT ? YK_FLUSH_TIMEOUT : YK_FLUSH_FAILED;
}

static yk_close_result_t close_session(uint32_t timeout_ms, void *ctx) {
    (void)ctx;
    const esp_err_t err = yk_session_close(timeout_ms);
    return err == ESP_OK ? YK_CLOSE_REPLIED : err == ESP_ERR_TIMEOUT ? YK_CLOSE_TIMEOUT : YK_CLOSE_GONE;
}

static int64_t close_now(void *ctx) { return (void)ctx, esp_timer_get_time() / 1000; }

bool yk_talk_close(uint32_t timeout_ms) {
    const int64_t t0 = esp_timer_get_time();
    const yk_close_env_t env = {.up = close_up, .flush = close_flush, .close = close_session, .now_ms = close_now};
    const yk_close_result_t r = T.task ? yk_close_run(&env, timeout_ms) : YK_CLOSE_GONE;
    printf("YK_TALK close=%s ms=%d\n", yk_close_result_name(r), (int)((esp_timer_get_time() - t0) / 1000));
    return r == YK_CLOSE_REPLIED;
}

void yk_talk_set(const char *language, const char *level, const char *pair) {
    taskENTER_CRITICAL(&T.lock);
    yk_talk_setup_choose(&T.setup, language, level, pair);
    taskEXIT_CRITICAL(&T.lock);
}

void yk_talk_hello_setup(char *language, size_t language_size, char *level, size_t level_size) {
    taskENTER_CRITICAL(&T.lock);
    if (language && language_size) copy(language, language_size, T.hello_language);
    if (level && level_size) copy(level, level_size, T.hello_level);
    taskEXIT_CRITICAL(&T.lock);
}

// ---------------------------------------------------------------- the session

static void on_state(yk_session_state_t state, void *ctx) {
    (void)ctx;
    if (state == YK_SESSION_UP) return;  // up counts from hello.ok
    xSemaphoreTake(T.state_lock, portMAX_DELAY);
    memset(T.in_state, 0, sizeof(*T.in_state));
    T.in_state->kind = Q_NET;
    T.in_state->in = YK_TSM_NET_DOWN;
    T.in_state->credits = -1;
    post_ctl(T.in_state);
    xSemaphoreGive(T.state_lock);
}

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

/** A copy for the queue (PSRAM), or NULL when out of memory. */
static yk_srv_msg_t *copy_msg(const yk_srv_msg_t *m) {
    yk_srv_msg_t *c = psram_alloc(sizeof(*c));
    if (c && yk_talk_msg_copy(c, m, psram_alloc)) return c;
    free(c);
    ESP_LOGE(TAG, "%s not passed on: no memory", yk_proto_type_name(m->type));
    return NULL;
}

static void on_message(const yk_srv_msg_t *m, void *ctx) {
    (void)ctx;
    q_item_t *q = T.in_msg;  // one sender (the session task)
    memset(q, 0, sizeof(*q));
    q->kind = Q_SRV;
    q->credits = -1;
    copy(q->turn, sizeof(q->turn), m->turn);
    yk_talk_copy_cut(q->text, sizeof(q->text), m->text);
    copy(q->lang, sizeof(q->lang), m->lang);
    switch (m->type) {
    case YK_SRV_HELLO_OK: {
        struct timeval tv = {.tv_sec = (time_t)(m->time_ms / 1000), .tv_usec = (suseconds_t)(m->time_ms % 1000) * 1000};
        settimeofday(&tv, NULL);  // the server's clock
        printf("YK_TALK hello.ok credits=%d\n", m->credits_remaining);
        mem_line("hello");  // connected: what Wi-Fi and the session took, before any turn
        q->kind = Q_NET;
        q->in = YK_TSM_NET_UP;
        q->credits = m->credits_remaining;
        q->msg = copy_msg(m);  // the app hears of it from the loop (YK_TALK_EV_MESSAGE)
        break;
    }
    case YK_SRV_STT_PARTIAL: q->in = YK_TSM_STT_PARTIAL; break;
    case YK_SRV_STT_FINAL: q->in = YK_TSM_STT_FINAL; break;
    case YK_SRV_REPLY_LINE:
        q->in = YK_TSM_REPLY_LINE;
        q->line = m->line;
        q->fix = m->fix_line;
        break;
    case YK_SRV_REPLY_END:
        q->in = YK_TSM_REPLY_END;
        copy(q->muted, sizeof(q->muted), m->muted);
        q->credits = m->has_credits ? m->credits_remaining : -1;
        break;
    case YK_SRV_ERROR:
        q->in = YK_TSM_ERROR;
        copy(q->code, sizeof(q->code), m->code);
        q->credits = m->has_credits ? m->credits_remaining : -1;
        if (T.production && !strcmp(m->code, "TOKEN_EXPIRED")) yk_account_token_expired();
        break;
    case YK_SRV_ACTION:
        q->kind = Q_ACTION;
        copy(q->id, sizeof(q->id), m->action_id);
        q->act_kind = m->kind;
        q->level = m->level;
        q->step = m->step;
        q->mute = m->mute;
        q->seconds = m->seconds;
        break;
    case YK_SRV_PRACTICE_TURN:
        // Through the queue, after the turn's lines before it, and only while its turn is open (yk_tsm_takes_message).
        q->kind = Q_MSG;
        q->msg = copy_msg(m);
        if (!q->msg) return;
        break;
    default:
        printf("YK_TALK %s\n", yk_proto_type_name(m->type));  // ui.state, inbox, app.records.ok: Yay's own
        return;
    }
    post_ctl(q);
}

static void on_audio(int line, int seq, const uint8_t *packet, size_t bytes, void *ctx) {
    (void)seq, (void)ctx;
    if (bytes > PACKET_MAX) return;
    q_item_t *q = T.in_audio;  // one sender (the WebSocket client)
    q->kind = Q_AUDIO;
    q->msg = NULL;
    q->line = line;
    q->bytes = (uint16_t)bytes;
    memcpy(q->packet, packet, bytes);
    post_ctl(q);
}

/** The session's token: the app's own in production; none for a development server (nothing asks the cloud). */
static esp_err_t token(char *out, size_t size, bool fresh) {
    if (!T.production) {
        out[0] = 0;
        return ESP_OK;
    }
    if (fresh) yk_account_token_expired();
    const char *code = NULL;
    return yk_account_token(out, size, &code);
}

static char *make_hello(void *ctx) {
    (void)ctx;
    char locale[8];
    if (T.locale[0]) copy(locale, sizeof(locale), T.locale);
    else if (T.production) yk_account_locale(locale, sizeof(locale));
    else copy(locale, sizeof(locale), "zh");
    board_battery_t b = {.percent = -1};
    const bool battery = board_battery(&b) == ESP_OK;
    wifi_ap_record_t ap;
    const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 1;
    char language[sizeof(T.setup.language)], level[sizeof(T.setup.level)], pair[sizeof(T.setup.pair)];
    taskENTER_CRITICAL(&T.lock);
    copy(language, sizeof(language), T.setup.language);
    copy(level, sizeof(level), T.setup.level);
    copy(pair, sizeof(pair), T.setup.pair);
    copy(T.hello_language, sizeof(T.hello_language), language);  // as this hello says them (yk_talk_hello_setup)
    copy(T.hello_level, sizeof(T.hello_level), level);
    taskEXIT_CRITICAL(&T.lock);
    const yk_talk_hello_in_t h = {.fw = T.fw, .locale = locale, .battery_pct = battery ? b.percent : -1,
                                  .charging = battery && b.charging, .rssi = rssi, .volume = board_pref_volume(),
                                  .brightness_pct = board_pref_brightness(), .screen_off_s = board_pref_screen_off_s(),
                                  .mode = str(T.mode), .language = str(language), .level = str(level),
                                  .pair = str(pair)};
    char *json = yk_talk_hello(&h);
    if (!json) ESP_LOGE(TAG, "hello: the mode's setup breaks the schema (mode=%s)", T.mode);
    printf("YK_TALK hello mode=%s language=%s level=%s pair=%s\n", str(T.mode) ? T.mode : "chat", language, level, pair);
    return json;
}

// ---------------------------------------------------------------- the stream's sink: the session

static bool sink_begin(const char *turn, int preroll_ms, void *ctx) {
    (void)ctx;
    yk_session_new_turn();
    if (yk_session_send(yk_proto_listen_start(turn, preroll_ms, NULL)) == ESP_OK) return true;
    ESP_LOGW(TAG, "listen.start not sent");
    return false;
}

static void sink_packet(const uint8_t *opus, size_t bytes, void *ctx) {
    (void)ctx;
    yk_session_send_audio(opus, bytes);
}

static void sink_end(const char *turn, yk_talk_mic_end_t how, void *ctx) {
    (void)ctx;
    if (how == YK_TALK_MIC_STOPPED) yk_session_send(yk_proto_turn_message("listen.stop", turn));
    else if (how == YK_TALK_MIC_CANCELLED) yk_session_send(yk_proto_turn_message("listen.cancel", turn));
}

static const yk_talk_sink_t SESSION_SINK = {.begin = sink_begin, .packet = sink_packet, .end = sink_end};

// ---------------------------------------------------------------- effects


static void apply(const yk_tsm_fx_t *f, const q_item_t *q) {
    switch (f->type) {
    case YK_TSM_MIC_OPEN:
        yk_speak_prepare();  // the amplifier needs 0.3 s: start it now for the cue and the reply
        yk_talk_mic_open();
#if CONFIG_YAY_DEV
        mem_line("mic");  // the front end and the I2S's larger DMA buffers
#endif
        break;
    case YK_TSM_MIC_DISCARD: yk_talk_mic_discard(); break;
    case YK_TSM_LISTEN_START:
#if CONFIG_YAY_DEV
        if (T.dev_say[0]) {  // development servers only: what `yay say` stands for, before listen.start
            if (!T.production) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "type", "dev.say");
                cJSON_AddStringToObject(o, "text", T.dev_say);
                const bool sent = yk_talk_mic_post_send(cJSON_PrintUnformatted(o));
                cJSON_Delete(o);
                printf("YK_TALK dev.say bytes=%u sent=%d\n", (unsigned)strlen(T.dev_say), sent);
            }
            T.dev_say[0] = 0;
        }
#endif
        T.muted[0] = 0;  // a new turn: nothing of the one before
        yk_talk_mic_stream(f->turn, &SESSION_SINK);
        printf("YK_TALK turn id=%s stage=listening\n", f->turn);
        mem_line("listen");  // the microphones open, before the turn's audio and reply
        break;
    case YK_TSM_LISTEN_STOP:
        yk_talk_mic_stop();
        yk_speak_prepare();  // for the reply
        break;
    case YK_TSM_LISTEN_CANCEL: yk_talk_mic_cancel(); break;
    case YK_TSM_PLAYBACK_STOP: yk_talk_mic_post_send(yk_proto_turn_message("playback.stop", f->turn)); break;
    case YK_TSM_SPEAK_BEGIN:
        yk_talk_play_begin();
        T.speak_ended = false;
#if CONFIG_YAY_DEV
        mem_line("speak");  // the reply's first sentence: the decoder, the speaker's format
#endif
        break;
    case YK_TSM_SPEAK_END:
        yk_talk_play_end();  // after the audio already queued
        T.speak_ended = true;
        break;
    case YK_TSM_SPEAK_STOP: yk_talk_play_stop(); break;
    case YK_TSM_CHIME_START:
    case YK_TSM_CHIME_END:
        if (!T.cfg.no_chimes) yk_talk_play_cue(f->type == YK_TSM_CHIME_START);
        break;
    case YK_TSM_PARTIAL:
    case YK_TSM_HEARD:
    case YK_TSM_LINE: {
        yk_talk_event_t ev = {.type = f->type == YK_TSM_PARTIAL ? YK_TALK_EV_PARTIAL
                                      : f->type == YK_TSM_HEARD ? YK_TALK_EV_HEARD
                                                                : YK_TALK_EV_LINE,
                              .turn = f->turn, .text = q ? q->text : "", .lang = q ? q->lang : "",
                              .line = q ? q->line : 0, .fix = q && q->fix};
        emit(&ev);
        break;
    }
    case YK_TSM_ENDED: {
        yk_talk_event_t ev = {.type = YK_TALK_EV_END, .turn = f->turn, .end = f->end, .muted = T.muted,
                              .credits = T.credits};
        printf("YK_TALK turn id=%s stage=end end=%s\n", f->turn, yk_talk_end_name(f->end));
        mem_line("turn");
        emit(&ev);
        T.muted[0] = 0;
        break;
    }
    }
}

/** What the app hears of (an event, possibly drawn under the board lock), not what the hardware does. */
static bool tells_app(yk_tsm_fx_type_t type) {
    return type == YK_TSM_PARTIAL || type == YK_TSM_HEARD || type == YK_TSM_LINE || type == YK_TSM_ENDED;
}

/** One input to the state machine: the speaker, the microphones and the messages first (a press during the reply:
 *  quiet and recording before the app draws anything, or the preroll comes short), then the app's events, in order,
 *  then a new state. */
static void feed(yk_tsm_input_type_t type, const q_item_t *q, int64_t at_ms) {
    const yk_talk_state_t before = T.sm.state;
    const yk_tsm_input_t in = {.type = type, .now_ms = at_ms, .turn = q ? q->turn : NULL, .text = q ? q->text : NULL,
                               .code = q ? q->code : NULL, .muted = q ? q->muted : NULL};
    // This turn's reply.end, for the END that follows (now, or once played): not one stopped or cancelled already.
    if (type == YK_TSM_REPLY_END && q && T.sm.turn_open && !strcmp(q->turn, T.sm.turn))
        copy(T.muted, sizeof(T.muted), q->muted);
    if (q && q->credits >= 0) T.credits = q->credits;  // hello.ok, reply.end, an error: the account's points left
    yk_tsm_feed(&T.sm, &in);
    for (int i = 0; i < T.sm.fx_count; i++)
        if (!tells_app(T.sm.fx[i].type)) apply(&T.sm.fx[i], q);
    for (int i = 0; i < T.sm.fx_count; i++)
        if (tells_app(T.sm.fx[i].type)) apply(&T.sm.fx[i], q);
    if (T.sm.state != before) {
        printf("YK_TALK state from=%s to=%s\n", yk_talk_state_name(before), yk_talk_state_name(T.sm.state));
        yk_talk_event_t ev = {.type = YK_TALK_EV_STATE};
        emit(&ev);
    }
}

// ---------------------------------------------------------------- the device's settings by voice

static void setting(const q_item_t *q, const char *kind, int value) {
    printf("YK_TALK setting %s=%d id=%s\n", kind, value, q->id);
    yk_talk_mic_post_send(yk_proto_action_done_value(q->id, true, NULL, value));
    // Changed while nobody talks to it (from the phone): what it sounds like now, as Yay's settings page does
    // (internal commands, step 2, 3.3). Not mid-turn; not for an app that plays its own cues. The ticks are the app's.
    if (!strcmp(kind, "volume") && !T.cfg.no_chimes && T.sm.state == YK_TALK_IDLE) yk_talk_play_cue(false);
    yk_talk_event_t ev = {.type = YK_TALK_EV_SETTING, .kind = kind, .value = value};
    emit(&ev);
}

/** volume.set, brightness.set, screen_off.set; app.open and app.install are Yay's (BUSY), the rest UNSUPPORTED. */
static void do_action(const q_item_t *q) {
    switch (q->act_kind) {
    case YK_ACT_VOLUME: {
        const int v = yk_talk_volume_after(board_pref_volume(), q->level, q->step, q->mute, &T.unmute,
                                           BOARD_PREF_VOLUME_DEFAULT);
        yk_speak_volume(v);
        board_pref_set_volume(v);  // the device's (P10): every app opens with it
        setting(q, "volume", v);
        break;
    }
    case YK_ACT_BRIGHTNESS: {
        const int level = yk_talk_brightness_after(board_pref_brightness(), q->level, q->step);
        if (level < 0) {  // "auto" from an older server: there is no light sensor
            yk_talk_mic_post_send(yk_proto_action_done(q->id, false, "UNSUPPORTED"));
            break;
        }
        board_battery_t b = {.percent = -1};  // read before the board lock: drawing waits on it
        const bool read = board_battery(&b) == ESP_OK;
        if (board_lock(200) != ESP_OK) {  // the panel shares the SPI bus with drawing
            yk_talk_mic_post_send(yk_proto_action_done(q->id, false, "BUSY"));
            break;
        }
        const int pct = yk_talk_brightness_percent(level);
        board_pref_set_brightness(pct);
        // As Yay: a dark panel stays dark (lit at the new one when the app lights it); on battery at 15 % or less,
        // 60 % at most. Bringing a dark screen on to show it is the app's (YK_TALK_EV_SETTING).
        if (!board_display_asleep() && board_brightness_get() > 0)
            board_brightness_set(yk_talk_brightness_lit(pct, read ? b.percent : -1, read && b.usb));
        board_unlock();
        setting(q, "brightness", yk_talk_brightness_level(board_pref_brightness()));
        break;
    }
    case YK_ACT_SCREEN_OFF:
        board_pref_set_screen_off_s(q->seconds);  // the app's own screen-off follows it (yk_talk_event SETTING)
        setting(q, "screen_off", yk_talk_screen_off_s(board_pref_screen_off_s()));
        break;
    case YK_ACT_APP_OPEN:
    case YK_ACT_APP_INSTALL:
        yk_talk_mic_post_send(yk_proto_action_done(q->id, false, "BUSY"));  // only Yay opens and installs
        break;
    case YK_ACT_APP_HOME:
        // Back to Yay, asked from the phone (internal commands, step 2): not while a slot is being written; else the
        // answer first, through the uplink (300 ms at most, as practice.end), then leave as 双击 › 回到 Yay does.
        if (yk_app_leave_held()) {
            yk_talk_mic_post_send(yk_proto_action_done(q->id, false, "BUSY"));
            break;
        }
        yk_talk_mic_post_send(yk_proto_action_done(q->id, true, NULL));
        yk_talk_flush(300);
        printf("YK_TALK app_home id=%s\n", q->id);
        T.going_home = true;  // before the wrap-up runs (yk_app_on_leave, in yaykit_app's task)
        yk_return_home();     // does not return when it works
        T.going_home = false;
        break;
    default:
        yk_talk_mic_post_send(yk_proto_action_done(q->id, false, "UNSUPPORTED"));
        break;
    }
}

// ---------------------------------------------------------------- the loop

static void handle_input(const input_t *in) {
    static const yk_tsm_input_type_t map[] = {[IN_KEY_DOWN] = YK_TSM_KEY_DOWN, [IN_KEY_HOLD] = YK_TSM_KEY_HOLD,
                                              [IN_KEY_UP] = YK_TSM_KEY_UP, [IN_KEY_SHORT] = YK_TSM_KEY_SHORT,
                                              [IN_CANCEL] = YK_TSM_CANCEL, [IN_STOP_REPLY] = YK_TSM_STOP_REPLY};
    if (in->type == IN_PREFS) {
        T.prefs_due_ms = in->at_ms + 2000;  // 2 s after the last change
        return;
    }
    feed(map[in->type], NULL, in->at_ms);
}

static void tell_message(const yk_srv_msg_t *msg) {
    yk_talk_event_t ev = {.type = YK_TALK_EV_MESSAGE, .turn = msg->turn, .msg = msg};
    emit(&ev);
}

static void handle(q_item_t *q) {
    switch (q->kind) {
    case Q_NET:
        if (q->msg) tell_message(q->msg);  // hello.ok, before the state it brings
        feed(q->in, q, now_ms());
        break;
    case Q_SRV: feed(q->in, q, now_ms()); break;
    case Q_MSG:
        if (yk_tsm_takes_message(&T.sm, q->msg->turn)) tell_message(q->msg);
        break;
    case Q_AUDIO:
        if (T.sm.speaking && !T.speak_ended) yk_talk_play_opus(q->packet, q->bytes);
        break;
    case Q_ACTION: do_action(q); break;
    }
    if (q->msg) {
        yk_proto_free(q->msg);
        free(q->msg);
        q->msg = NULL;
    }
}

static void tick(void) {
    feed(YK_TSM_TICK, NULL, now_ms());
    // Played out: the end went through the playback task and the speaker is quiet.
    if (T.sm.speaking && T.speak_ended && yk_talk_play_done()) feed(YK_TSM_SPEAK_DONE, NULL, now_ms());
    if (T.prefs_due_ms && now_ms() >= T.prefs_due_ms) {
        T.prefs_due_ms = 0;
        // Not connected: the next hello carries them.
        if (yk_session_state() == YK_SESSION_UP)
            yk_talk_mic_post_send(yk_proto_prefs(board_pref_volume(), yk_talk_brightness_level(board_pref_brightness()),
                                                 yk_talk_screen_off_s(board_pref_screen_off_s())));
    }
}

static void run_task(void *arg) {
    (void)arg;
    int64_t next_tick = now_ms() + TICK_MS;
    for (;;) {
        input_t in;
        while (xQueueReceive(T.input, &in, 0) == pdTRUE) handle_input(&in);
        const bool got = xQueueReceive(T.ctl, T.loop_item, 0) == pdTRUE;
        if (got) handle(T.loop_item);
        if (now_ms() >= next_tick) {
            next_tick = now_ms() + TICK_MS;
            tick();
        }
        if (!got) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
    }
}

#if CONFIG_YAY_DEV
static void on_say(int seconds, const char *text) {
    (void)seconds;
    copy(T.dev_say, sizeof(T.dev_say), text);
    printf("YK_TALK say bytes=%u\n", (unsigned)strlen(T.dev_say));
}
#endif

esp_err_t yk_talk_start(const yk_talk_config_t *cfg) {
    if (T.task) return ESP_ERR_INVALID_STATE;
    if (!cfg || !cfg->slug || !cfg->version) return ESP_ERR_INVALID_ARG;
    T.cfg = *cfg;
    copy(T.slug, sizeof(T.slug), cfg->slug);
    copy(T.version, sizeof(T.version), cfg->version);
    copy(T.mode, sizeof(T.mode), cfg->mode);
    taskENTER_CRITICAL(&T.lock);
    yk_talk_setup_start(&T.setup, cfg->language, cfg->level, cfg->pair);  // what yk_talk_set chose already stays
    taskEXIT_CRITICAL(&T.lock);
    copy(T.locale, sizeof(T.locale), cfg->locale);
    snprintf(T.fw, sizeof(T.fw), "%s/%s", T.slug, T.version);
#if CONFIG_YAY_DEV
    copy(T.url, sizeof(T.url), cfg->url && cfg->url[0] ? cfg->url : YK_SESSION_URL);
    yk_app_dev_on_say(on_say);
#else
    copy(T.url, sizeof(T.url), YK_SESSION_URL);  // release builds: the production session only
#endif
    T.production = !strcmp(T.url, YK_SESSION_URL);
    T.cfg.slug = T.slug, T.cfg.version = T.version, T.cfg.mode = T.mode, T.cfg.language = T.setup.language;
    T.cfg.level = T.setup.level, T.cfg.pair = T.setup.pair, T.cfg.locale = T.locale, T.cfg.url = T.url;
    yk_tsm_init(&T.sm, cfg->max_hold_s, esp_random());

    // The microphones' and the playback's tasks stay once made (idempotent, below the session, apps may use them).
    esp_err_t err = yk_talk_mic_init();
    if (err == ESP_OK) err = yk_talk_play_init();
    if (err != ESP_OK) return err;
    T.input = xQueueCreate(INPUT_LEN, sizeof(input_t));
    T.ctl = xQueueCreateWithCaps(CTL_LEN, sizeof(q_item_t), MALLOC_CAP_SPIRAM);
    T.in_msg = heap_caps_calloc(1, sizeof(q_item_t), MALLOC_CAP_SPIRAM);
    T.in_audio = heap_caps_calloc(1, sizeof(q_item_t), MALLOC_CAP_SPIRAM);
    T.loop_item = heap_caps_calloc(1, sizeof(q_item_t), MALLOC_CAP_SPIRAM);
    T.in_state = heap_caps_calloc(1, sizeof(q_item_t), MALLOC_CAP_SPIRAM);
    T.state_lock = xSemaphoreCreateMutex();
    err = T.input && T.ctl && T.in_msg && T.in_audio && T.loop_item && T.in_state && T.state_lock ? ESP_OK
                                                                                                  : ESP_ERR_NO_MEM;
    if (err == ESP_OK && xTaskCreatePinnedToCore(run_task, "yk_talk", STACK, NULL, 7, &T.task, 0) != pdPASS)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        printf("YK_TALK start fw=%s mode=%s server=%s\n", T.fw, T.mode[0] ? T.mode : "chat",
               T.production ? "production" : T.url);
        mem_line("start");
        const yk_session_config_t s = {.url = T.url, .firmware = T.fw, .token = token, .hello = make_hello,
                                       .on_state = on_state, .on_message = on_message, .on_audio = on_audio};
        err = yk_session_start(&s);
    }
    if (err == ESP_OK) {
        yk_speak_set_live(true);  // effects keep out of the microphones and the replies
        return ESP_OK;
    }
    // Not started: nothing left half-made, yk_talk_start() may be called again.
    ESP_LOGE(TAG, "not started: %s", esp_err_to_name(err));
    if (T.task) vTaskDelete(T.task);
    T.task = NULL;
    if (T.input) vQueueDelete(T.input);
    if (T.ctl) vQueueDeleteWithCaps(T.ctl);
    T.input = T.ctl = NULL;
    free(T.in_msg);
    free(T.in_audio);
    free(T.loop_item);
    free(T.in_state);
    if (T.state_lock) vSemaphoreDelete(T.state_lock);
    T.in_msg = T.in_audio = T.loop_item = T.in_state = NULL;
    T.state_lock = NULL;
    return err;
}

static int talk_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    printf("YK_TALK state=%s online=%d turn=%s speaking=%d held=%d session=%s\n", yk_talk_state_name(T.sm.state),
           T.sm.online, T.sm.turn, T.sm.speaking, T.held, yk_session_state_name(yk_session_state()));
    mem_line("talk");
    return 0;
}

esp_err_t yk_talk_register_console(void) {
    const esp_console_cmd_t cmd = {.command = "talk", .help = "yaykit_talk: state and memory", .func = talk_cmd};
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err == ESP_OK) err = yk_session_register_console();
    return err;
}
