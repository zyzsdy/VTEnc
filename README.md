# VTEnc

面向 Apple Silicon M 系列的原生 VideoToolbox 硬件编码器，借鉴 NVEncC 的 CLI、能力探测和模块划分，使用 C++20 + 少量 Objective-C++。

本仓库独立于 NegativeEncoder；未来通过命令行协议集成。功能范围以截至 M6 的硬件支持为目标，硬编能力以当前系统的真实探测为准。编码始终使用硬件；解码支持显式硬解、FFmpeg 软解，以及硬解失败后一次软解重试。

## 当前状态

已实现 P1 硬编闭环、基于 avformat 的多格式输入转码，以及 P3 GPU 缩放、裁剪、基础色彩转换与 Bob 反交错。现有功能：

- CMake / Ninja 的 Debug、Release 构建与安装目标。
- 纯 C++ CLI、参数校验，以及独立的 Apple 框架适配库。
- H.264 8-bit、HEVC 8/10-bit 的硬件会话探测与支持属性枚举。
- Objective-C++ / ARC 的 Metal 设备查询。
- 合成测试帧 / 原始 NV12、P010 输入，H.264 High 8-bit、HEVC Main 8-bit / Main10 硬件编码。
- 平均码率目标或原生 Quality、GOP 上限、整数有理数帧率；Annex B 输出及逐帧时间戳 CSV。
- 压缩输入统一交给 `libavformat` 自动探测，不设格式或扩展名白名单；支持实测 MKV、TS、WebM、MP4/MOV 等，探测失败直接报错。原生 VideoToolbox 硬解或 `libavcodec` 软解，原生硬编，avformat 根据输出文件名选择封装器，或由 `-f` / `--output-format` 显式指定；默认复制第一条音轨。
- 文件转码将源 PTS、VFR、A/V 起点偏移、像素宽高比和显示变换传入封装器，精度与元数据保存取决于目标格式；支持 `hev1` 描述适配。
- Metal 裁剪 / 双线性缩放、NV12/P010 8/10-bit 与 limited/full range 转换、BT.601/BT.709/BT.2020 NCL/SMPTE 240M 矩阵转换，以及 `--vpp-colorspace` 线性光色域、传递函数转换，以及 Hable / Mobius / Reinhard / BT.2390 HDR→SDR tone mapping。
- Metal Bob 按 TFF/BFF 输出两场对应的逐行帧，精确场倍频；支持原始 NV12/P010、硬解或软解的文件输入。两个原始交错样片在本机无法硬解，可用软解接入 Metal Bob。
- 有界回调和重排队列、背压和排空；SIGINT 取消、失败清理、输出防覆盖。
- 不依赖硬件编码器的 CLI / 码流适配检查，以及显式硬件集成验证脚本。

完整开发方案见 [docs/开发方案.md](docs/开发方案.md)。

## 环境准备

需要原生 arm64 macOS、支持 C++20 的 Apple Clang、macOS SDK、CMake 3.25+、Ninja 和 FFmpeg 7.0+ 开发库（`libavformat`、`libavcodec`、`libavutil`）。最低部署版本暂定 macOS 13；这不代表所有目标功能在 macOS 13 上均可用，新 API 必须检查系统版本与会话能力。

```bash
xcode-select --install  # 尚未安装 Xcode Command Line Tools 时执行
brew install cmake ninja ffmpeg
cd /Users/tedzy/Workspace/VTEnc
./scripts/bootstrap.sh
```

`bootstrap.sh` 检查平台与工具并构建 Debug，不修改系统配置、不安装依赖、不自动运行测试。SDK 由 `xcode-select` / `xcrun` 选择，不在仓库中硬编码 SDK 路径。自定义 FFmpeg 安装可通过 `cmake --preset dev -DCMAKE_PREFIX_PATH=/your/ffmpeg/prefix` 指定。FFmpeg 的头文件和动态库必须来自同一安装；运行时需要保留这些动态库。实际可运行的最低 macOS 版本同时受 FFmpeg 库及其依赖的部署目标约束，本机 Homebrew FFmpeg 9.0.2 使用 macOS 27.0 部署目标，不能据此声称产物兼容 macOS 13。当前无需完整 Xcode；Metal Shader 源码嵌入可执行文件，运行时编译，失败直接报错。

也可以将构建工具安装到项目内隔离环境（本次环境初始化采用此方式）：

```bash
python3 -m venv build/tools
build/tools/bin/python -m pip install 'cmake==3.31.6' 'ninja==1.11.1.4'
source build/tools/bin/activate
./scripts/bootstrap.sh
```

后续终端使用该环境前执行 `source build/tools/bin/activate`。Python 用于安装开发工具及准备测试素材，`vtenc` 本身没有 Python 运行时依赖；`build/` 整体不纳入 Git。

2026-11-16 已在 Apple M6、macOS 27.2.1、Apple Clang 21 / macOS SDK 27.2 上验证 Debug / Release 构建、本地安装和 CLI 检查；H.264 8-bit、HEVC 8-bit / 10-bit 的 1920×1080 硬件会话均成功准备并确认硬件后端。这不是码流质量验证，也不代表其他机型或旧系统已完成验证。

## 构建与运行

```bash
cmake --preset dev
cmake --build --preset dev
./build/dev/vtenc --help
./build/dev/vtenc --version
./build/dev/vtenc --check-device
./build/dev/vtenc --check-features --codec h264
./build/dev/vtenc --check-features --codec hevc --output-depth 10

# 只运行与 CLI 有关的检查
ctest --preset dev -R '^vtenc.cli$'

# 发布构建与本地安装
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix "$PWD/build/install"
```

能力探测固定使用 1920×1080 的 NV12 / P010 输入约束，设置对应 Profile，准备会话并核验 `UsingHardwareAcceleratedVideoEncoder`。它不提交帧，不验证真实码流，不代表最大分辨率、吞吐量或属性值组合均可用。硬件忙、配置不受支持等错误直接报告 OSStatus 并以非零状态退出，不降级到软件编码。

