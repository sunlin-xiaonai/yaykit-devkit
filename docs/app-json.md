# app.json：应用清单

每个应用的根目录有一份 `app.json`，告诉商城、Yay 和平台：它叫什么、怎么打开、能用哪些云能力。版本号跟着固件一起改。我们审核时用 Yaykit 的正式契约逐字段核对；不认识的字段一律不收。

## 最小的一份（模板）

```json
{
  "slug": "my-timer",
  "kind": "extension",
  "version": "0.1.0",
  "name": {"zh": "计时器", "en": "Timer"},
  "boards": ["tikpal-r1"],
  "layout": "yaykit-v1",
  "minYay": "0.1.10",
  "apiLevel": 3,
  "capabilities": {},
  "launch": {
    "summary": {"zh": "计时器：说多久就计多久，到点提醒。", "en": "Timer: counts down as long as you say."},
    "examples": {"zh": ["打开计时器", "帮我计时十分钟"], "en": ["Open the timer", "Set a ten minute timer"]}
  },
  "aliases": {"zh": ["定时器"], "en": ["Countdown"]},
  "scenes": ["tools"],
  "audience": "all",
  "pricing": {"kind": "free"}
}
```

## 字段

| 字段 | 必填 | 说明 |
|---|---|---|
| `slug` | 是 | 小写字母、数字、连字符，字母开头，2–41 个字符。和项目名 `yaykit_<slug>` 对得上。定了就不改。`yay`、`launcher`、`recovery` 是保留的 |
| `kind` | 是 | 扩展应用都写 `extension` |
| `version` | 是 | `主.次.修`，和 `CMakeLists.txt` 的 `PROJECT_VER` 一样。每次提交都比上一次大 |
| `name` | 是 | 名字，`zh` 和 `en` 必填，`ja`、`es-419` 可选；每种不超过 24 个字符 |
| `boards` | 是 | 写 `["tikpal-r1"]` |
| `layout` | 是 | 写 `"yaykit-v1"` |
| `minYay` | 建议 | 最低要哪一版 Yay。至少写开发包对应的那一版（`./kit version` 的 `yay`） |
| `apiLevel` | 建议 | 开发包的接口级别（`./kit version` 的 `apiLevel`） |
| `capabilities` | 是 | 要用的云能力和限额，最小权限：设备令牌只许这些。不要云能力就写 `{}`。要什么，立项时告诉我们，我们帮你写这一段 |
| `launch` | 要让 Yay 用说话打开就要 | `summary`：一句话说它能做什么（≤ 300 字）。`examples`：用户会说的话，每种语言最多 12 句、每句 ≤ 80 字，两类都要有：点了名的（「打开计时器」）和没点名、只说想做什么的（「帮我计时十分钟」），Yay 找应用主要靠后一类。`params`：Yay 能带给你的启动参数（见下） |
| `aliases` | 是 | 别的叫法，每种语言最多 8 个；没有就写空数组 `{"zh": [], "en": []}` |
| `scenes` | 是 | 在商城哪一类里，1–3 个：`party` 聚会、`learning` 学习、`relax` 放松、`language` 语言、`focus` 专注、`tools` 工具 |
| `audience` | 是 | `kids`、`adults` 或 `all` |
| `pricing` | 是 | 现在只有 `{"kind": "free"}` |
| `settings` 等 | 否 | 可以在手机 App 里调的设置（`settings`、`settingOptions`、`settingLabels`、`settingRules`）。要的话立项时说，我们一起定 |
| `records` | 否 | 记录进「动态」的格式。用到 `yk_app_record()` 的应用要写，立项时一起定 |
| `ephemeral` | 否 | `true`：什么都不留（不写记录、不存数据和文件），比如翻译 |

### 启动参数 `launch.params`

Yay 打开你时能带的参数，键名是参数名，值说明它：

```json
"params": {
  "minutes": {"type": "number", "description": "要计时多少分钟"},
  "sound": {"type": "string", "description": "到点用什么声音", "enum": ["bell", "chime"]}
}
```

- `type`：`string`、`number` 或 `boolean`；`description`：给 Yay 的模型看，它按这个决定传什么；`enum`：只能是这几个值。
- 只写固件真读的参数。读法见[应用开发指南](app-guide.md)第 5 节。

## 常见的错

- `version` 忘了跟 `PROJECT_VER` 一起改。
- `examples` 只写了「打开 xxx」：Yay 只有在用户点名时才找得到你。
- `capabilities` 里写了没用到的能力：审核会请你删掉。
- 写了 `launch.params`，固件却不读：用户说了参数也没用。
