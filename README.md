# VTEnc

面向 Apple Silicon M 系列的原生 VideoToolbox 硬件编码器，借鉴 NVEncC 的 CLI、能力探测和模块划分，使用 C++20 + 少量 Objective-C++。

本仓库独立于 NegativeEncoder；未来通过命令行协议集成。功能范围以截至 M6 的硬件支持为目标，硬编能力以当前系统的真实探测为准。编码始终使用硬件；解码支持显式硬解、FFmpeg 软解，以及硬解失败后一次软解重试。

## 当前状态

已实现 P1 硬编闭环、P2 原生 MP4/MOV 文件转码，以及 P3 GPU 缩放、裁剪、基础色彩转换与 Bob 反交错。现有功能：

- CMake / Ninja 的 Debug、Release 构建与安装目标。
- 纯 C++ CLI、参数校验，以及独立的 Apple 框架适配库。
- H.264 8-bit、HEVC 8/10-bit 的硬件会话探测与支持属性枚举。
- Objective-C++ / ARC 的 Metal 设备查询。
- 合成测试帧 / 原始 NV12、P010 输入，H.264 High 8-bit、HEVC Main 8-bit / Main10 硬件编码。
- 平均码率目标或原生 Quality、GOP 上限、整数有理数帧率；Annex B 输出及逐帧时间戳 CSV。
- MP4/MOV 输入：原生 VideoToolbox 硬解或 FFmpeg `libavformat` / `libavcodec` 软解，原生硬编和封装，默认复制第一条音轨。
- 文件转码保留源 PTS、VFR、A/V 起点偏移、像素宽高比和显示变换；支持 `hev1` 描述适配。
- Metal 裁剪 / 双线性缩放、NV12/P010 8/10-bit 与 limited/full range 转换、BT.601/BT.709 YCbCr 矩阵转换。
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

输入路径扩展名 `.mp4` / `.mov` 自动选择文件转码，输出也必须是这两种容器。文件模式读取源尺寸、时间戳和全部帧，拒绝 `--input-res`、`--fps`、`--frames`；这些参数仍属于 P1 原始帧模式。编码参数继续使用 `--codec`、`--output-depth`、`--bitrate` / `--quality` 和 `--gop-len`。

解码模式（仅用于 MP4/MOV 压缩输入，两个参数互斥）：

- `--avhw`：强制原生 VideoToolbox 硬解，创建会话和处理帧时的硬解错误直接退出，不回退。
- `--avsw`：直接通过 FFmpeg `libavformat` 解封装、`libavcodec` 软解，不创建硬解会话。
- 两者省略：先尝试硬解。仅硬解错误触发一次完整重试：报告原始诊断，销毁解码、编码与封装状态，清理临时输出，重新打开输入并从头软解。软解再失败则退出；编码、滤镜、输入时间轴、音轨和写文件错误不重试。

硬解路径使用 AVFoundation `AVSampleCursor` / `AVSampleBufferGenerator` 读取压缩视频，显式调用并验证 VideoToolbox；根据 SPS / hvcC / av1C 请求同位深 NV12/P010，B 帧显示重排队列上限 32。软解路径链接 FFmpeg 底层库，不执行 `ffmpeg` 子进程；只选择软件解码器和 CPU 像素格式，处理 send/receive、B 帧与 EOF 排空，将 8/10-bit 4:2:0 帧无损重排为 Metal 兼容 NV12/P010。其他软解像素布局直接报错，不隐式转换色度或位深。AVFoundation 仍用于 MP4/MOV 轨道元数据、编辑列表校验、压缩音轨复制和输出封装，因此没有扩展到 MKV/TS 等输入容器。

两条路径都保留整数有理数 PTS、VFR 和源颜色信息；软解检测损坏帧、尺寸或格式变化并报错。H.264 / HEVC 编码始终沿用原生 VideoToolbox 硬件编码器，强制并检查实际硬件后端。成功日志分别报告软解或硬解，不把软解标为“硬解已验证”。

```bash
./build/dev/vtenc --avhw -i input.mp4 -o build/hardware.mp4
./build/dev/vtenc --avsw -i input.mp4 -o build/software.mp4
./build/dev/vtenc -i input.mp4 -o build/automatic.mp4

# 策略测试不创建硬件会话；集成验证必须显式运行
ctest --preset dev -R '^vtenc.decode-policy$'
python3 tests/decode_modes_hardware.py --vtenc build/dev/vtenc
```

2026-11-17 已在本机用 FFmpeg 9.0.2 完成 13 个成功转码用例的独立码流 / PTS 验证，覆盖 H.264、HEVC、AV1 的 8/10-bit 软解、VFR 与 AAC 保真、旋转、默认硬解和强制硬解，以及两种交错样片的软解 / 自动回退 Bob。另验证了互斥参数、只重试一次、迟发损坏包、SIGINT、输出防覆盖和 HDR 拒绝；未执行全量测试。Telecine 样片的逐帧场标记存在变化，Bob 验证显式使用 `--field-order tff`。

