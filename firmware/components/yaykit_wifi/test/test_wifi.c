// Host tests for the pure C parts of yaykit_wifi. Run: bash firmware/components/yaykit_wifi/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_wifi_creds.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

#define OK(ssid, password) CHECK(yk_wifi_join_error(ssid, password) == NULL)
#define BAD(ssid, password, code) CHECK(yk_wifi_join_error(ssid, password) && !strcmp(yk_wifi_join_error(ssid, password), code))

static void test_join_arguments(void) {
    OK("Home", "12345678");
    OK("Home", NULL);  // open network
    OK("Home", "");
    OK("\xe5\xb0\x8f\xe9\xb1\xbc\xe5\xae\xb6", "pass word with spaces");  // 小鱼家
    OK("12345678901234567890123456789012", "123456789012345678901234567890123456789012345678901234567890123");
    OK("Home", "0123456789abcdefABCDEF0123456789abcdef0123456789abcdef0123456789");  // 64 hex: a raw key
    BAD(NULL, "12345678", "BAD_SSID");
    BAD("", "12345678", "BAD_SSID");
    BAD("123456789012345678901234567890123", "12345678", "BAD_SSID");
    BAD("Home", "1234567", "BAD_PASSWORD");
    BAD("Home", "0123456789abcdefABCDEF0123456789abcdef0123456789abcdef012345678g", "BAD_PASSWORD");
    BAD("Home", "line\nbreak", "BAD_PASSWORD");
    BAD("Home", "1234567890123456789012345678901234567890123456789012345678901234567", "BAD_PASSWORD");
}

int main(void) {
    test_join_arguments();
    printf("yaykit_wifi host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
