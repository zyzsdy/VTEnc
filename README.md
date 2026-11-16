# VTEnc

面向 Apple Silicon M 系列的原生 VideoToolbox 硬件编码器，借鉴 NVEncC 的 CLI、能力探测和模块划分，使用 C++20 + 少量 Objective-C++。

本仓库独立于 NegativeEncoder；未来通过命令行协议集成。功能范围以截至 M6 的硬件支持为目标，每项能力以当前系统的真实探测为准，禁止软件视频编解码回退。

## 当前状态

已实现 P1 硬编闭环和 P2 原生 MP4/MOV 文件转码。现有功能：

- CMake / Ninja 的 Debug、Release 构建与安装目标。
- 纯 C++ CLI、参数校验，以及独立的 Apple 框架适配库。
- H.264 8-bit、HEVC 8/10-bit 的硬件会话探测与支持属性枚举。
- Objective-C++ / ARC 的 Metal 设备查询。
- 合成测试帧 / 原始 NV12、P010 输入，H.264 High 8-bit、HEVC Main 8-bit / Main10 硬件编码。
- 平均码率目标或原生 Quality、GOP 上限、整数有理数帧率；Annex B 输出及逐帧时间戳 CSV。
- MP4/MOV 压缩样本读取、显式 VideoToolbox 硬解、硬编和原生封装，默认复制第一条音轨。
- 文件转码保留源 PTS、VFR、A/V 起点偏移、像素宽高比和显示变换；支持 `hev1` 描述适配。
- 有界回调和重排队列、背压和排空；SIGINT 取消、失败清理、输出防覆盖。
- 不依赖硬件编码器的 CLI / 码流适配检查，以及显式硬件集成验证脚本。

完整开发方案见 [docs/开发方案.md](docs/开发方案.md)。

## 环境准备

需要原生 arm64 macOS、支持 C++20 的 Apple Clang、macOS SDK、CMake 3.25+ 和 Ninja。最低部署版本暂定 macOS 13；这不代表所有目标功能在 macOS 13 上均可用，新 API 必须检查系统版本与会话能力。

```bash
xcode-select --install  # 尚未安装 Xcode Command Line Tools 时执行
brew install cmake ninja
cd /Users/tedzy/Workspace/VTEnc
./scripts/bootstrap.sh
```

`bootstrap.sh` 检查平台与工具并构建 Debug，不修改系统配置、不安装依赖、不自动运行测试。SDK 由 `xcode-select` / `xcrun` 选择，不在仓库中硬编码 SDK 路径。当前无需完整 Xcode；后续离线编译 Metal Shader 时再明确所需工具链。

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

退出码：`0` 成功、`1` 运行时或硬件错误、`2` 参数错误、`130` 用户取消。结果写入 stdout，诊断写入 stderr。当前只提供文本输出，JSON 协议留待后续实现。

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

本阶段限定逐行、video range、BT.709 SDR；调用方负责提供符合约定的原始数据。10-bit 不等于 HDR；本阶段不读取或转换输入色彩元数据。帧率是整数或整数分数，PTS 从 0 开始，以 `帧号 × fps_den / fps_num` 构造，不做浮点累积。默认禁用帧重排，未实现 B 帧、VFR、滤镜、音频或容器封装。

默认平均码率目标为 5000 kbit/s。`--bitrate` 绑定 `AverageBitRate`，是软目标，不保证输出大小；`--quality` 绑定原生 `Quality`，不等价于 CRF/CQP，两者不能同时指定。`--gop-len` 是最大关键帧间隔，编码器可以提前插入关键帧。所有属性设置检查 OSStatus，不支持就失败，不做软件回退。

输出为 Annex B 裸码流，不因文件扩展名变成 MP4/MOV。同步帧前写入 SPS/PPS（HEVC 还包括 VPS），将长度前缀 NAL 转为起始码。裸码流不携带容器级 PTS/DTS，播放器或 FFprobe 猜测的帧率可能不正确；封装时需指定本次输入帧率。`<输出路径>.timestamps.csv` 保存编码回调原始整数 PTS/DTS/duration、同步帧标记及该帧的字节范围；VideoToolbox 未指定的 DTS 保持为空，范围包括该帧前的参数集。

