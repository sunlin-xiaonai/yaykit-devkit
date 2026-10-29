// One line read straight from the console, outside linenoise: nothing is echoed or kept in the history. Used for
// `yk cmd <name> -`, whose JSON arguments may carry a secret such as a Wi-Fi password. Pure C, host-tested
// (test/run.sh).
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    YK_LINE_MORE,      // keep feeding
    YK_LINE_DONE,      // buf holds the line, NUL-terminated
    YK_LINE_TOO_LONG,  // the whole line was consumed but did not fit; buf is cleared
} yk_line_result_t;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} yk_line_t;

void yk_line_init(yk_line_t *line, char *buf, size_t cap);
/**
 * Feeds one byte. Line breaks before the first character are skipped (the command line's own CR/LF arrives
 * first); CR or LF ends the line. Other control characters are dropped. A line that does not fit is still
 * read to its end, so none of it reaches the console prompt.
 */
yk_line_result_t yk_line_feed(yk_line_t *line, char c);
/** Overwrites `len` bytes in a way the compiler keeps. */
void yk_line_wipe(void *p, size_t len);