退出码：`0` 成功、`1` 运行时或硬件错误、`2` 参数错误、`130` 用户取消。文件输出任务的结果写入 stdout，诊断写入 stderr。使用 `-o -` 时 stdout 只含媒体码流，结果、日志和诊断全部写入 stderr。文本结果尚不提供 JSON 协议。

## P1 硬件编码

```bash
# 合成 65 帧 1080p、30000/1001 fps 的 H.264 码流
./build/dev/vtenc --synthetic --input-res 1920x1080 --frames 65 \
  --fps 30000/1001 --codec h264 --bitrate 5000 --gop-len 60 -o build/demo.h264

# 原始 P010：HEVC Main10，Quality 是 VideoToolbox 原生 0..1 质量目标
./build/dev/vtenc -i input.p010 --input-res 1920x1080 --frames 120 \
  --fps 24000/1001 --codec hevc --output-depth 10 --quality 0.7 -o build/demo.hevc

# 无硬件码流适配检查
ctest --preset dev -R '^vtenc.bitstream$'

# 显式硬件集成验证：需要真实 Apple Silicon、ffprobe、ffmpeg、Python 3
python3 tests/encode_hardware.py --vtenc build/dev/vtenc
```

输入尺寸和帧数必须显式提供。`-i` 只接受普通原始文件：8-bit 是紧密排列的 NV12（Y 后接交错 UV），10-bit 是小端 P010（每个 16-bit 字的高 10 位有效，低 6 位为零）；文件中没有行填充。`--output-depth` 同时决定原始输入格式，不做位深转换。只读 `--frames` 指定的帧数，尾部数据不消费，输入不足则报错。合成输入为移动灰阶渐变。

本阶段限定逐行、video range、BT.709 SDR；调用方负责提供符合约定的原始数据。10-bit 不等于 HDR；本阶段不读取或转换输入色彩元数据。帧率是整数或整数分数，PTS 从 0 开始，以 `帧号 × fps_den / fps_num` 构造，不做浮点累积。默认禁用帧重排；原始帧输入使用固定帧率，不携带音频，可输出裸码流或 avformat 容器。

默认平均码率目标为 5000 kbit/s。`--bitrate` 绑定 `AverageBitRate`，是软目标，不保证输出大小；`--quality` 绑定原生 `Quality`，不等价于 CRF/CQP，两者不能同时指定。`--gop-len` 是最大关键帧间隔，编码器可以提前插入关键帧。所有属性设置检查 OSStatus，不支持就失败，不做软件回退。

输出格式由文件名或 `-f` / `--output-format` 决定，例如 `.mkv`、`.ts`、`.mp4`，与压缩输入转码使用相同的封装器选择规则。选择与编码器匹配的 H.264 / HEVC 裸码流文件时，保留 Annex B 和时间戳 CSV 输出（`-o -` 只有码流，不生成 CSV）：同步帧前写入 SPS/PPS（HEVC 还包括 VPS），将长度前缀 NAL 转为起始码。裸码流不携带容器级 PTS/DTS，播放器或 FFprobe 猜测的帧率可能不正确；封装时需指定本次输入帧率。`<输出路径>.timestamps.csv` 保存编码回调原始整数 PTS/DTS/duration、同步帧标记及该帧的字节范围；VideoToolbox 未指定的 DTS 保持为空，范围包括该帧前的参数集。

同目录临时文件在全部排空、帧数与时间戳校验、硬件后端复核后发布；已有输出或时间戳文件均不覆盖。每批最多 8 帧，回调只保留样本或记录错误，调度线程负责码流写出。SIGINT 停止新提交、排空已提交帧、删除临时输出并返回 130。运行错误返回 1 并清理临时输出；进程被强制杀死或系统崩溃时可能残留 `.part.*` 文件。两个文件分别原子发布，不承诺跨文件的崩溃事务。

2026-11-16 在 Apple M6 / macOS 27.2.1 上完成三种格式的真实硬编及独立码流验证，覆盖合成输入、非对齐宽度原始输入、尾批排空及异常路径。详见 [P1 验证记录](docs/P1验证.md)。该结果不代表其他硬件、旧 macOS、任意分辨率或 HDR 已验证。

## 压缩输入转码与输出格式

```bash
# H.264 输出，默认复制第一条音轨（也可以显式写 --audio-copy）
./build/dev/vtenc -i input.mp4 -o build/output.mov --codec h264 --bitrate 5000

# 10-bit 输入保持位深，输出 HEVC Main10
./build/dev/vtenc -i input10.mov -o build/output10.mp4 --codec hevc --output-depth 10

# video_samples 中的 VFR / AAC 完整转码
./build/dev/vtenc -i tests/video_samples/timing/vfr_h264_aac.mp4 \
  -o build/vfr.hevc.mp4 --codec hevc --audio-copy

# 该旧样片 AAC 有重复 PTS，明确关闭音频可转码其完整视频轨
./build/dev/vtenc -i tests/video_samples/decode/h264_8bit.mp4 \
  -o build/bear.mp4 --no-audio

# 相关规则与显式硬件集成检查
ctest --preset dev -R '^vtenc.timeline$'
python3 tests/transcode_hardware.py --vtenc build/dev/vtenc
```

不提供 `--input-res` 的文件输入全部走 avformat：不检查支持格式列表，也不依赖扩展名。MKV、TS、WebM、未知扩展名、无扩展名或错误扩展名均直接尝试解封装；失败保留 FFmpeg 的操作名、错误文本和错误码。输出不限制扩展名或格式列表：省略 `-f` 时由 avformat 根据输出文件名选择封装器；指定 `-f FORMAT` 或 `--output-format FORMAT` 时优先使用该 FFmpeg 封装器名称。无法推断格式、未知封装器或不兼容的流直接报错，不触发软解回退。文件模式读取源尺寸和时间轴，拒绝 `--fps`、`--frames`。`--input-res` 明确选择 P1 紧密排列原始帧模式，此时还需 `--frames`，且不能指定 `--avhw` / `--avsw`。

