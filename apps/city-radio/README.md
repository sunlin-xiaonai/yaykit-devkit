# city-radio

一个 Yay 圆屏应用（Yaykit 扩展固件），用 Yaykit 开发包做的。

```bash
./kit build              # 编译，再跑主机测试
./kit install --slot 2   # USB 写进测试圆屏的位置 2，写完直接打开
./kit logs               # 看日志；./kit snap 截图，./kit heap 看内存
./kit build --release    # 提交用的那份
```

- 在开发包仓库的 `apps/city-radio/` 里，`devkit/` 是指向仓库根目录的链接（`devkit -> ../..`），用的就是这个仓库当前的开发包。别改它。
- 应用清单是 `app.json`；逻辑写在 `main/`，能不依赖 ESP-IDF 的部分写成纯 C，`test/run.sh` 在电脑上测。
- 做完、自测过，照开发包的「从立项到上架」开提交单。