同目录临时文件在全部排空、帧数与时间戳校验、硬件后端复核后发布；已有输出或时间戳文件均不覆盖。每批最多 8 帧，回调只保留样本或记录错误，调度线程负责码流写出。SIGINT 停止新提交、排空已提交帧、删除临时输出并返回 130。运行错误返回 1 并清理临时输出；进程被强制杀死或系统崩溃时可能残留 `.part.*` 文件。两个文件分别原子发布，不承诺跨文件的崩溃事务。

2026-11-16 在 Apple M6 / macOS 27.2.1 上完成三种格式的真实硬编及独立码流验证，覆盖合成输入、非对齐宽度原始输入、尾批排空及异常路径。详见 [P1 验证记录](docs/P1验证.md)。该结果不代表其他硬件、旧 macOS、任意分辨率或 HDR 已验证。

## P2 MP4/MOV 文件转码

```bash
# H.264 输出，默认复制第一条音轨（也可以显式写 --audio-copy）
./build/dev/vtenc -i input.mp4 -o build/output.mov --codec h264 --bitrate 5000

# 10-bit 输入保持位深，输出 HEVC Main10；不做 10 → 8-bit 转换
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

输入路径扩展名 `.mp4` / `.mov` 自动选择文件转码，输出也必须是这两种容器。文件模式读取源尺寸、时间戳和全部帧，拒绝 `--input-res`、`--fps`、`--frames`；这些参数仍属于 P1 原始帧模式。编码参数继续使用 `--codec`、`--output-depth`、`--bitrate` / `--quality` 和 `--gop-len`。

视频使用 AVFoundation `AVSampleCursor` / `AVSampleBufferGenerator` 取得压缩样本，再显式提交 VideoToolbox。根据公开 SPS / hvcC / av1C 读取源位深和色度，请求同位深 NV12/P010，不使用 AVFoundation 未压缩视频输出或软件视频回退。H.264、HEVC、AV1 解码是否可用由实际硬件会话决定，H.264 / HEVC 输出沿用 P1 硬件编码器。输入 B 帧按 DTS 边界和 PTS 排序恢复显示顺序，队列上限 32 帧；输出不启用帧重排，保留原始显示 PTS 和 VFR 间隔。

音频由 `AVAssetReaderTrackOutput(outputSettings:nil)` 读取压缩数据，`AVAssetWriterInput(outputSettings:nil)` 封装复制，不进行音频重编码。默认选择第一条音轨；`--no-audio` 显式禁用。保留原生 AAC priming、尾部裁剪和编辑映射，音视频共用电影时间原点，不分别归零。轨道配置变化、不兼容容器、重复音频 PTS、无法精确表示的时间基等问题直接失败。

当前限定一个逐行 SDR 视频轨；不支持 HDR / Dolby Vision、交错、位深或尺寸转换、音频转码、多音轨选择、字幕、章节与附件。显示旋转仅保留容器变换，不修改像素。支持单段正常速率的视频编辑映射；不实现用户裁剪、变速或复杂编辑列表。对旧文件因声明时长舍入而落在末尾边界外的完整视频帧予以保留，不以粗粒度声明时长截断码流；显著裁剪存储视频帧的编辑映射会报错。

取消或运行错误不发布成品，临时输出目录自动清理；成功结束封装后以不覆盖既有文件的方式发布。写出背压和封装完成等待均检查取消与错误，并对 30 秒无进展报错。强制杀进程或系统崩溃可能残留 `.part.*` 临时目录。

已使用 `video_samples` 的全部 13 个原始样片实施正向转码或明确拒绝检查，另以无重编码派生 MOV 验证输入容器和旋转。9 个成功转码用例的码流和时间轴均通过独立检查，VFR 音轨逐包哈希、PTS 和解码 PCM 完全一致。详见 [P2 验证记录](docs/P2验证.md)。

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

VideoToolbox、CoreMedia、CoreVideo 提供 C API，直接使用 `.cpp`；Metal、AVFoundation 等 Objective-C API 的桥接才使用 `.mm`，不将 Objective-C 类型传播到公共接口。
