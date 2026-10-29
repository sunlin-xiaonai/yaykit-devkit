# 上手：从零到第一次写进圆屏

大约半小时，大半是等 ESP-IDF 下载。

## 要准备的

- 一台 Mac（Apple 芯片或 Intel 都行），装了 Xcode 命令行工具（没装的话 `./kit setup` 会弹窗让你装）。
- 一台测试圆屏，装着 Yay（正式版就行），连着 Wi-Fi。
- 一个 Yay 账号：自己注册，在 Yay App 里扫码绑你的测试圆屏，再把账号**私下**告诉我们（不要写在公开的单子里）。我们把它加进内部测试名单，给你开发用的点数。
- 一根能传数据的 USB-C 线。
- 你自己的 GitHub 账号。

## 1. 装环境（只做一次）

```bash
git clone https://github.com/vzheng56/yaykit-devkit.git
cd yaykit-devkit
./kit setup
```

`setup` 把 ESP-IDF 5.5.5 装到 `~/esp/esp-idf-v5.5.5`，钉在开发包要的那个提交上，再装 ESP32-S3 的编译工具。已经有同一个提交的 ESP-IDF（`$IDF_PATH` 或 `~/esp/esp-idf`），就直接用它。在国内下载慢，用 `./kit setup --cn`：ESP-IDF 走 Gitee，编译工具走乐鑫的国内镜像。

## 2. 起一个应用

```bash
./kit new my-timer             # 在当前目录下建 my-timer/；--dir <目录> 放别处
cd my-timer
```

`my-timer/` 是一个新的 git 仓库，里面是模板（一个计数器：短按加一，按住清零），已经改好了名字：

| 文件 | 是什么 |
|---|---|
| `app.json` | 应用清单：名字、版本、怎么打开、要哪些能力（[规范](app-json.md)） |
| `main/main.c` | 程序：启动顺序、界面、侧键、命令、退出前写结果 |
| `main/counter.c` | 应用的逻辑，纯 C，不依赖 ESP-IDF |
| `test/run.sh` | 主机测试：在电脑上测 `counter.c` |
| `devkit/` | 开发包（git 子模块），钉在建应用时的版本，只读 |
| `kit` | 这个应用的 `kit` 命令 |
| `CLAUDE.md`、`.claude/skills/yaykit-app/` | 给 Claude Code 看的规矩和做法 |

在 GitHub 上建一个空仓库，推上去。仓库可以公开；私有的话，把 `vzheng56` 加成协作者，我们审核时要读它：

```bash
git remote add origin git@github.com:<你>/my-timer.git
git push -u origin main
```

别人（或换一台电脑）`git clone` 下来以后，第一次跑 `./kit` 会自己把 `devkit/` 拉下来。

## 3. 编译

```bash
./kit build
```

第一次要几分钟（还要从乐鑫的组件库下载 LVGL 这些依赖），之后只编改了的。编完会跑主机测试，并检查镜像不超过 3.5 MB。

## 4. 写进圆屏

用 USB 线把圆屏接到 Mac 上（圆屏开着就行，不用按任何键）：

```bash
./kit install --slot 2
```

它先让圆屏进恢复器，把镜像写进位置 2，然后直接打开。位置 1 是 Yay，碰不到；也不擦别的。位置 2 原来有应用的会被换掉（以后可以从 Yay 的「我的应用」再装回来）。

打开以后：

- **短按**侧键加一，**按住**清零；
- **双击**侧键开应用环，点中间的 Yay 回去；
- 回到 Yay 以后，双击开应用环也能看到它，点它再打开。

## 5. 看它在干什么

```bash
./kit logs 60       # 60 秒的日志（MY_TIMER_TAP、YAY_APP …）
./kit snap          # 截图存成 snap.png
./kit heap          # 内存：内部 RAM 不能低于 40 KB
./kit info          # 圆屏上现在跑的是什么、三个位置各装着什么
```

## 6. 接下来

- 读[应用开发指南](app-guide.md)：组件怎么用、侧键归谁、怎么回到 Yay、怎么给 Yay 留结果。
- 读[圆屏设计规范](round-screen-design.md)：颜色、字号、安全圆、提示怎么写。
- 把 `counter.c` 换成你的逻辑，把界面换成你的。
- 做完先 `./kit build --release` 再 `./kit check`，全过了照[从立项到上架](from-idea-to-store.md)提交。

## 出问题了

| 现象 | 怎么办 |
|---|---|
| `没找到 ESP-IDF` | 在开发包目录跑 `./kit setup` |
| `install` 说找不到圆屏 | 换一根能传数据的线；`ls /dev/cu.usbmodem*` 要能看到它 |
| `install` 卡住、圆屏黑屏 | 拔线再插；圆屏重启以后照常回到 Yay。不要按住侧键插线（那会进下载模式） |
| 编译说依赖锁变了 | `dependencies.lock` 跟着开发包走，别手改；换开发包版本以后重编一次再提交 |
| 我编的 SHA-256 和审核重编的对不上 | 有没提交、没推上去的改动；或者应用放在 `/tmp` 下编了（macOS 上那里编出来的字节每次不一样）：放到 `~/` 下面的目录再编 |
| 镜像超过 3.5 MB | 少放图和字；图放 PSRAM 解码，别整张编进程序 |
