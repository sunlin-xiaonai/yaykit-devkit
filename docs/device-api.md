# 设备公共 API

圆屏上的应用用云能力（AI 对话、语音识别、语音合成、存数据和文件）都走设备公共 API，固件里用 `yaykit_cloud` 组件调（头文件 `devkit/firmware/components/yaykit_cloud/include/`）。地址固定是线上的，不用配。

## 权限：只给清单里写的

每个应用用自己的令牌（`yk_account_token()`，15 分钟，快过期组件自己换），令牌只许给这个应用开通的那些能力，别的一律被拒（`OPERATION_NOT_ALLOWED`）。要哪些能力立项时告诉我们：开通要我们部署一次，可能得等；能力或限额改了，要再开通一次。

## 先问 `/me`

```c
yk_cloud_me_t me;
const char *code = NULL;
if (yk_cloud_me(&me, &code) == ESP_OK && yk_cloud_has("asr")) {   // 开始时问一次；有这个能力才用
    int max_s = yk_cloud_limit("asr", "maxSeconds", 15);            // 读不到才用后面这个默认值
}
```

`/me` 回来这些：

- `capabilities`：你这个应用被授予的能力和限额（时长、字数、语言、格式、模板）。**按它调整，不写死。**
- `credits`：圆屏绑定的那个账号还剩多少点数（开发时扣的就是你绑的账号）。用完了接口返回 `CREDITS_EXHAUSTED`，界面要说人话（「今天的点数用完了」）。
- `owner`：机主的昵称、语言、时区（不给邮箱这些资料）。界面语言跟着 `locale`。
- `settings`：用户在手机 App 里给你这个应用调的设置（`app.json` 里有 `settings` 的才有）。

## 能力一览

| 能力 | 组件里怎么调 | 说明 |
|---|---|---|
| `asr` 语音识别 | `yk_cloud_asr()` | 一段 WAV（16 kHz 单声道，`yk_audio_rec_*` 录）或 Ogg Opus，回文字 |
| `tts` 语音合成 | `yk_cloud_tts()` | 回 Opus 包，直接交给 `yk_speak_opus()` 放 |
| `llm.fast` / `llm.smart` 对话 | `yk_cloud_chat()` | 用**模板名加变量**（提示词和模型在服务端，换了不用重刷固件）；简单试验也可以直接给消息 |
| 场景接口 | `yk_cloud_scene()` | 一个应用专用的一条（比如「拍照查字」），立项时一起定 |
| `data` 小数据 | `yk_cloud_data_get/put/del/list()` | 按键存小块 JSON（进度、偏好），跟着账号走 |
| `files`、`transcribe` 文件 | `yk_cloud_file_*()` | 登记、分片直传、保存期限、转写（录音类应用） |
| `records` 记录 | `yk_app_record()` | 不用自己联网：先进圆屏上的发件箱，谁在线谁送 |
| 语音对话 | `yaykit_talk` | 按住说话 → 回答 → 播放 → 打断，整套交给组件（见应用开发指南） |

## 规矩

- **调之前先 `yk_cloud_has()`**；限额从 `/me.capabilities` 读。
- **提示词、模型、音色不写进固件**：用模板名或场景接口。要新模板，提需求单。
- **只做加法**：接口和事件以后会加字段，解析时忽略不认识的字段和事件；不兼容的改动会走新版本路径，旧的至少保留 6 个月。
- **错误码原样给出**（`CREDITS_EXHAUSTED`、`NETWORK`、`OPERATION_NOT_ALLOWED`……），界面上换成人话，不显示错误码。
- **断网**：组件返回 `NETWORK`；`yk_notice_net()` 先显示「正在连」，久了提示「回到 Yay 设置网络」。
- **一台圆屏同时只有一个语音请求**；对话、安装、配对不同时进行（内部 RAM 不够）。
