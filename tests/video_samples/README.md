# 视频测试素材

已下载/生成 13 个 MP4 样片，共 20,561,499 字节（约 19.61 MiB）。全部路径相对于本目录。文件用于后续解码、HDR、反交错和时间轴测试；不代表 VTEnc 已实现或硬件已支持对应功能。

## 样片清单

| 文件 | 实际视频格式 | 分辨率 | 长度 / 帧数 | 测试目的 |
| --- | --- | --- | --- | --- |
| `decode/h264_8bit.mp4` | H.264 High / 8-bit 4:2:0 | 1280×720 | 2.736 s / 82 | H.264 8-bit 解码，含 AAC |
| `decode/h264_10bit.mp4` | H.264 High 10 / 10-bit 4:2:0 | 320×180 | 2.736 s / 82 | H.264 10-bit 解码或明确拒绝，含 AAC |
| `decode/hevc_8bit.mp4` | HEVC Main / 8-bit 4:2:0 | 1280×720 | 2.736 s / 82 | H.265 8-bit 解码 |
| `decode/hevc_10bit.mp4` | HEVC Main 10 / 10-bit 4:2:0 | 1280×720 | 2.736 s / 82 | H.265 10-bit 解码 |
| `decode/av1_8bit.mp4` | AV1 Main / 8-bit 4:2:0 | 320×240 | 2.736 s / 82 | AV1 8-bit 解码 |
| `decode/av1_10bit.mp4` | AV1 Main / 10-bit 4:2:0 | 320×180 | 2.735 s / 82 | AV1 10-bit 解码 |
| `hdr/hlg_av1_10bit.mp4` | AV1 Main / 10-bit / HLG / BT.2020 | 1280×720 | 3.200 s / 96 | HLG 信号识别、传递和处理 |
| `hdr/hdr10_hevc_10bit.mp4` | HEVC Main 10 / PQ / BT.2020 | 1920×1080 | 2.000 s / 120 | HDR10、MDCV / CLL 静态元数据 |
| `hdr/dolby_vision_p5.mp4` | HEVC / Dolby Vision Profile 5 | 1920×1080 | 2.000 s / 120 | Dolby Vision 配置和逐帧 RPU；不能按普通 PQ 解释 |
| `hdr/dolby_vision_p81.mp4` | HEVC / Dolby Vision Profile 8.1 | 1920×1080 | 2.000 s / 120 | HDR10 兼容基础层及逐帧 RPU |
| `deinterlace/interlaced_tff_h264.mp4` | H.264 8-bit / TFF / 30000/1001 fps | 720×480 | 6.006 s / 180 | 普通交错：60000/1001 场/秒 |
| `deinterlace/telecine_32_tff_h264.mp4` | H.264 8-bit / TFF / 30000/1001 fps | 720×480 | 6.006 s / 180 | 24000/1001 → 30000/1001 的 3:2 hard telecine |
| `timing/vfr_h264_aac.mp4` | H.264 8-bit / VFR + AAC LC 44.1 kHz 双声道 | 640×360 | 220 帧；音轨 12 s | VFR PTS、B 帧 DTS、音视频起始偏移和尾部处理 |

这些短片适合功能回归，不用于长时间吞吐量、热稳定性或长时钟漂移测试。六种解码输入都是真实对应位深的码流；部分上游 10-bit 样片由 8-bit 素材转码，因此不能据此判断原生 10-bit 摄影内容的画质。硬件若不支持某个输入，测试应检查明确错误，禁止静默软件回退。

## 来源与派生方式

