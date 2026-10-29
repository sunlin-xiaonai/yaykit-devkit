# yaykit_board

tikpal-r1 圆屏（ESP32-S3R8、32 MB Flash、8 MB PSRAM、466×466 AMOLED）的板级驱动，固件里唯一知道硬件的地方。

| 文件 | 内容 |
|---|---|
| `include/board.h`、`board.c` | 外设电源（GPIO21）、共用 I2C、SH8601 屏幕（QSPI）、CST820 触摸、LVGL 任务和锁、亮度；引脚表在 `board.h` 开头 |
| `board_rtc.c`、`rtc_regs.c` | PCF85063A 时钟（`board_rtc_init`、`board_rtc_save`、`board_rtc_restored`）：断网重启后从时钟芯片恢复系统时间，SNTP 校准后写回 |
| `lvgl_mem_psram.c` | LVGL 的内存放 PSRAM（`CONFIG_LV_USE_CUSTOM_MALLOC=y`） |
| `include/audio.h`、`audio.c` | 一个 I2S 口：ES8311 放音、ES7210 四路麦克风（TDM）；采样率开机时选 24 kHz（默认）或 16 kHz |
| `include/cjk_font.h`、`cjk_font.c`、`yk_font_pack.c` | 从 `fonts` 分区映射字体包，按语言选主字体，交给 LVGL TinyTTF |
| `fonts/cjk/yaykit_fonts.bin`、`OFL.txt`、`OFL-NotoSansJP.txt` | 共享字体分区的内容：简中、日文两份子集加索引，3 MB 整个分区的镜像（SIL OFL 1.1，见下文「字体包」） |
| `fonts/gen_font_pack.py`、`fonts/cjk/tikpal_sc_chars.txt` | 生成字体包；原来那份字体的全部字符（生成时照样放进简中） |
| `board_pref.c` | 全设备共用的音量、亮度、息屏时间（共享 NVS `yk_pref`，公共层计划 P10） |
| `board_power.c` | 电量（ESP32 ADC 读 GPIO17 的 V_BAT/2、GPIO18 的 USB 5 V/2，P20）和 SW6106 只读对照 |
| `board_touch_fast.c` | 指定时段每 5 ms 读触摸、带时间戳（P20；接了中断脚就用中断） |
| `yk_board_logic.c`、`rtc_regs.c` | 不依赖 ESP-IDF 的部分（含电量换算、设置的范围），`test/run.sh` 在主机上测 |

依赖全是公开组件，版本和 Tikpal 0.4.9 在设备上跑的完全一致（对照它在 `5a299f5c` 的 `dependencies.lock`，每个组件的版本和 `component_hash` 都相同）：`lvgl/lvgl` 9.6.0、`espressif/esp_lvgl_adapter` 0.6.4、`waveshare/esp_lcd_sh8601` 2.0.0、`espressif/esp_codec_dev` 1.5.11，间接依赖 `espressif/button` 4.2.1、`espressif/libpng` 1.6.58~1 也钉住了（`require: no`，只管版本）。版本写死在 `idf_component.yml`，各固件的 `dependencies.lock` 提交进仓库；要升级就连同真机验收一起改。

## 用法

```cmake
# 固件的 CMakeLists.txt
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/yaykit_board" ...)
# main/CMakeLists.txt
idf_component_register(... REQUIRES yaykit_board ...)
esptool_py_flash_to_partition(flash "fonts" "${CMAKE_CURRENT_LIST_DIR}/../../components/yaykit_board/fonts/cjk/yaykit_fonts.bin")
```

用组件管理器构建（不要设 `IDF_COMPONENT_MANAGER=0`）。`sdkconfig.defaults` 里要有 `CONFIG_LV_USE_CUSTOM_MALLOC=y` 和 `CONFIG_LV_USE_TINY_TTF=y`。

音频：`audio_init()` 就是 `audio_init_rate(24000)`；要 16 kHz 就调用 `audio_init_rate(16000)`。放音和录音共用时钟，开起来以后不能换采样率：再按另一个采样率启动会得到 `ESP_ERR_INVALID_STATE`（`audio_init()` 也一样），打开 codec 设备时用 `audio_sample_rate()`。I2S 默认每个 DMA 帧 20 ms，8 帧共 160 ms 余量；`audio_init_cfg()` 可以设帧长和帧数（低延迟 4 × 5 ms，P13），收发共用一个设置。

共享设置（P10）：`board_pref_*()` 读写 NVS `yk_pref` 的 `volume`、`bright`、`screen_off`；第一次读音量时，如果 `yk_pref` 里还没有，就从主固件原来的 `yay/volume` 搬一次。`board_display_start_opts()` 的 `start_dark` 让屏幕起来时保持黑（初始化序列里亮度写 0），应用画完第一帧再调亮度；`board_brightness_raw_set(0..255)` 直接写面板刻度。息屏默认 30 秒（P22 起；以前是 180 秒）。

