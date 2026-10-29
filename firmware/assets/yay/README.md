# Yay 角色素材（圆屏）

脚本 `yaykit/scripts/export-yay-art.ts` 生成，**不要手改**。规格见 `docs/design/yay-device.md` 第 6.11 节。

## 现在是占位素材

这些文件用 35cae99（`docs/yay-character-v2-spec` 分支）的栅格定稿画稿生成。关口 B 已定为「全矢量」，以后改成从 rig 出图：身体不带五官，五官用矢量几何画。到时文件名、尺寸、`yay_art.h` 的接口都不变，固件不用改。

## 怎么生成

画稿还没进 main，要先取那个提交，再用 `--rev` 读（不检出、不切分支）：

```sh
git fetch origin docs/yay-character-v2-spec
cd yaykit && pnpm art:export --rev 35cae99
pnpm art:export --rev 35cae99 --check     # 只核对这里的固件文件，不一致就报错
```

- `--source <目录>`：读磁盘上的一套画稿（目录结构和 `design/yay/source/character` 一样）。和 `--rev` 只能二选一，两个都给会报错。画稿进 main 以后，不带参数就读 `design/yay/source/character`。
- 默认只写这里的固件素材。iOS 的 @2x/@3x 姿势图要显式给 `--ios-out <Assets.xcassets/Poses 目录>` 才写；`--check` 不管 iOS。
- `--preview <png>`：把所有固件图从 I8 数据解码，拼成一张图，方便看。
- **以 macOS arm64（sharp 0.35.5）上的输出为准**。同一台机器上重跑逐字节相同；换平台（比如 Linux x64）缩放结果可能差 1 级，量化后的字节就不同，这时 `--check` 报差异是正常的，回到 macOS arm64 重新生成。

所有画稿用同一个裁切框 `[71, 69, 953, 951]`（1024 画布上的 882 × 882，右下不含），再缩到各档。

## 文件

| 文件 | 尺寸 | 字节 |
|---|---|---|
| `yay_ready_176.bin` | 176 × 176 | 32012 |
| `yay_listening_176.bin` | 176 × 176 | 32012 |
| `yay_thinking_176.bin` | 176 × 176 | 32012 |
| `yay_working_176.bin` | 176 × 176 | 32012 |
| `yay_approval_176.bin` | 176 × 176 | 32012 |
| `yay_done_176.bin` | 176 × 176 | 32012 |
| `yay_reminder_176.bin` | 176 × 176 | 32012 |
| `yay_help_176.bin` | 176 × 176 | 32012 |
| `yay_resting_176.bin` | 176 × 176 | 32012 |
| `yay_speaking_176.bin` | 176 × 176 | 32012 |
| `yay_ack_176.bin` | 176 × 176 | 32012 |
| `yay_greet_176.bin` | 176 × 176 | 32012 |
| `yay_working_laptop_176.bin` | 176 × 176 | 32012 |
| `yay_ready_264.bin` | 264 × 264 | 70732 |
| `yay_resting_264.bin` | 264 × 264 | 70732 |
| `yay_greet_264.bin` | 264 × 264 | 70732 |
| `yay_greet_132.bin` | 132 × 132 | 18460 |

17 张，合计 646812 字节（631.7 KB）。176 档是 12 个状态加拿电脑的 `working_laptop`；264 档 ready、resting、greet（待命）；132 档 greet（配对页）。

- `.bin`：LVGL 9 的二进制图片：12 字节头（magic `0x19`、颜色格式 I8 `0x0A`、宽、高、stride），256 个调色板颜色（每个 B、G、R、A 四字节，alpha 不预乘），再每像素一个字节的索引。也能直接给 LVGL 的文件系统读。
- `yay_art.h`、`yay_art.c`：每张图的 `lv_image_dsc_t`（数据指向嵌入的 `.bin` 跳过 12 字节头），和表 `yay_art[]`：姿势、尺寸、眼睛和嘴的框、眼睛圆角、嘴的三档、肤色。`yay_art_find("greet", 132)` 按名字查。
- `yay_art.cmake`：画 Yay 的组件 `include` 它，把 `YAY_ART_SRCS`、`YAY_ART_INCLUDE_DIRS`、`YAY_ART_EMBED_FILES` 加进 `idf_component_register`（用法写在文件头）。
- `face.json`：同样的数据，另有眼睛和嘴的中心、在 1024 画稿上的原始框、每张画稿的 SHA-256。

## 眼睛和嘴

坐标都是导出图自己的像素，左右按屏幕上看到的。

- 眼睛：从画稿里量出两只并排的深色椭圆（贴着的眉毛不算）。done、resting 画稿本来就闭着眼，量的是那两道弧，`eyes_open` 为 false，不眨眼。圆角 `eye_radius` = 较小那只眼的 min(w, h) / 2，向下取整。
- 嘴：眼睛下方居中的深色或粉色形状。说话时的三档 `mouth_levels`（闭、微张、张开）每个姿势都一样，以嘴框中心为中心：闭 = ready 那条微笑的宽度和线粗，张开 = speaking 的嘴，微张取两者中间。
- 画稿本身带着画好的五官。眨眼、对口型时，先把眼睛或嘴的框四边各外扩 `YAY_ART_COVER_PAD`（2 px），用 `skin` 色的圆角矩形盖住，眉毛、眼皮线和抗锯齿留下的残边也一起盖掉；再用 `YAY_ART_INK`（`#242422`）画新的。
