#!/usr/bin/env bash
# Host tests for the pure parts of yaykit_ui (no ESP-IDF, LVGL stubbed): the rim, subtitles, notices, Yay; the app
# ring, the app catalog, the hint arrow and 「我的应用」.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -I.. -I../include -Istub -o "$OUT/test_ui" test_ui.c ../yk_ui_logic.c -lm
"$OUT/test_ui"
cc -std=c11 -Wall -Wextra -Werror -O1 -I.. -I../include -Istub -o "$OUT/test_apps" test_apps.c ../yk_apps_logic.c \
  ../yk_ui_logic.c -lm
"$OUT/test_apps"
cc -std=c11 -Wall -Wextra -Werror -O1 -I.. -I../include -Istub -o "$OUT/test_more" test_more.c ../yk_more_logic.c \
  ../yk_apps_logic.c ../yk_ui_logic.c -lm
"$OUT/test_more"
rm -rf "$OUT"