息屏（P22）：`board_display_sleep()` 把亮度降到 0、SH8601 关显示（`0x28`），并关掉 LVGL 的重画（`lv_display_enable_invalidation`，是计数，和 `lv_qrcode` 自己的开关不冲突）；触摸芯片不再读，息屏期间 `board_touch_read()` 返回 `ESP_ERR_INVALID_STATE`。`board_display_wake(percent)` 开显示、设亮度（`-1` 用共享亮度）、整屏重画，触摸马上照常读。CST820 不进深度休眠：深度休眠只能靠复位脚出来，真机上亮屏时的这次复位和开麦撞在同一条 I2C 上，5 次里 3 次 ES7210 读写出错，1 次麦克风没打开（2026-10-21）。不读它时，芯片没被触摸就自己进低功耗扫描。

引脚（参考原理图 `yaykit/docs/hardware/ESP32-S3-1.75-AMOLED-AI.pdf`）：V_BAT 经 100k/100k 分压到 GPIO17，USB 5 V 经 200k/200k 到 GPIO18；触摸 TP_INT 在 GPIO11 或 GPIO13：2026-10-21 实测 GPIO13 以 59 Hz 跳（屏幕 TE），GPIO11 平时高，所以 TP_INT 应是 GPIO11，按屏幕确认后写进 `CONFIG_YK_BOARD_TOUCH_INT_GPIO`；插着 USB 时 GPIO17 读到约 4.40 V（充电输出，和 SW6106 一致）；QMI8658 的 INT1/INT2 在 GPIO47/48（`yaykit_motion` 现在不用中断）。

功放（GPIO43，`BOARD_PIN_PA_EN`）不由 esp_codec_dev 开关，归 `yaykit_speak`。`yaykit_speak` 出来之前，`yaykit_chime` 和呼吸自己开关功放，这是过渡期的例外；新代码一律通过 `yaykit_speak`。

## 来源

搬自用户自己的私有仓库 `vzheng56/tikpal-firmware`，提交 `5a299f5c081c2b40ba770f6fce5d7291197d2882`（Tikpal 0.4.9，快照标签 `snapshot/2026-10-pre-2.2`）。原样复制的版本在 Git 历史里（两个 `verbatim copy …` 提交：先是屏幕、音频和字体，后是时钟），之后的改动都能逐行对照。

| 这里 | Tikpal 原路径 | 原 blob |
|---|---|---|
| `board.c` | `firmware/components/board/board.c` | `797da3ba` |
| `lvgl_mem_psram.c` | `firmware/components/board/lvgl_mem_psram.c` | `23c79faf` |
| `include/board.h` | `firmware/components/board/include/board.h` | `3397bcea` |
| `audio.c` | `firmware/components/audio_io/audio.c` | `33cd3423` |
| `include/audio.h` | `firmware/components/audio_io/include/audio.h` | `2df9463e` |
| `cjk_font.c` | `firmware/components/ui/cjk_font.c` | `945654f8` |
| `include/cjk_font.h` | `firmware/components/ui/include/cjk_font.h` | `f99dd181` |
| `fonts/cjk/tikpal_sc.ttf` | `firmware/components/ui/fonts/cjk/tikpal_sc.ttf` | `87cd99b8` |
| `fonts/cjk/OFL.txt` | `firmware/components/ui/fonts/cjk/OFL.txt` | `fd9e0022` |
| `board_rtc.c` | `firmware/components/clock/clock.c` | `84ba586b` |
| `rtc_regs.c` | `firmware/components/clock/rtc_regs.c` | `93d21ad4` |
| `rtc_regs.h` | `firmware/components/clock/include/rtc_regs.h` | `548d3799` |
| `include/board.h` 的时钟部分 | `firmware/components/clock/include/clock.h` | `d74a27d7` |

和原来不同的地方：

- `board`、`audio_io`、`clock` 三个组件和 `ui` 里的字体合成一个组件 `yaykit_board`，依赖写在一个 `idf_component.yml` 里，从版本范围改成写死 Tikpal 0.4.9 锁定的版本（Tikpal 用的 `esp_audio_codec` 这里不需要，没有带）。
- 时钟的接口改名：`clock_init`、`clock_save`、`clock_restored` 成了 `board_rtc_init`、`board_rtc_save`、`board_rtc_restored`，声明并进 `board.h`；`rtc_regs.h` 改为组件内部头文件。算法和寄存器操作没有动，主机测试是新写的（Tikpal 的 `test_rtc.c` 没有搬）。
- 不再整个库强制链接（`WHOLE_ARCHIVE`），只用 `-u lv_malloc_core` 留住 LVGL 内存函数，不用音频的固件不会带上音频代码。
- 音频采样率可选 16 kHz（`audio_init_rate`、`audio_sample_rate`），默认仍是 24 kHz，现有固件的行为不变；已在运行时换采样率会被拒绝。DMA 帧长按采样率取 20 ms（24 kHz 时仍是 480 帧）。
- `board.h` 加了功放引脚 `BOARD_PIN_PA_EN`（43）。
- 屏幕刷新区域取整、触摸坐标解析挪到 `yk_board_logic.c`，有主机测试；逻辑不变。

