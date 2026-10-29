// Host tests for the pure C parts of yaykit_link. Run: bash firmware/components/yaykit_link/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_line.h"
#include "yk_mem.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

/** Feeds `text` and returns the result after the last byte; `*used` is how many bytes were taken. */
static yk_line_result_t feed(yk_line_t *line, const char *text, size_t *used) {
    yk_line_result_t got = YK_LINE_MORE;
    size_t i = 0;
    while (text[i] && got == YK_LINE_MORE) got = yk_line_feed(line, text[i++]);
    if (used) *used = i;
    return got;
}

static void test_hidden_line(void) {
    char buf[64];
    yk_line_t line;
    size_t used;
    // The command line's own line break arrives first (CR becomes LF on the device); it is skipped.
    yk_line_init(&line, buf, sizeof(buf));
    CHECK(feed(&line, "\n\n{\"ssid\":\"Home\",\"password\":\"p@ss word!\"}\n\n", &used) == YK_LINE_DONE);
    CHECK(!strcmp(buf, "{\"ssid\":\"Home\",\"password\":\"p@ss word!\"}"));
    CHECK(used == 2 + strlen(buf) + 1);  // the trailing line break is left for the console prompt
    // CR ends a line as well; control characters are dropped, UTF-8 is kept.
    yk_line_init(&line, buf, sizeof(buf));
    CHECK(feed(&line, "\r{\"ssid\":\"\xe5\xae\xb6\x01\x7f\"}\r", NULL) == YK_LINE_DONE);
    CHECK(!strcmp(buf, "{\"ssid\":\"\xe5\xae\xb6\"}"));
    // Nothing yet: keep waiting.
    yk_line_init(&line, buf, sizeof(buf));
    CHECK(feed(&line, "\r\n{\"ssid\"", NULL) == YK_LINE_MORE);
    // Exactly cap - 1 characters fit.
    char exact[sizeof(buf) + 1];
    memset(exact, 'a', sizeof(buf) - 1);
    exact[sizeof(buf) - 1] = '\n';
    exact[sizeof(buf)] = 0;
    yk_line_init(&line, buf, sizeof(buf));
    CHECK(feed(&line, exact, NULL) == YK_LINE_DONE && strlen(buf) == sizeof(buf) - 1);
    // Too long: the rest of the line is still consumed (none of it may reach the prompt) and the buffer cleared.
    char lng[200];
    memset(lng, 's', sizeof(lng) - 2);
    lng[sizeof(lng) - 2] = '\n';
    lng[sizeof(lng) - 1] = 0;
    yk_line_init(&line, buf, sizeof(buf));
    CHECK(feed(&line, lng, &used) == YK_LINE_TOO_LONG);
    CHECK(used == sizeof(lng) - 1);
    char zero[sizeof(buf)] = {0};
    CHECK(!memcmp(buf, zero, sizeof(buf)));
    // Wiping.
    char secret[] = "hunter22";
    yk_line_wipe(secret, strlen(secret));
    CHECK(!memcmp(secret, "\0\0\0\0\0\0\0\0", 8));
}

/** The figure from before 10-24 after the count started afresh: the lower of the two spans (yk_mem.h). */
static void test_mem_ever(void) {
    CHECK(yk_mem_ever(false, 0, 39828) == 39828);      // not ready yet: from boot, as it was
    CHECK(yk_mem_ever(true, 39828, 59419) == 39828);   // the boot span was lower (the 32 KB region filled once)
    CHECK(yk_mem_ever(true, 120000, 59419) == 59419);  // lower since ready
    CHECK(yk_mem_ever(true, 60000, 60000) == 60000);
}

int main(void) {
    test_hidden_line();
    test_mem_ever();
    printf("yaykit_link host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