输出示例（编码器始终为 VideoToolbox 硬件编码器）：

```bash
./build/dev/vtenc --avhw -i input.mkv -o build/output.ts
./build/dev/vtenc --avsw -i input.ts -o build/output.mkv
./build/dev/vtenc -i input.anything -o build/output.data -f matroska
./build/dev/vtenc -i input.mkv -o build/extensionless --output-format mpegts
# 裸视频封装器不接受音轨，需要明确省略音频
./build/dev/vtenc -i input.mkv -o build/output.h264 --no-audio
```

`-f` 与 `--output-format` 是同一选项，重复指定报错。格式名称由链接的 FFmpeg 提供，不在 CLI 中维护名单，也不会改变 `--codec` 的选择。本地文件输出保持单文件事务：完成后原子发布，不覆盖已有文件，失败或取消清理临时文件。不支持自行管理 IO / 多文件的封装器（如 HLS、segment）。

`-o -`（或 `--output -`）直接流式写入标准输出，必须显式提供 `-f` / `--output-format`，缺少格式在打开输入和硬件会话之前返回参数错误。压缩输入、原始帧和合成输入均适用；不创建名为 `-` 的文件、临时输出或时间戳 CSV，所有任务日志和诊断写入 stderr。

```bash
# 媒体流与日志分别重定向
./build/dev/vtenc --avhw -i input.mkv -o - -f mpegts > output.ts 2> vtenc.log
# 直接通过管道交给后续程序
./build/dev/vtenc --avsw -i input.ts -o - --output-format matroska | ffplay -i -
# 原始裸码流也可输出到管道
./build/dev/vtenc --synthetic --input-res 320x180 --frames 60 -o - -f h264 > output.h264
```

stdout 始终按不可 seek 的流交给 avformat。所选封装器必须能写入这种流；例如 MPEG-TS、Matroska、H.264/HEVC 裸码流可用，默认 MP4/MOV 需要 seek 回写，会明确报错；当前不自动启用 fragmented MP4。编码始终为硬件，不因目标封装改变编码器。

标准输出已经发送的字节无法撤回。默认解码模式只允许在开始 stdout 封装之前、且输入可从头读取时软解重试一次；开始封装后的硬解错误直接退出，避免拼接两次转码的码流。若需要确保使用软解，应预先指定 `--avsw`。取消和失败可能留下部分码流；下游必须检查退出码。下游提前关闭管道会报告 Broken pipe 并返回 1，SIGINT 返回 130，诊断不会声称标准输出已被删除。

显式运行输出封装回归（不注册为默认 CTest）：

```bash
python3 tests/output_formats_hardware.py --vtenc build/dev/vtenc
python3 tests/stdout_hardware.py --vtenc build/dev/vtenc
```

压缩输入 `-i -` 交给 avformat 读取标准输入。不可 seek 的管道 / 实时输入无法从头重试：默认模式若硬解失败，会明确报错并提示预先选择 `--avsw`，不会从已消耗数据的中间位置伪造一次成功重试。

解码模式（适用于所有压缩输入，两个参数互斥）：

- `--avhw`：avformat 解封装后强制原生 VideoToolbox 硬解，创建会话和处理帧时的硬解错误直接退出，不回退。
- `--avsw`：直接通过 FFmpeg `libavformat` 解封装、`libavcodec` 软解，不创建硬解会话。
- 两者省略：先尝试硬解。仅硬解错误触发一次完整重试：报告原始诊断，销毁解码、编码与封装状态，清理临时输出，重新打开输入并从头软解。软解再失败则退出；编码、滤镜、输入时间轴、音轨和写文件错误不重试。

两条路径共用同一个 avformat 输入上下文：选取视频轨与第一条音轨，按需读取，音视频交织缓存限制为 64 MiB / 4096 包。没有 AVFoundation 输入依赖。硬解路径将 avcC / hvcC 或 Annex B 参数集构造成 CoreMedia 格式描述，把 H.264 / HEVC NAL 统一为四字节长度前缀，AV1 使用 av1C；之后显式调用并验证 VideoToolbox。支持的硬解视频适配仍是 H.264、HEVC、AV1，不根据芯片名称推断可用性。硬解 B 帧显示队列上限 32 帧，容器缺失 DTS 时不伪造 DTS。

软解路径直接将同一解封装器的数据包提交 `libavcodec`，不执行 `ffmpeg` 子进程；只选择软件解码器和 CPU 像素格式，处理 send/receive、B 帧与 EOF 排空，将 8/10-bit 4:2:0 帧无损重排为 Metal 兼容 NV12/P010。其他软解像素布局直接报错，不隐式转换色度或位深。

两条路径都保留整数有理数 PTS、VFR 和源颜色信息；软解检测损坏帧、尺寸或格式变化并报错。H.264 / HEVC 编码始终沿用原生 VideoToolbox 硬件编码器，强制并检查实际硬件后端。成功日志分别报告软解或硬解，不把软解标为“硬解已验证”。

```bash
./build/dev/vtenc --avhw -i input.mkv -o build/hardware.mp4
./build/dev/vtenc --avsw -i input.ts -o build/software.mp4
./build/dev/vtenc -i input.anything -o build/automatic.mp4

# 策略测试不创建硬件会话；集成验证必须显式运行
ctest --preset dev -R '^vtenc.decode-policy$'
python3 tests/decode_modes_hardware.py --vtenc build/dev/vtenc
python3 tests/avformat_hardware.py --vtenc build/dev/vtenc
```

