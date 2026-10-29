# yaykit_chime

合成的短提示音，经板上 ES8311 和喇叭放出来。现在只有作业伙伴在用（专注计时结束 `CHIME_FOCUS_DONE`；录音时 `chime_set_muted(true)`，免得喇叭的声音录进去）。

| 接口 | 作用 |
|---|---|
| `chime_init()` | 开音频（`audio_init()`，24 kHz）和功放引脚，起一个优先级 10 的任务，开机时把三种提示音先合成好 |
| `chime_play(kind)` | 排队播放 `CHIME_FOCUS_DONE`（上行琶音）、`CHIME_BREAK_DONE`（下行两音）或 `CHIME_PREVIEW`（单音），立即返回 |
| `chime_set_muted`、`chime_set_volume` | 静音（录音时）、音量 0..100 |
| `chime_play_tone`、`chime_dump_regs`、`chime_soundtest`、`chime_set_pa_pins`… | 测量和诊断用 |

依赖 `yaykit_board` 的 `audio.h`。按 `AUDIO_SAMPLE_RATE`（24 kHz）合成和打开喇叭；音频已经按 16 kHz 启动时，`chime_init()` 返回 `ESP_ERR_INVALID_STATE`，不会放出变调的声音。

功放（GPIO43）现在由这里直接开关，这是 `yaykit_speak` 出来之前的过渡做法；新代码一律通过 `yaykit_speak`。

**以后会被 `yaykit_speak` 的 `yk_speak_chime()` 取代**（见 `docs/design/yay-device.md` 的 `yaykit_speak`）。到时候把作业伙伴迁过去，再删掉这个组件。在那之前，提示音不改。

## 来源

原样搬自用户自己的私有仓库 `vzheng56/tikpal-firmware`，提交 `5a299f5c081c2b40ba770f6fce5d7291197d2882`（Tikpal 0.4.9）：

| 这里 | Tikpal 原路径 | 原 blob |
|---|---|---|
| `chime.c` | `firmware/components/chime/chime.c` | `8522c06d` |
| `include/chime.h` | `firmware/components/chime/include/chime.h` | `3a0c0716` |
| `CMakeLists.txt` | `firmware/components/chime/CMakeLists.txt` | `4e1a621a` |

改动只有三处：`CMakeLists.txt` 里的 `audio_io` 换成 `yaykit_board`（`audio.h` 的接口相同），加了说明注释；`chime_init()` 多查一次 `audio_sample_rate()`，采样率不是 24 kHz 就拒绝启动；功放引脚只配置真正用到的（默认只有 GPIO43），原来每次初始化都把 GPIO46（启动采样脚，板上接到哪里没有资料）设成输出低电平，现在只有 `soundtest` 试它的时候才碰。音符、合成算法、音量映射、功放时序和任务优先级都没有动，所以作业伙伴的提示音和以前一模一样。原样复制的版本在 Git 历史里（`yaykit_board, yaykit_chime: verbatim copy …`）。