FFmpeg 来自外部安装，本仓库没有复制其上游实现。FFmpeg 的基础许可证为 LGPL 2.1-or-later，启用 GPL/version3 等配置会改变该构建的许可证；本机 Homebrew 库启用了 GPL 与 version3。分发时须按实际链接库的许可证提供对应源码、配置及声明，不能将任意 Homebrew 构建视为 LGPL-only。见 [FFmpeg 许可证说明](https://ffmpeg.org/legal.html)。VTEnc 不调用 FFmpeg 的视频编码 API。

音频由 `AVAssetReaderTrackOutput(outputSettings:nil)` 读取压缩数据，`AVAssetWriterInput(outputSettings:nil)` 封装复制，不进行音频重编码。默认选择第一条音轨；`--no-audio` 显式禁用。保留原生 AAC priming、尾部裁剪和编辑映射，音视频共用电影时间原点，不分别归零。轨道配置变化、不兼容容器、重复音频 PTS、无法精确表示的时间基等问题直接失败。

当前限定一个 SDR 视频轨；交错输入必须显式启用下述 Bob；只有 `--avhw` 要求硬解成功。P3 已加入位深、尺寸和基础色彩转换。不支持 HDR / Dolby Vision、音频转码、多音轨选择、字幕、章节与附件。显示旋转仅保留容器变换，不修改像素。支持单段正常速率的视频编辑映射；不实现时间裁剪（Trim）、变速或复杂编辑列表。对旧文件因声明时长舍入而落在末尾边界外的完整视频帧予以保留，不以粗粒度声明时长截断码流；显著裁剪存储视频帧的编辑映射会报错。

取消或运行错误不发布成品，临时输出目录自动清理；成功结束封装后以不覆盖既有文件的方式发布。写出背压和封装完成等待均检查取消与错误，并对 30 秒无进展报错。强制杀进程或系统崩溃可能残留 `.part.*` 临时目录。

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

这些滤镜仅用于 MP4/MOV 文件模式。处理顺序是裁剪、双线性采样、YCbCr 矩阵 / range / 位深转换。`--crop LEFT,TOP,RIGHT,BOTTOM` 是四个非负偶数边距，必须留有正偶数宽高；坐标基于未应用容器旋转的存储像素。省略 `--output-res` 时输出裁剪后的尺寸，显式指定时精确拉伸到目标偶数宽高，保留源像素宽高比，不自动补黑边。几何变换后 clean aperture 重置为完整输出栅格；容器显示方向保持，平移按新尺寸调整。

`--output-depth` 决定文件输出位深，默认仍为 8；10-bit 文件输入配合默认参数现在会通过 Metal 转成 8-bit。`--output-range limited|full` 默认保留源范围。`--output-matrix bt601|bt709` 默认保留源矩阵，只改变 YCbCr 系数，不改变 primaries / transfer，也不是完整 BT.601/BT.709 色域转换。`--input-matrix` 明确覆盖源矩阵解释；当转换需要矩阵但解码缓冲区和格式描述未提供支持的值时必须指定，不按尺寸猜测。

支持 left、center、top-left 输入色度位置，Metal 输出统一重采样为 left。优先读取解码缓冲区 / 格式描述；缺失或不支持时需要 `--input-chroma-loc left|center|top-left` 明确解释。转换后的实际码流位深、range、颜色和色度标记会核查，不能仅修改标签冒充转换。

GPU 通过 `CVMetalTextureCache` 映射 IOSurface 兼容缓冲区；生产处理路径不锁定或复制 CPU 像素，无 CPU 滤镜回退。每帧等待 Metal 命令成功完成后提交硬编，保留原 PTS / duration，输出缓冲池最多 3 个。stdout 的 `Metal GPU frames completed` 统计成功提交硬编的已完成 GPU 帧；无需滤镜且位深相同时为 0。

当前双线性为固定两点插值，不带缩小时的低通抗混叠，不适合作为强缩小的最高质量算法。量化采用舍入，无 dithering；不支持 HDR、tone mapping、色域 / 传递函数转换、物理旋转、Bicubic/Lanczos、字幕合成和运动自适应反交错。这些仍属于后续工作。

2026-11-17 已在本机完成 240 组 GPU 逐像素参考比对、`video_samples` 的 11 个成功转码用例和 6 个拒绝用例，以及 P2 回归；详见 [P3 验证记录](docs/P3验证.md)。结果不代表其他硬件、系统或任意尺寸已验证，也不承诺零拷贝或长期吞吐量。

## P3 Bob 反交错

`--deinterlace bob` 对每个输入帧对的两个场分别进行垂直线性重建，按时间先后输出完整尺寸的逐行帧。Y 与 UV 都只从所选场取样；色度位置按各场坐标解释，再转换为逐行 left 色度位置。Bob 可与文件模式的裁剪、缩放和色彩转换组合；源高度和上下裁剪边距必须为 4 的倍数，以保留完整的 4:2:0 场布局。

`--field-order auto|tff|bff` 默认 auto，要求解码缓冲区或格式描述有无歧义的场序；缺失时直接报错。tff/bff 是显式源解释覆盖，也可处理实际包含交错栅格但被标记为逐行编码的文件。它不是场序检测器，选错会颠倒运动时序。不启用 Bob 时继续拒绝已识别的交错输入。

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

Bob 保留 hard telecine 的重复场，输出 60000/1001p，不还原为 24000/1001p；不实现 IVTC、soft telecine 的重复场展开、场匹配、运动自适应或自动混合模式检测。单场重建会损失垂直细节，细线可能闪烁。真实结果和复现边界见 [Bob 验证记录](docs/P3Bob验证.md)。

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
