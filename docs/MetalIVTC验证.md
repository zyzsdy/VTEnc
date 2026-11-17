# Metal IVTC 验证记录

日期：2026-11-17。真实 macOS ARM64 / Apple Silicon 本机执行；未运行全量 CTest。

## 移植范围

参考 rigaya/NVEnc `5bb35d860bc5f877b01fe60e4de5ac756b9b80ed` 的
`NVEncFilterIvtc.cu/.cpp`。MIT 声明见仓库 `THIRD_PARTY_NOTICES.md`。
移植 C/P/N 评分、32×8 块归约、场重组、三帧残余梳齿修复、亮度 SAD 与五帧去一帧。
CPU 只读取块统计、选择匹配并调度，像素运算在 Metal 上完成。

不是完整 NVEnc IVTC 兼容层：不含 RFF 展开、混合模式、cadence lock、五帧 BWDIF 或双场序置信度分类。
VTEnc 使用固定源场序，并针对干净 P→N 候选增加连续性保护；时间轴保持各源周期总时长，尾部不完整周期全部保留。
具体参数与约束见 README 的 Metal IVTC 一节。

## 构建与针对性检查

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R '^vtenc.cli$'
ctest --preset dev -R '^vtenc.timeline$'
./build/dev/vtenc_ivtc_test
./build/dev/vtenc_gpu_filter_test
python3 tests/ivtc_hardware.py --vtenc build/dev/vtenc
```

本机 CMake 位于 `build/tools/bin`；运行 Python 硬件脚本时将 `/opt/homebrew/bin` 加入 PATH。
构建成功，编译无新增诊断。链接仍提示本机 FFmpeg dylib 的最低系统版本 27.0 高于项目部署目标 13.0；此次仅验证当前机器，不据此宣称 macOS 13 运行兼容性。

- CLI 检查通过，涵盖裸选项、可选参数、互斥选项、未知/重复/越界参数及原始输入拒绝。
- 时间轴检查通过，包含有理数四等分、不能表示的时间基拒绝，以及 C/P/N 排序。
- `vtenc_ivtc_test`：90 个用例通过。检查 GPU SAD、分块评分、C/P/N 重组与残余修复；覆盖 NV12/P010、limited/full、TFF/BFF、非整线程块尺寸、P010 低位为零。另以已知电影帧生成五种相位的硬 telecine，检查中间输出逐像素恢复与无漏帧/重帧；验证 1–11 帧长度、VFR、非零 PTS、match-only 及所有 EOF 余数。
- 共享 Metal 回归：252 色彩转换、200 tone mapping、288 Bob、240 基础像素用例通过，共 980 个；最大误差 1 码值。

## 独立码流验证

`tests/ivtc_hardware.py` 使用 FFmpeg/ffprobe 独立解码、检查包时间戳与码流场标志。
测试夹具准备仅执行解码、参考滤镜或 remux；新增测试视频由 VTEnc 硬件编码生成。
最终运行报告：`build/hardware-tests/ivtc-lzoja8cw/report.json`（本机生成物，不提交）。

| 用例 | 输入 → 输出帧数 | 核验 |
| --- | --- | --- |
| 原始 TFF hard telecine / `--avsw` | 180 → 144 | H.264 逐行，24000/1001，6.006 秒，逐包 PTS/duration |
| 同源 / 自动解码策略 | 180 → 144 | 明确硬解失败诊断后从头软解，实际编码端确认为硬件 |
| `cycle=0` | 180 → 180 | 不降帧，逐行输出，保留原始显示区间 |
| IVTC＋crop＋resize＋colorspace | 180 → 144 | HEVC Main10，360×240，full range、BT.601 元数据 |
| 视频偏移 0.2 秒＋AAC | 180 → 144 | 保留视频首尾及音轨每包时间与 SHA-256 payload |
| 7 帧短片 | 7 → 6 | 完整周期 5→4，尾部两帧全部保留，片尾不延长 |
| BFF P010 / `--avhw` | 30 → 24 | 真实硬解＋Metal＋HEVC 10-bit 硬编 |
| 同一 BFF P010 / `--avsw` | 30 → 24 | 软解＋Metal＋HEVC 10-bit 硬编 |
| `-o - -f nut`＋`log=true` | 7 → 6 | stdout 可完整解码，无文本污染，PTS 与文件输出一致，逐帧场标志为逐行 |

NUT 在此路径不保存 packet duration，也不提供可用于本测试断言的容器 field_order。
stdout 用例检查实际解码帧的 interlaced/repeat 标志、帧数、PTS 与严格解码成功；不伪造 NUT 的 packet duration 验证。
MP4 用例均逐包检查 duration。

## 画面与 cadence

以素材清单中用于生成 telecine 的原始 `decode/h264_8bit.mp4` 为参考，重新执行相同的 loop、720×480 缩放与 24000/1001 fps 转换，对齐 144 帧比较：

- Metal IVTC＋VT H.264 硬编：平均 PSNR **43.339010 dB**，最低单帧 **42.265319 dB**。
- 该检查已进入硬件脚本，要求平均至少 40 dB、最低单帧至少 38 dB，用来发现“帧数正确但电影时刻漏失/重复”的回归。
- 最初仅逐帧选最低梳齿评分时，P/N 干净候选会因压缩噪声切换，造成少量电影时刻漏失。加入连续性约束后，以上测试通过。

此 PSNR 只说明当前参考片段的恢复质量，不代表所有剪辑、噪声、字幕或混合内容都能无损还原。

## 解码边界

原始交错 H.264 样片在本机 VideoToolbox 创建硬解会话时仍返回 `OSStatus=-12911`。
自动模式的成功来自已明确记录的一次软解重试，不将其记为该原始文件硬解成功。
BFF 10-bit 用例使用逐行编码但内部包含交织场的 HEVC 载体，显式指定场序；实际硬解和硬编均已成功。
该载体验证不等同于原始交错码流的硬解支持。