- 六种解码素材来自 [Chromium media/test/data](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/media/test/data/README.md)，下载后未修改。
- Dolby Vision 的两个文件同样来自 Chromium，原始来源是 Dolby 的 Glass Blowing 官方示例；上游已做无重编码截取和封装，本地保持文件原样。原始 Dolby URL 和上游命令见本地 `sources/chromium-README.md` 的 Dolby Vision 小节。实际文件各 120 帧、2 秒，以探测为准。
- HLG 来自 [WebM Project HDR Explorer 的 fountain_hlg.mp4](https://github.com/webmproject/hdr-explorer/blob/main/data/fountain_hlg.mp4)，下载后未修改。实际编码是 AV1；HLG 由码流中的 `arib-std-b67`、BT.2020 和 10-bit 像素格式共同核对。
- HDR10 从 Profile 8.1 文件通过 `-c:v copy -bsf:v dovi_rpu=strip=1` 提取。保留 PQ、BT.2020、mastering display 和 content light level SEI，去除 Dolby Vision 配置和 RPU。所有 120 帧基础层像素校验值与原始 P8.1 一致。原始 mastering peak 为 4000 nit；MaxCLL / MaxFALL 均为 0（未指定），原样保留。
- 普通交错样片把下载的 bear 视频按 2 倍速解释为 60000/1001p，再合并相邻时刻的上下场，生成 30000/1001i TFF；不是只修改 field-order 标志。Telecine 则先得到 24000/1001p，再用 `telecine=first_field=top:pattern=23` 生成 3:2 场重复。
- VFR 基于 bear 视频及其原有音轨循环派生；按下节的整数时间轴抽帧，保留连续音轨，并有意将视频整体延后 200 ms。

[manifest.json](manifest.json) 固定了上游 Git revision、下载 URL、SHA-256、字节数和完整 FFmpeg 参数。`sources/` 保存下载的上游说明和 HDR Explorer 的 Apache-2.0 LICENSE。第三方素材保留各自权利归属，不因为下载或出现在开源测试仓库中就自动获得额外再分发授权。

媒体、下载的上游文档和详细探测报告保留在本地并由本目录 `.gitignore` 排除；Git 提交清单、脚本、说明和精简验证快照，避免把第三方视频二进制直接纳入仓库。其他 checkout 可按清单重新下载并生成。

## VFR 时间轴判定基准

视频 time base 是 `1/30000`，共 220 帧。以未经抽帧的 30 fps 序列索引 `n = 0..359` 为基准，保留以下帧，输出 `PTS = 6000 + n * 1000`：

| 原始时间区段 | 选择规则 | 实际帧率 | 帧数 | 相邻 PTS 差值 |
| --- | --- | --- | --- | --- |
| `[0, 4)` 秒 | `n % 3 == 0` | 10 fps | 40 | 3000 ticks |
| `[4, 8)` 秒 | `n % 2 == 0` | 15 fps | 60 | 2000 ticks |
| `[8, 12)` 秒 | 全部 | 30 fps | 120 | 1000 ticks |

视频第一帧 PTS 为 `6000/30000 = 0.2 s`，最后一帧为 `365000/30000`；音轨从 `0` 开始，duration 为 `529200/44100 = 12 s`。视频延后是有意设置的测试条件，不能分别把两条轨道归零；也不能用帧序号除以平均帧率重建时间戳。

保留 B 帧，因此包的解码顺序与显示顺序不同：每条轨道的 DTS 严格递增，显示 PTS 应按帧重排。音频包含 AAC 编码延迟和容器中的 skip-samples 信息；不能把负的首包时间戳直接当作实际音轨起点。`reports/vfr_packets.json` 保存全部包和附带数据，CSV 便于对照。FFprobe 报告的视频 stream duration 为 `364000/30000`，与按显示 PTS 计算的跨度不同；检查尾帧时应区分轨道声明时长、包时长和显示时间轴。

## 下载、复现与验证

从仓库根目录执行。需要 Python 3、curl、FFmpeg / FFprobe；本次使用 Homebrew FFmpeg 9.0.2，支持 libx264 和 `dovi_rpu` bitstream filter。

```bash
brew install ffmpeg
# 需要代理时，在当前终端设置：
export all_proxy=http://127.0.0.1:7890

python3 tests/video_samples/prepare.py all
# 或按步骤执行：
python3 tests/video_samples/prepare.py fetch
python3 tests/video_samples/prepare.py generate
python3 tests/video_samples/prepare.py verify
```

已有文件先检查 SHA-256，不重复下载/覆盖；下载和生成先写临时文件，通过校验后再替换。FFmpeg 或 x264 版本、线程配置变化可能导致生成物字节不同，脚本会明确报 SHA-256 错误，并保留 `.part.mp4` 供检查。更新工具链时，应人工复核码流和生成参数，再有意更新清单与验证快照；不要跳过校验或把错误当成功。

验证包括全部样片的严格软件解码、真实 codec / pixel format、HDR 色彩信号和逐帧元数据、交错场检测、逆 Telecine 帧数以及 VFR 的完整预期 PTS 序列。普通交错样片 180/180 帧被 `idet` 判为 TFF，重复场为 0；Telecine 检出 Top 38 / Bottom 37 个重复场，逆 Telecine 得到 144 帧。`idet` 是内容启发式检查，需结合已知生成过程判断，不能单独作为任意未知素材的真值。

[verification.json](verification.json) 是本次成功验证的精简快照；重新运行写入 `reports/verification.json`，同时生成逐文件 FFprobe 报告、场检测日志、VFR 帧 PTS 和包时间戳。四个派生文件已在空输出目录重新生成，SHA-256 全部一致。

FFmpeg 的软件编解码只用于离线制作和核对测试输入，不链接到 VTEnc，不改变其强制硬件路径。本次未运行 VTEnc 硬件解码、HDR 渲染或全量 CTest；FFmpeg 解码成功不等于 VideoToolbox 硬件支持，也不等于 Dolby Vision 显示效果通过认证。