原来的字体（`tikpal_sc.ttf`）由 Tikpal 的 `tools/gen_cjk_font.sh` 从 Noto Sans SC 截取（GB2312 + 拉丁字母 + 标点，7,686 个码位），和 Tikpal 0.4.9 一字不差；它的字符全部保留在下面字体包的简中那份里（`fonts/cjk/tikpal_sc_chars.txt`）。各固件写进 `fonts` 分区的是同一个文件，内容保持一致。

**字体包**（多语言）：`fonts` 分区也可以放几份字体，每种文字一份，末尾 64 字节是索引（`yk_font_pack.h`，主机测试 `test/test_board.c`）。第一份从偏移 0 开始，所以老固件照样读得到。`cjk_font_set_language()` 在第一次 `cjk_font_get()` 之前调用（主固件用账户的 `owner.locale`），对应语言的那份当主字体，其余的挂在 `lv_font_t.fallback` 上补缺字；没有索引时和以前一样，整个分区就是一份字体。包由 `fonts/gen_font_pack.py` 生成，整个分区的镜像就是 `fonts/cjk/yaykit_fonts.bin`，各固件写进 `fonts` 分区的就是它（以前是 `tikpal_sc.ttf`）。

现在的字体包（2026-10-20，SHA-256 `e3c04e9e6154d4af75d78f2326310142d20f570e48f7ae83f72e454040103e33`），3 MB 用了 3,077,188 字节，剩 68 KB：

| 份 | 偏移 | 大小 | 字符 | 内容 |
|---|---|---|---|---|
| `zh` | 0x000000 | 2,349,440 | 7,894 | 《通用规范汉字表》（2013）一、二级 6,500 字；原来那份字体的全部字符（`tikpal_sc_chars.txt`：GB2312 全部，加上三级字 328 个和 亻 扌 这类部首）；固件源码里用到的字；GB2312 符号行；ASCII、Latin-1（西语）、标点、€ ™、假名和 ー ・、全角字符 |
| `ja` | 0x23e000 | 726,020 | 2,688 | 假名、半角片假名、常用汉字 2,136 个（日文字形）、JIS X 0208 符号行、€ ™。拉丁字母和全角字符不放：它们从后面的简中那份拿，Noto Sans SC 和 JP 这部分字形一样 |

覆盖：
- 规范字 8,105 个里有 6,828 个（原来 6,638）：一级 3,500/3,500，二级 3,000/3,000，三级 328/1,605。原来有的一个没少，新增 190 个（啰、乂、玕、玥…）。
- 常用汉字 2,136 个全有；「ー」「€」「・」两份都有；ñ ¿ ¡ ¥ 在简中那份里。

来源：
- Noto Sans SC：Version 2.004（LVGL 9.6.0 组件自带的 `tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf`，SHA-256 `ae82f4e2…852ff`），许可证 `OFL.txt`。
- Noto Sans JP：Version 2.004（Google Fonts 的 `github.com/google/fonts/ofl/notosansjp/NotoSansJP[wght].ttf`，9,589,900 字节，SHA-256 `c2f3b4d4…fc68f`），用 fontTools 的 instancer 固定成 wght=400；许可证 `OFL-NotoSansJP.txt`。
- 字表：Unicode 18.0.0 的 `Unihan.zip`（unicode.org，SHA-256 `4c93ea9c…d43e`），其中 kTGH 是《通用规范汉字表》的序号，kJoyoKanji 是常用汉字。
- 子集保留名称表的许可证字段（13、14 号）。字体原文件和 Unihan 都不进仓库。

重新生成（fontTools 4.67；同样的输入得到一模一样的文件）：

```sh
python3 -I fonts/gen_font_pack.py --sc <NotoSansSC-Regular.ttf> --ja <NotoSansJP-Regular.ttf> \
  --unihan <Unihan.zip> --zh tgh6500 --ja-set joyo --ja-no-latin --scan ../.. \
  --keep fonts/cjk/tikpal_sc_chars.txt --out fonts/cjk/yaykit_fonts.bin
```
