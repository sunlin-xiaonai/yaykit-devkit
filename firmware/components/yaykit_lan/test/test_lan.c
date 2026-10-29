// Host tests for yaykit_lan's pure parts (yk_lan_logic.c).
#include <stdio.h>
#include <string.h>

#include "yk_lan_logic.h"

static int failures, checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

int main(void) {
    const uint8_t r[16] = {0x00, 0x01, 0x7f, 0x80, 0xff, 0x10, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde};
    char tok[33];
    yk_lan_token_text(r, tok);
    CHECK(!strcmp(tok, "00017f80ff10abcdef123456789abcde"));
    CHECK(strlen(tok) == 32);

    CHECK(yk_lan_token_equal(tok, "00017f80ff10abcdef123456789abcde"));
    CHECK(!yk_lan_token_equal(tok, "00017f80ff10abcdef123456789abcdf"));
    CHECK(!yk_lan_token_equal(tok, "00017f80ff10abcdef123456789abcd"));
    CHECK(!yk_lan_token_equal(tok, ""));
    CHECK(!yk_lan_token_equal("", ""));  // no token: nobody gets in
    CHECK(!yk_lan_token_equal(NULL, tok));

    char name[16];
    char msg[200];
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"token\":\"%s\",\"name\":\"iPhone\"}", tok);
    CHECK(yk_lan_hello(msg, tok, name, sizeof(name)) == YK_LAN_HELLO_OK && !strcmp(name, "iPhone"));
    // Unknown fields are fine; no name is fine.
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"token\":\"%s\",\"v\":2}", tok);
    CHECK(yk_lan_hello(msg, tok, name, sizeof(name)) == YK_LAN_HELLO_OK && name[0] == 0);
    CHECK(yk_lan_hello("{\"t\":\"hello\",\"token\":\"nope\",\"name\":\"x\"}", tok, name, sizeof(name)) == YK_LAN_HELLO_BAD &&
          name[0] == 0);
    CHECK(yk_lan_hello("{\"t\":\"hello\"}", tok, name, sizeof(name)) == YK_LAN_HELLO_BAD);
    CHECK(yk_lan_hello("{\"t\":\"settings\"}", tok, name, sizeof(name)) == YK_LAN_NOT_HELLO);
    CHECK(yk_lan_hello("[1,2]", tok, name, sizeof(name)) == YK_LAN_NOT_HELLO);
    CHECK(yk_lan_hello("hello", tok, name, sizeof(name)) == YK_LAN_NOT_JSON);
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"token\":\"%s\"}", tok);
    CHECK(yk_lan_hello(msg, "", name, sizeof(name)) == YK_LAN_HELLO_BAD);  // forgotten token

    // A long name is cut without leaving half a character; control characters become spaces.
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"token\":\"%s\",\"name\":\"小明的手机手机\\n\"}", tok);
    CHECK(yk_lan_hello(msg, tok, name, sizeof(name)) == YK_LAN_HELLO_OK);
    CHECK(!strcmp(name, "小明的手机"));  // 15 bytes fit; the 6th character would need 3 more
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"token\":\"%s\",\"name\":\"a\\tb\"}", tok);
    CHECK(yk_lan_hello(msg, tok, name, sizeof(name)) == YK_LAN_HELLO_OK && !strcmp(name, "a b"));

    printf("yaykit_lan host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
