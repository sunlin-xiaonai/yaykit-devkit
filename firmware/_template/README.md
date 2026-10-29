# 扩展固件模板

新做一个扩展固件就从这里开始（[公共固件开发指南](../../docs/development/firmware-guide.md)第 6 节，开发文档第 6.10 节）。它是一个能直接装进应用位置的小应用：短按侧键加一，按住清零，**双击侧键回到 Yay**。

## 照着改

1. 复制这个目录到 `firmware/<slug>`。
2. `CMakeLists.txt`：`project(yaykit_template)` 改成 `project(yaykit_<slug，连字符换下划线>)`，位置和商店都靠这个名字认应用。
3. `main/main.c`：`APP_SLUG` 改成你的 slug；启动横幅自动是 `YAYKIT_<SLUG>_READY`。
4. `app.json`（应用清单，契约是仓库根目录的 `packages/contracts/src/app-spec.ts`）：改 `slug`、`version`（和 `PROJECT_VER` 一样）、名字、能力、打开方式、场景；版本号跟固件一起改。只写这一版固件真做得到的：固件不读的启动参数不写进 `launch.params`。`launch.examples` 每种语言都要有两类：点了名的（「打开计数器」），和没点名、只说想做什么的（「帮我数数」），Yay 找应用主要靠后一类。
5. 把 `counter.c` 换成你的逻辑。逻辑写成不依赖 ESP-IDF 的纯 C，`test/run.sh` 在电脑上测。
6. 构建：`bash scripts/build-template.sh` 照着改一份。只编不刷。

## 已经替你做好的

- 启动顺序：NVS（出错不擦）→ 板子 → 侧键 → `yaykit_app` → Yaykit Link → 界面 → `yk_app_ready()` → 控制台。
- `yaykit_key`：短按、按住给你；双击归系统，回到 Yay。要自己用双击，就在 `yk_app_config_t` 里设 `own_double`，并另外给一个回到 Yay 的办法。
- `yaykit_app`：启动横幅、确认镜像、读 Yay 带来的启动参数（`yk_app_launch`）、退出前留一句结果（`yk_app_set_result`）、`yk cmd app.exit` / `app.recovery` / `app.status`、控制台 `app`，开发版还有 `yay tap|double|hold|say|launch`。
- `yaykit-v1` 分区、带回滚；开机不做 PSRAM 自检（每次切换都快 0.4 秒）；蓝牙默认关。
- 颜色用圆屏的深色那组；`yaykit_ui` 的主题做好后改用主题。

发布前过一遍指南第 6 节的兼容检查清单。
