// Closing before a restart (yk_talk_close.h).
#include "yk_talk_close.h"

yk_close_result_t yk_close_run(const yk_close_env_t *env, uint32_t timeout_ms) {
    if (!env->up(env->ctx)) return YK_CLOSE_GONE;  // nothing to close (offline, never up, closed)
    const int64_t t0 = env->now_ms(env->ctx);
    const yk_flush_result_t f = env->flush(timeout_ms, env->ctx);
    if (f == YK_FLUSH_FAILED) return YK_CLOSE_GONE;  // a message found no session: it went meanwhile
    if (f == YK_FLUSH_TIMEOUT) return YK_CLOSE_TIMEOUT;
    const int64_t spent = env->now_ms(env->ctx) - t0;
    if (spent >= (int64_t)timeout_ms) return YK_CLOSE_TIMEOUT;
    return env->close(timeout_ms - (uint32_t)spent, env->ctx);
}

const char *yk_close_result_name(yk_close_result_t r) {
    static const char *const names[] = {"replied", "timeout", "gone"};
    return r <= YK_CLOSE_GONE ? names[r] : "?";
}
