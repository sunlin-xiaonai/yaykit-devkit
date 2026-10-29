# Yaykit 开发包

给 Yay 圆屏做应用（扩展固件）用的开发包：公共组件的源码、应用模板、写进圆屏和看日志的工具，加一条 `kit` 命令。应用装在圆屏的位置 2、3 上，由 Yay 打开，双击侧键开应用环回到 Yay。

每次 Yay 发版，这里就出一版开发包（`devkit.json` 写着版本、对应的 Yay 和接口级别）。你的应用钉住一个版本，我们这边后来怎么改，都不会突然把它弄坏。

许可：保留所有权利，只许查看，只许用来给 Yaykit 开发应用，见 [LICENSE](LICENSE)。

## 从零到第一次写进圆屏

要一台 Mac、一台测试圆屏（装着 Yay，正式版就行）和一根 USB-C 线。

```bash
git clone https://github.com/vzheng56/yaykit-devkit.git
cd yaykit-devkit
./kit setup              # 装 ESP-IDF 5.5.5（只装一次）；在国内加 --cn
./kit new my-timer       # 在当前目录下建 my-timer/：一个应用一个 git 仓库
cd my-timer
./kit build              # 编译，再跑主机测试
./kit install --slot 2   # USB 写进位置 2，写完直接打开
./kit logs               # 看日志；./kit snap 截图，./kit heap 看内存
```

- 写进去不用签名，也不用任何密钥：USB 写的位置圆屏直接能开（正式版 Yay 也一样）。上商城的那份由我们审过以后签名。
- 只写位置 2、3，不擦别的；位置 1 是 Yay，恢复器在出厂位置，都碰不到。
- 应用仓库里的 `devkit/` 是这个开发包（git 子模块），钉在建应用时的版本。别改它：组件缺什么，告诉我们，做进下一版开发包。

## 文档

| 先读 | 讲什么 |
|---|---|
| [上手](docs/getting-started.md) | 从零到第一次写进圆屏，出问题了怎么办 |
| [从立项到上架](docs/from-idea-to-store.md) | 一个应用从想法到用户能装上的七步，你做哪些、我们做哪些 |
| [应用开发指南](docs/app-guide.md) | 资源归谁、启动顺序、启动参数和结果、侧键、组件速查、内存、版本 |
| [圆屏设计规范](docs/round-screen-design.md) | 颜色、字号、安全圆、边缘环、提示、动效、文案 |
| [app.json](docs/app-json.md) | 应用清单的每个字段 |
| [设备公共 API](docs/device-api.md) | 云能力怎么用、权限、规矩 |
| [兼容检查清单](docs/checklist.md) | 自测和审核都照它 |

用 Claude Code 开发的，`kit new` 已经把 skill（`skills/yaykit-app`）放进应用仓库的 `.claude/skills/`，打开就会用上。

## 里面有什么

| 目录 | 是什么 |
|---|---|
| `kit` | 那条命令（`./kit help`） |
| `firmware/components/` | 公共组件，带源码，只读 |
| `firmware/assets/` | Yay 的图：`yaykit_ui` 编进去用 |
| `firmware/_template/` | 应用模板：一个能跑的计数器，带 `app.json` 和主机测试；`kit new` 从它起步 |
| `firmware/partitions/` | 圆屏的分区表（yaykit-v1） |
| `firmware/tools/yay_dev.py` | 写进圆屏、看日志、截图、看内存（`kit` 用它） |
| `config/` | 编译设置：应用仓库用的（`app.defaults`），提交用的那份（`release.defaults`） |
| `app/` | `kit new` 放进新应用仓库的几个文件 |
| `docs/` | 上面那些文档 |
| `skills/yaykit-app/` | 给 Claude Code 的「Yaykit 应用开发」skill |
| `devkit.json` | 这一版开发包的版本、对应的 Yay、接口级别、ESP-IDF 版本 |