2026-11-17 已在本机用 FFmpeg 9.0.2 完成解码模式的 13 个成功转码用例的独立码流 / PTS 验证，覆盖 H.264、HEVC、AV1 的 8/10-bit 软解、VFR 与 AAC 保真、旋转、默认硬解和强制硬解，以及两种交错样片的软解 / 自动回退 Bob。另验证了互斥参数、只重试一次、迟发损坏包、SIGINT、输出防覆盖和 HDR 拒绝；未执行全量测试。Telecine 样片的逐帧场标记存在变化，Bob 验证显式使用 `--field-order tff`。

统一 avformat 输入后，另完成 20 个新增输入转码用例：MKV/TS 的 H.264 VFR 与 HEVC 10-bit、WebM AV1、错误 / 未知 / 无扩展名、交错输入软解及一次自动回退、标准输入。验证完整码流、逐帧 PTS、末帧时长、AAC 压缩内容和解码 PCM；音频换算误差不超过半个输出采样刻度。原有 MP4/MOV 硬解转码、音轨 / 旋转、取消与写入失败回归也通过。未运行全量测试。

FFmpeg 来自外部安装，本仓库没有复制其上游实现。FFmpeg 的基础许可证为 LGPL 2.1-or-later，启用 GPL/version3 等配置会改变该构建的许可证；本机 Homebrew 库启用了 GPL 与 version3。分发时须按实际链接库的许可证提供对应源码、配置及声明，不能将任意 Homebrew 构建视为 LGPL-only。见 [FFmpeg 许可证说明](https://ffmpeg.org/legal.html)。VTEnc 不调用 FFmpeg 的视频编码 API。

音频通过 avformat 读取并复制到所选输出容器，不进行音频重编码；必要时由封装器执行 AAC ADTS → AudioSpecificConfig 位流适配。默认复制第一条音轨，`--no-audio` 禁用。与目标容器不兼容的音频直接报错并提示 `--no-audio`。AVPacket 的 priming / trim 边信息随包传入封装器，能否写入取决于目标格式（例如 TS 不保存 AAC priming / trim）；不同容器的音频首尾解码采样数可能不同。音视频共用输入时间原点，不分别归零。

视频 PTS 使用整数有理数传递，相邻 PTS 决定显示时长。TS 等容器可能没有末帧 duration：此时明确打印诊断并沿用前一个显示间隔；只有一帧且无 duration 时仍报错。音视频的输入时钟未必能精确映射到封装器选定的输出时钟（例如 MKV 常用 1/1000，TS 使用 1/90000）；写入时采用整数有理数就近舍入，误差不超过半个输出刻度，并首次打印换算诊断。可精确表示时不舍入；输出时钟若把不同视频帧的 PTS 或有效时长压成同一刻度，直接报错。封装器最终可保存的时长、旋转与边信息取决于格式；裸码流没有容器级时间戳。源音频 PTS 不递增、损坏包、流参数变化或目标封装失败均直接退出。

当前限定一个视频轨；HDR 输入必须显式指定 `--vpp-colorspace`，交错输入必须显式启用 Bob。支持位深、尺寸和色彩空间转换；不支持 Dolby Vision、音频转码、多音轨选择、字幕、章节与附件。显示旋转作为容器变换交给输出封装器（目标格式必须能保存该元数据）；输入编辑列表和丢弃标记按 avformat 语义处理，不再经过 AVFoundation 的单段编辑列表校验。

本地文件任务取消或运行错误不发布成品，临时目录自动清理；成功写完 trailer、检查写入错误并关闭文件后，以不覆盖既有文件的方式发布。强制杀进程或系统崩溃可能残留 `.part.*` 临时目录。

已使用 `video_samples` 的全部 13 个原始样片实施正向转码或明确拒绝检查，另以无重编码派生 MOV 验证输入容器和旋转。9 个成功转码用例的码流和时间轴均通过独立检查，VFR 音轨逐包哈希、PTS 和解码 PCM 完全一致。详见 [P2 验证记录](docs/P2验证.md)。

## P3 GPU 缩放、裁剪与基础色彩转换

```bash
# 在存储像素坐标裁去左/上/右/下边缘，再缩放；同时将 10-bit 转成 8-bit
./build/dev/vtenc -i tests/video_samples/decode/hevc_10bit.mp4 \
  -o build/scaled.mp4 --crop 16,8,16,8 --output-res 640x352 --no-audio

# 明确指定源矩阵解释，转换为 BT.601 系数、full range、HEVC Main10
./build/dev/vtenc -i tests/video_samples/decode/h264_8bit.mp4 \
  -o build/color.mp4 --input-matrix bt709 --output-matrix bt601 \
  --output-range full --codec hevc --output-depth 10 --no-audio

# 显式执行相关检查；硬件检查不会注册到默认 CTest
ctest --preset dev -R '^vtenc.cli$'
./build/dev/vtenc_gpu_filter_test
python3 tests/filter_hardware.py --vtenc build/dev/vtenc
```

这些滤镜用于所有 avformat 压缩输入。处理顺序是裁剪、双线性采样、YCbCr 矩阵 / range / 位深转换。`--crop LEFT,TOP,RIGHT,BOTTOM` 是四个非负偶数边距，必须留有正偶数宽高；坐标基于未应用容器旋转的存储像素。省略 `--output-res` 时输出裁剪后的尺寸，显式指定时精确拉伸到目标偶数宽高，保留源像素宽高比，不自动补黑边。几何变换后 clean aperture 重置为完整输出栅格；容器显示方向保持，平移按新尺寸调整。

`--output-depth` 决定文件输出位深，默认仍为 8；10-bit 文件输入配合默认参数现在会通过 Metal 转成 8-bit。`--output-range limited|full` 默认保留源范围。`--output-matrix bt601|bt709|bt2020nc|smpte240m` 默认保留源矩阵，只改变 YCbCr 系数，不改变 primaries / transfer，也不是完整 BT.601/BT.709 色域转换。`--input-matrix` 明确覆盖源矩阵解释；当转换需要矩阵但解码缓冲区和格式描述未提供支持的值时必须指定，不按尺寸猜测。

支持 left、center、top-left 输入色度位置，Metal 输出统一重采样为 left。优先读取解码缓冲区 / 格式描述；缺失或不支持时需要 `--input-chroma-loc left|center|top-left` 明确解释。转换后的实际码流位深、range、颜色和色度标记会核查，不能仅修改标签冒充转换。

GPU 通过 `CVMetalTextureCache` 映射 IOSurface 兼容缓冲区；生产处理路径不锁定或复制 CPU 像素，无 CPU 滤镜回退。每帧等待 Metal 命令成功完成后提交硬编，保留原 PTS / duration，输出缓冲池最多 3 个。任务结果中的 `Metal GPU frames completed` 统计成功提交硬编的已完成 GPU 帧；无需滤镜且位深相同时为 0。

当前双线性为固定两点插值，不带缩小时的低通抗混叠，不适合作为强缩小的最高质量算法。量化采用舍入，无 dithering；不支持物理旋转、Bicubic/Lanczos、字幕合成和运动自适应反交错。这些仍属于后续工作。

2026-11-17 已在本机完成 240 组 GPU 逐像素参考比对、`video_samples` 的 11 个成功转码用例和 6 个拒绝用例，以及 P2 回归；详见 [P3 验证记录](docs/P3验证.md)。结果不代表其他硬件、系统或任意尺寸已验证，也不承诺零拷贝或长期吞吐量。

## Metal 色彩空间转换

参考 [NVEnc 的 --vpp-colorspace](https://github.com/rigaya/NVEnc/blob/5bb35d860bc5f877b01fe60e4de5ac756b9b80ed/NVEncC_Options.en.md#--vpp-colorspace-param1value1param2value2)，将核心矩阵、传递函数和色域算法移植为 Metal；许可证和来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。这不是 NVEnc 全部参数的兼容实现。

```bash
# SDR BT.709 转为 BT.2020 色域与矩阵；保留原传递函数
./build/dev/vtenc -i input.mp4 -o bt2020.mp4 --codec hevc --output-depth 10 \
  --vpp-colorspace matrix=auto:bt2020nc,colorprim=auto:bt2020

# PQ BT.2020 转为 BT.709，100 cd/m² 为线性光 1.0；超过输出范围的亮部会裁切
# 这是色度学转换，不是保留高光层次的 HDR tone mapping
./build/dev/vtenc -i hdr10.mp4 -o bt709.mp4 --codec hevc --output-depth 10 \
  --vpp-colorspace matrix=auto:bt709,transfer=auto:bt709,colorprim=auto:bt709,sdr_source_peak=100

# 显式解释源值，并按场景参考的传递曲线转换
./build/dev/vtenc -i input.mp4 -o converted.mp4 \
  --vpp-colorspace matrix=smpte170m:bt709,colorprim=smpte170m:bt709,transfer=bt709:srgb,range=limited:full,scene_ref=true
```

| 参数 | 支持值 |
| --- | --- |
| `matrix=源:目标` | `bt601`（`smpte170m` / `bt470bg`）、`bt709`、`smpte240m`、`bt2020nc`（`bt2020`） |
| `transfer=源:目标` | `bt709`（`smpte170m` / `bt2020-10` / `bt2020-12`）、`linear`、`srgb`（`iec61966-2-1`）、`smpte2084`（`pq`）、`arib-std-b67`（`hlg`） |
| `colorprim=源:目标` | `bt709`、`smpte170m`（`smpte240m`）、`bt470bg`、`bt2020`、`display-p3`（`smpte432`）、`dci-p3`（`smpte431`） |
| `range=源:目标` | `limited`、`full` |
| `scene_ref` | `false`（默认，显示参考）或 `true`（场景参考） |
| `sdr_source_peak` | 线性光 1.0 对应的亮度，默认 100 cd/m²，范围 `[0.01,10000]` |

四组源/目标参数均支持 `auto`：源从解码帧附件优先、轨道元数据次之读取；目标继承已解析的源值（`hdr2sdr` 的默认目标例外，见下节）。省略的参数等价于 `auto:auto`。显式源值覆盖解释，目标值用于实际像素变换和输出标签；所需元数据缺失或不支持时直接报错，不按尺寸猜测。与旧的 `--input-matrix` / `--output-matrix` / `--output-range` 互斥，避免顺序相关的覆盖。原始帧路径仍不接受本选项。

转换在浮点 Metal 内核中依次执行：范围归一化 → YCbCr 转 R′G′B′ → 逆传递函数 → 线性 RGB 色域矩阵（需要时做 Bradford 白点适应）→ 目标传递函数 → 目标 YCbCr → 量化。仅矩阵/范围转换和身份转换跳过不必要的传递函数往返。非线性转换后才做垂直两点色度降采样，保持 left 相位；支持与裁剪、缩放、Bob 组合。

与 NVEnc 一致，显示参考 BT.709 使用理想 BT.1886 gamma 2.4；`scene_ref=true` 切换为 BT.709 OETF。PQ 以 10000 cd/m² 归一化；HLG 显示参考采用 1000 cd/m²、gamma 1.2 的亮度相关 OOTF，场景参考采用逆/正 OETF 与 12 倍标度。HLG 在逆 OETF **之后**计算场景线性亮度并应用 OOTF，与参考版本对编码 RGB 先作亮度校正的顺序不同，保证正反变换互逆。PQ 输入负值及超过 1.0 的非法信号先限幅，避免有理式奇点；色域转换不做中间 RGB 限幅，传递函数按自身定义处理负值，最终输出超范围值裁切。

当前没有移植 LUT3D、`approx_gamma` LUT 近似、RGB 平面、YCgCo、BT.2020 constant-luminance 或其他未列出的曲线/基色；请求这些参数会报错。未启用 `hdr2sdr` 时，HDR 转 SDR 不会自动压缩高光；HDR 输出只写转换后的矩阵、传递函数、基色和范围，不复制失效的 mastering display / content light 元数据，也不生成新的 HDR10 静态元数据。Dolby Vision 始终拒绝。编码器必须确认输出色彩标记，否则任务失败。

### HDR→SDR tone mapping

```bash
# 默认目标 BT.709；默认源峰值 1000 cd/m²、SDR 白点 100 cd/m²
./build/dev/vtenc -i hdr10.mp4 -o sdr.mp4 --codec hevc --output-depth 10 \
  --vpp-colorspace hdr2sdr=hable

# PQ 或 HLG 都可接入；源色彩标记缺失时使用已有 FROM:TO 参数显式声明
./build/dev/vtenc -i hlg.mp4 -o sdr-hlg.mp4 --avsw --input-chroma-loc left \
  --vpp-colorspace hdr2sdr=bt2390,source_peak=1000,ldr_nits=100

# Mobius 保留 transition 以下线性响应；降低去饱和强度保留更多 RGB 比例
./build/dev/vtenc -i hdr10.mp4 -o sdr-mobius.mp4 \
  --vpp-colorspace hdr2sdr=mobius,transition=0.3,peak=1,desat_strength=0.5

./build/dev/vtenc -i hdr10.mp4 -o sdr-reinhard.mp4 \
  --vpp-colorspace hdr2sdr=reinhard,contrast=0.5,peak=1,source_peak=4000,ldr_nits=203
```

`hdr2sdr=none|hable|mobius|reinhard|bt2390` 默认 `none`。启用时只接受 PQ/HLG 输入，要求 `scene_ref=false`；源矩阵、基色和传递函数仍从元数据读取或显式覆盖，不猜测为 HDR10。输出矩阵、传递函数和基色的 `auto` 改为 **BT.709**，范围仍继承源；可显式选择其他已支持 SDR 色彩空间，但不能输出 PQ/HLG。输出位深仍由 `--output-depth` 决定，默认 8-bit。

处理顺序为逆传递函数 → **源色域的线性 RGB tone mapping 与高光去饱和** → 目标色域矩阵 → SDR 传递函数。与 NVEnc 一致，去饱和将保持 RGB 比例的亮度缩放结果与逐通道曲线结果混合；`desat_strength=0` 保持 RGB 比例。不做局部/逐帧自适应映射，也不读取 MaxCLL 自动猜测 `source_peak`。

| 参数 | 默认值 | 校验范围 / 适用曲线 |
| --- | --- | --- |
| `source_peak` / `ldr_nits` | 1000 / 100 cd/m² | `1 <= ldr_nits <= source_peak <= 10000` |
| `desat_base` / `desat_strength` / `desat_exp` | 0.18 / 0.75 / 1.5 | `[0,1]` / `[0,1]` / `[0.01,10]`，四种曲线通用 |
| `a,b,c,d,e,f` | 0.22,0.3,0.1,0.2,0.01,0.3 | 仅 Hable；`a,e∈[0,10]`、`b,d,f∈[0.0001,10]`、`e<f`、`e/f<=c<=1`，曲线不能恒为零 |
| `transition` | 0.3 | 仅 Mobius，`[0,0.999]` |
| `peak` | 1 | Mobius / Reinhard，`[0.001,100]`；Mobius 另要求 `peak*source_peak/ldr_nits >= 1` |
| `contrast` | 0.5 | 仅 Reinhard，`[0.001,0.999]` |

参数须有限，重复参数、为错误曲线设置参数、未启用映射却设置调节参数均报错。HDR2SDR 统一以 `ldr_nits` 作为线性 RGB 的亮度标度；若同时显式设置 `sdr_source_peak`，必须与其相等。这避免参考实现两个独立亮度标度不一致时，声明峰值与曲线白点错位。

四种曲线均按所选 NVEnc 版本移植：Hable 用声明峰值归一化有理曲线；Mobius 在 transition 处平滑连接；Reinhard 用 `(1-contrast)/contrast` 控制映射；BT.2390 在 PQ 域使用 Hermite 肩部曲线。**BT.2390 保留该 NVEnc 实现 PQ 辅助函数的 203 参考白倍率**，不宣称与其他软件的同名映射逐位一致。针对数值边界，Hable/Mobius 使用代数等价的稳定表达式，Mobius 有效峰值为 1 时为身份映射；BT.2390 将超声明源峰值的输入限制到峰值后再进肩部，避免多项式外推反转高光，源/目标峰值相等时为身份映射。最终量化仍会裁切目标色域外的值。

显式验证（不运行全量测试）：

```bash
ctest --preset dev -R '^vtenc.cli$'
ctest --preset dev -R '^vtenc.colorspace$'
./build/dev/vtenc_gpu_filter_test
python3 tests/colorspace_hardware.py
python3 tests/colorspace_hardware.py --tone-mapping
```

2026-11-17 本机通过 980 个 GPU 像素用例；基础色彩转换验证了 6 个真实码流，tone mapping 追加验证了 9 个映射输出和 1 个裁切对照，见 [色彩空间验证记录](docs/色彩空间验证.md)。

## P3 Bob 反交错

`--deinterlace bob` 对每个输入帧对的两个场分别进行垂直线性重建，按时间先后输出完整尺寸的逐行帧。Y 与 UV 都只从所选场取样；色度位置按各场坐标解释，再转换为逐行 left 色度位置。Bob 可与文件模式的裁剪、缩放和色彩转换组合；源高度和上下裁剪边距必须为 4 的倍数，以保留完整的 4:2:0 场布局。

`--field-order auto|tff|bff` 默认 auto，要求解码缓冲区或格式描述有无歧义的场序；缺失时直接报错。tff/bff 是显式源解释覆盖，也可处理实际包含交错栅格但被标记为逐行编码的文件。它不是场序检测器，选错会颠倒运动时序。不启用 Bob 或 IVTC 时继续拒绝已识别的交错输入。

文件模式精确拆分每帧的显示区间：第一场 PTS 为原 PTS，第二场为原 PTS 加半个 duration，两个输出 duration 相等。保留 VFR 间隔和 A/V 起点，封装视频时间基加倍，不能精确表示时失败。原音轨仍逐包复制。每场 GPU 完成后才提交硬编，保持有界资源与取消清理。

```bash
# 默认先硬解，失败后从头软解重试一次；编码仍为硬件
./build/dev/vtenc -i interlaced.mp4 -o build/bob.mp4 \
  --deinterlace bob --field-order tff

# 已有原始交错 NV12：180 个场对 → 360 个逐行帧，30000/1001 → 60000/1001
./build/dev/vtenc -i interlaced.nv12 -o build/bob.h264 \
  --input-res 720x480 --frames 180 --fps 30000/1001 \
  --deinterlace bob --field-order tff

# 显式硬解 / 原始输入验证；包含离线测试夹具准备，文件输入指定 --avhw
python3 tests/bob_hardware.py --vtenc build/dev/vtenc
```

原始 Bob 模式必须给出 tff/bff，不接受合成输入。Y 平面与交错 UV 平面都按行交织上下场，使用每场 left 色度位置、BT.709 SDR limited range；P010 仍为高 10 位存储。`--fps` / `--frames` 描述输入场对，输出数量和帧率加倍；`--gop-len` 仍计输出帧。原始模式只增加 Bob，不开放其他 GPU 滤镜参数，也不改变 P1 输入/输出同位深的约定。Annex B 时间轴以配套 CSV 为准，外部封装需使用倍频后的帧率。

**本机限制：**`video_samples/deinterlace` 的普通交错和 3:2 hard telecine 原始 MP4，强制硬解均在创建会话时返回 `OSStatus=-12911`。因此这两个原始文件的完整硬解转码未通过。测试明确将其记为硬解受阻；另将样片离线导出为原始帧，实际验证 Metal Bob 与 VT 硬编，并用明确标注的逐行编码交错栅格载体 / VFR 文件验证硬解、调度和音轨复制。这是原始硬解路径的限制；新增 `--avsw` 与默认回退可直接软解这两个原始样片，再通过 Metal Bob 与 VT 硬编完成转码。原有硬件验证脚本现显式使用 `--avhw`，软解及自动重试由 `decode_modes_hardware.py` 单独验证。

Bob 保留 hard telecine 的重复场，输出 60000/1001p，不还原为 24000/1001p；Bob 本身不做场匹配或去重；还原电影帧请使用下面的 `--vpp-ivtc`。单场重建会损失垂直细节，细线可能闪烁。真实结果和复现边界见 [Bob 验证记录](docs/P3Bob验证.md)。

## Metal IVTC：soft / hard telecine 还原

`--vpp-ivtc` 在 Metal 上展开 soft telecine 的 RFF 重复场、匹配相邻帧的上下场并去重，将典型的 30000/1001 hard telecine 还原为 24000/1001 逐行输出。软硬 telecine 与普通逐行视频混合时使用 `mixed=true`，按重复证据决定是否降帧，输出可以是 VFR。普通 60i 的完整场运动恢复仍使用 Bob；IVTC 的混合模式仅在源帧率上修复真正交错片段。

```bash
./build/dev/vtenc -i telecine.mp4 -o build/film.mp4 \
  --vpp-ivtc --field-order tff

# 软/硬 telecine 与逐行视频混合，保留源时间轴及音轨
./build/dev/vtenc -i mixed.mp4 -o build/mixed-film.mp4 \
  --vpp-ivtc expand=auto,mixed=true

# 显式软解，硬件编码；同时缩放、输出 HEVC 10-bit
./build/dev/vtenc -i telecine.mp4 -o build/film10.mp4 --avsw \
  --vpp-ivtc post=2,nt=10,cthresh=4,combpel=8,dthresh=7 \
  --field-order tff --output-res 1280x720 --codec hevc --output-depth 10

# 仅场匹配、不丢帧；逐帧诊断写 stderr
./build/dev/vtenc -i telecine.mp4 -o build/matched.mp4 \
  --vpp-ivtc cycle=0,post=0,log=true --field-order tff
```

参数可省略；重复、未知参数和越界数值直接报错。`--field-order auto|tff|bff` 与 Bob 共用：hard telecine 的逐帧场标记可能变化，此时使用已知的 tff/bff 覆盖；RFF 展开始终优先使用原始逐帧首场标记，不能用固定覆盖值破坏 3:2 交替场序。混合模式允许在边界排空后切换场序。IVTC 与 Bob 互斥，仅支持 libavformat 压缩输入。

| 参数 | 默认值 | 范围及含义 |
| --- | --- | --- |
| `cycle` | 5 | 5：每五个显示帧最多丢一帧；0：只匹配和修复，不去重 |
| `expand` | auto | auto/on：按逐帧 RFF 标记展开；off：关闭展开。on 不凭空生成重复场 |
| `mixed` | false | true：保留干净逐行帧，只在干净匹配且有明显重复的周期去重；false：显式 3:2，每完整周期固定去一帧 |
| `post` | 2 | 2：修复残留梳齿；0：关闭修复 |
| `nt` | 10 | 0–255，匹配评分的噪声门限 |
| `cthresh` | 4 | 0–255，评分的单像素梳齿门限 |
| `combpel` | 8 | 1–128，每个 32×8 块判为梳齿块所需的像素数 |
| `dthresh` | 7 | 0–255，修复的像素残差门限；0 关闭此门控 |
| `log` | false | true/false，输出帧选择、评分、SAD 与周期丢帧位置到 stderr |

`nt`/`cthresh` 按源位深换算后最小为 1 码值（与参考实现一致）；`dthresh=0` 才表示关闭像素门控。

移植参考 [NVEnc IVTC](https://github.com/rigaya/NVEnc/blob/5bb35d860bc5f877b01fe60e4de5ac756b9b80ed/NVEncCore/NVEncFilterIvtc.cu)，MIT 声明保留于 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。处理如下：

- 按当前显示帧场序，对 C（当前帧）、P（当前首场＋前帧次场）、N（后帧首场＋当前次场）计算亮度的五行插值误差与梳齿计数。32×8 分块后按梳齿块数量、最大块计数、裁剪均值、匹配误差排序；指标由 GPU 计算，CPU 仅汇总块统计和调度。
- 保留上游 N→P 重复场对保护，并增加干净 P→N 候选间的连续性约束，避免压缩噪声导致漏掉一个电影时刻。首帧不借未来首场取代片段开头；相邻原始帧亮度 SAD 超过最大可能值的 20% 时禁止跨该边界借场。
- Y 和交织 UV 在 Metal 上按同一场选择重组；残余梳齿的最大块计数达到 65 时，`post=2` 使用上游三帧时空自适应插值与像素门控。支持 NV12/P010 的 limited/full range，源高度至少 8 且为 4 的倍数。10-bit 按码值缩放门限并保持 P010 高位布局。
- 对合成帧计算亮度 SAD，选择与前一合成帧最相似的重复帧，包含跨周期边界比较。初始帧没有前帧，不参与首次最小差值丢帧。混合模式要求无残余梳齿、存在展开/交错证据，最小 SAD 不超过最大可能 SAD 的 0.5%；未展开的周期还要求最小差值不超过周期最大差值的 1/5。证据不足保留全部帧。
- 参考上游 pending-field 调度，将每个编码图片按 `2 + repeat_pict` 个显示场拆分，再用 Metal 重组相邻异奇偶场。支持额外 0/1/2/4 场；保留跨图片的待配对场，使用有界缓存，无需全片预扫描。源场序不连续或 EOF 剩余单场时输出诊断，以该场的源图片承载剩余时间区间，并标记边界，禁止跨边界匹配/去重。

IVTC 在裁剪、缩放和色彩转换之前执行。决定去重的完整周期按原始首尾时间均分为四个输出，整数有理数计算，保留非零 PTS 起点与周期总时长；纯 hard telecine CFR 输入得到 4/5 帧率，soft telecine 先展开再还原，混合/VFR 保留每周期时长。尾部不足五帧全部输出并保留原区间，避免拉长片尾。RFF 将每个编码图片的原始时间区间按场数精确拆分，既支持 3/2 场变时长标记也支持固定编码图片时长。启用展开时封装时间基预留 12 倍精度，`cycle=5` 再扩大四倍，超出 CoreMedia 32-bit 时间基范围时明确失败。音轨继续按原时间轴逐包复制。`-o -` 的码流与 stderr 诊断分离；启动 stdout 封装后仍禁止从头重试。

逐帧元数据来自 libavcodec 解码帧、原生 H.264 的 FFmpeg 元数据解析器，或独立的 HEVC SPS/PPS/VUI 与 picture-timing SEI 解析器；硬解路径不启用隐藏的软件解码。HEVC 按当前图片的 slice→PPS→SPS 关系读取 `pic_struct`、`source_scan_type` 和 `duplicate_flag`，支持 MP4 hvcC/长度前缀及 Annex B 参数集。软解通过 packet/frame opaque 数据传递标记，硬解随图片排序，B 帧重排后仍按显示顺序处理。支持完整帧 `pic_struct=0/3/4/5/6/7/8`；`duplicate_flag` 不当作额外重复场。SPS 要求的 picture-timing 缺失、标记截断或保留值均明确报错，不触发软解重试；多层 HEVC 和独立单场编码暂不支持。`expand=off,mixed=true` 可保留 soft telecine 的原编码图片区间；已知纯 hard telecine 可用 `expand=off` 跳过原生 RFF 解析。

这不是完整 NVEnc 兼容层：未移植全片预扫描、cadence lock、双场序置信度分类或五帧 BWDIF，未强制上游式全片 CFR。混合分类是保守的局部像素/元数据判断，不能保证所有噪声、字幕、静止段和剪辑断点都恢复严格电影 cadence；证据不足或不足五帧时允许保留重复。`mixed=false,cycle=5` 仍明确假设 3:2；`cycle=0` 保留展开后的显示帧数量。压缩破坏和真正交错片段中的不可恢复场仍可能需要插值。

显式运行相关验证：

```bash
ctest --preset dev -R '^vtenc.cli$'
ctest --preset dev -R '^vtenc.timeline$'
./build/dev/vtenc_ivtc_test
python3 tests/ivtc_hardware.py --vtenc build/dev/vtenc
python3 tests/rff_hardware.py --vtenc build/dev/vtenc
ctest --preset dev -R '^vtenc.hevc-cadence$'
python3 tests/hevc_rff_hardware.py --vtenc build/dev/vtenc
```

2026-11-17 本机验证覆盖 IVTC/RFF 数值与调度、原有 9 个 hard-telecine 输出、12 个 H.264 soft/mixed 输出，以及 HEVC 元数据解析、Main/Main10 混合码流和 B 帧重排；原始 telecine 的 180→144 帧恢复平均 PSNR 为 43.34 dB。详见 [Metal IVTC 验证记录](docs/MetalIVTC验证.md)。

## 代码布局

视频测试素材见 [tests/video_samples/README.md](tests/video_samples/README.md)：包含 H.264 / HEVC / AV1 的 8/10-bit 输入、HLG / HDR10 / Dolby Vision、普通交错 / Telecine，以及带音轨的 VFR。下载、生成和校验按需运行，不加入默认 CTest。

```text
include/vtenc/        纯 C++ 公共接口
src/cli/             CLI 入口和输出
src/core/            参数解析与业务规则
src/apple/           VideoToolbox C++ 适配、CF RAII、Metal Objective-C++ 桥接
tests/               按范围选择的验证
scripts/             环境检查与开发辅助
docs/                开发方案
```

VideoToolbox、CoreMedia、CoreVideo、FFmpeg 提供 C API，直接使用 `.cpp`；当前仅 Metal 的 Objective-C API 桥接使用 `.mm`，不将 Objective-C 类型传播到公共接口。
