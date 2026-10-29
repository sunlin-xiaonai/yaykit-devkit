# 第三方组件和许可

开发包本身按 [LICENSE](LICENSE) 保留所有权利；下面这些是别人的东西，各按各自的许可，[LICENSE](LICENSE) 的限制不适用于它们。编出来的固件里也有它们，发布固件时一并遵守。

## 开发包里带的

| 名字 | 在哪 | 许可 | 许可文件 |
|---|---|---|---|
| Noto Sans SC（简体中文子集） | `firmware/components/yaykit_board/fonts/cjk/yaykit_fonts.bin`（字体包的简中那份） | SIL Open Font License 1.1 | `firmware/components/yaykit_board/fonts/cjk/OFL.txt` |
| Noto Sans JP（日文子集） | 同一个字体包的日文那份 | SIL Open Font License 1.1 | `firmware/components/yaykit_board/fonts/cjk/OFL-NotoSansJP.txt` |

字体包怎么生成见 `firmware/components/yaykit_board/README.md`「字体包」。两个许可文件要跟字体包放在一起，不能删。

## 编译时从乐鑫组件库拉进来的

不在开发包里：`./kit build` 时按应用的 `dependencies.lock` 从 https://components.espressif.com 下载到 `managed_components/`。下面的版本是开发包模板锁定的。

| 组件 | 版本 | 许可 |
|---|---|---|
| lvgl/lvgl | 9.6.0 | MIT |
| espressif/esp_lvgl_adapter | 0.6.4 | Apache-2.0 |
| waveshare/esp_lcd_sh8601 | 2.0.0 | Apache-2.0 |
| espressif/esp_codec_dev | 1.5.11 | Apache-2.0 |
| espressif/button | 4.2.1 | Apache-2.0 |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 |
| espressif/esp_lcd_touch | 1.2.1 | Apache-2.0 |
| espressif/esp_lv_decoder | 0.4.3 | Apache-2.0 |
| espressif/esp_lv_fs | 1.0.1 | Apache-2.0 |
| espressif/esp_mmap_assets | 2.0.1 | Apache-2.0 |
| espressif/knob | 1.1.0 | Apache-2.0 |
| espressif/esp_new_jpeg | 1.0.2 | Espressif MIT License |
| espressif/freetype | 2.14.3~1 | FreeType License（FTL），或 GPL-2.0，任选其一；这里按 FTL 用 |
| espressif/libpng | 1.6.58~1 | libpng License |
| espressif/zlib | 1.3.2~1 | zlib License |

应用用到下面这些公共组件时，还会多拉：

| 组件 | 版本 | 许可 | 谁要它 |
|---|---|---|---|
| espressif/esp-sr | 2.5.5 | Espressif MIT License（含降噪、人声检测模型） | `yaykit_audio` |
| espressif/esp_audio_codec | 2.6.2 | Espressif Modified MIT License | `yaykit_audio`、`yaykit_speak` |
| espressif/esp_websocket_client | 1.8.0 | Apache-2.0 | `yaykit_session`（`yaykit_talk` 经它） |

每个组件的许可原文在 `managed_components/<组件>/` 里（`LICENSE`、`license.txt` 或 `LICENCE.txt`）。

## ESP-IDF 5.5.5

不在开发包里：`./kit setup` 装的。ESP-IDF 本身是 Apache-2.0；编进固件的还有它带的第三方部分，比如 FreeRTOS（MIT）、lwIP（BSD）、Mbed TLS（Apache-2.0）、cJSON（MIT）、NimBLE（Apache-2.0）、wpa_supplicant（BSD）、protobuf-c（BSD）、newlib（BSD）、linenoise（BSD-2-Clause）、argtable3（BSD-3-Clause）、TLSF（BSD-3-Clause）。完整的清单和许可以 ESP-IDF 的 `COPYRIGHT.rst`（`$IDF_PATH/docs/zh_CN/COPYRIGHT.rst`）为准。

---

# Third-party components and licences

The kit itself is all rights reserved ([LICENSE](LICENSE)). What is listed above belongs to others and is under its own licence; the restrictions of [LICENSE](LICENSE) do not apply to it. Firmware built with the kit contains it too, so honour those licences when firmware is released.

- **In the kit**: Noto Sans SC and Noto Sans JP subsets in `yaykit_fonts.bin`, SIL Open Font License 1.1 (`OFL.txt`, `OFL-NotoSansJP.txt` beside the font pack; keep them there).
- **Fetched at build time** from the Espressif component registry, at the versions in the tables above (the template's `dependencies.lock`); each one's licence text is in `managed_components/<component>/`.
- **ESP-IDF 5.5.5** (Apache-2.0) and the third-party parts it compiles in; its `COPYRIGHT.rst` is the full list.
