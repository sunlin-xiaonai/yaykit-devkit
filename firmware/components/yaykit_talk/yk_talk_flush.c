// Waiting for the uplink (yk_talk_flush.h).
#include "yk_talk_flush.h"

uint32_t yk_flush_post(yk_flush_t *f) { return ++f->posted; }

void yk_flush_through(yk_flush_t *f, uint32_t n, bool sent) {
    f->done = n;
    if (!sent) f->failed++;
}

yk_flush_result_t yk_flush_wait(const yk_flush_env_t *env, uint32_t timeout_ms) {
    const yk_flush_t at = env->read(env->ctx);  // what is queued now, and the failures so far
    const int64_t until = env->now_ms(env->ctx) + timeout_ms;
    for (;;) {
        const yk_flush_t f = env->read(env->ctx);
        // Numbers wrap after 4 billion commands: the distance, not the order, says whether it is there.
        if ((int32_t)(f.done - at.posted) >= 0) return f.failed != at.failed ? YK_FLUSH_FAILED : YK_FLUSH_SENT;
        if (env->now_ms(env->ctx) >= until) return YK_FLUSH_TIMEOUT;
        env->sleep_ms(YK_FLUSH_POLL_MS, env->ctx);
    }
}

const char *yk_flush_result_name(yk_flush_result_t r) {
    static const char *const names[] = {"sent", "failed", "timeout"};
    return r <= YK_FLUSH_TIMEOUT ? names[r] : "?";
}
