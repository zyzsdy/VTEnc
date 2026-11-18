# VTEnc

VTEnc 是面向 Apple Silicon Mac 的命令行视频编码工具，使用 VideoToolbox 进行硬件编码，支持 H.264、HEVC，以及裁剪、缩放、反交错、色彩转换和字幕烧录等视频处理。适合日常转码、批量处理和脚本集成。

视频编码始终使用硬件。具体编码格式、位深和控制选项是否可用，取决于运行时的 macOS、硬件和任务配置。

项目受到 [NVEnc](https://github.com/rigaya/NVEnc) 的启发，并使用了其中部分代码。相关版权及许可证见 [第三方声明](THIRD_PARTY_NOTICES.md)。

## 安装

### 使用发行包

从 [Releases](https://github.com/zyzsdy/VTEnc/releases) 下载 `vtenc-<版本>-macos-arm64.zip` 并解压。发行包适用于原生 Apple Silicon macOS 环境，运行系统要求及签名信息见包内说明。

```bash
# 将版本号替换为下载文件对应的版本
vtenc_version='替换为实际版本号'
unzip "vtenc-${vtenc_version}-macos-arm64.zip"
"./vtenc-${vtenc_version}-macos-arm64/bin/vtenc" --version

# 将程序加入当前终端的 PATH
export PATH="$PWD/vtenc-${vtenc_version}-macos-arm64/bin:$PATH"
vtenc --help
```

保持解压后的 `bin/` 和 `lib/` 相对位置不变，不要只复制 `vtenc` 可执行文件。发行包包含所需的第三方动态库，运行时无需另外安装 Homebrew、FFmpeg 命令行程序或 Python。若要在新终端中直接使用 `vtenc`，将安装目录的绝对 `bin/` 路径加入 shell 配置中的 `PATH`。

可用以下命令计算下载文件的 SHA-256，并与同版本的 `SHA256SUMS.txt` 中对应条目核对：

```bash
shasum -a 256 "vtenc-${vtenc_version}-macos-arm64.zip"
```

未经 Apple 公证的发行包可能受到 Gatekeeper 限制；请查看包内的签名和公证说明。

### 从源码安装

先按下文建立开发环境，再从仓库根目录执行：

```bash
cmake --preset release
cmake --build --preset release --parallel
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
vtenc --version
```

这种安装方式依赖本机的 FFmpeg、libass、FreeType 等动态库，需要保留对应依赖。用于分发到其他 Mac 时，请使用本文的发行打包流程。

## 使用方法

### 查看帮助与设备信息

```bash
vtenc --help
vtenc --help vpp
vtenc --help vpp-yadif
vtenc --help bitrate
vtenc --version
vtenc --check-device
vtenc --check-features --codec hevc --output-depth 10
```

`--help` 首先打印版本和编译信息，然后展示基本用法、示例及各参数的独立说明。`--help 关键词`（也可写作 `-h 关键词`）按参数名做区分大小写的子串匹配，并打印所有匹配参数的完整段落。关键词无需带前面的 `--`。

`--check-features` 检查指定配置的硬件会话及公开控制选项，不编码帧；查询成功不代表所有分辨率、码率组合或素材都可用。

### 基本转码

命令框架为 `vtenc -i 输入文件 -o 输出文件 [选项]`。默认使用 H.264 8-bit、5000 kbit/s 平均码率，并复制第一条音轨（如果存在）。

```bash
# H.264 输出
vtenc -i input.mkv -o output.mp4 --bitrate 5000

# HEVC 10-bit 输出；10-bit 本身不代表 HDR
vtenc -i input.mkv -o output-hevc.mp4 \
  --codec hevc --output-depth 10 --bitrate 5000

# 显式使用软件解码，视频编码仍使用硬件
vtenc --avsw -i input.mkv -o output-software-decode.mp4

# 转换第一条音轨为 AAC
vtenc -i input.mkv -o output-aac.mp4 \
  --audio-codec aac --audio-bitrate 192
```

输入格式自动探测，不按文件扩展名筛选。输出封装格式由目标文件名选择，也可用 `-f` / `--output-format` 显式指定；无法探测或流与封装器不兼容时会报错。已有输出文件不会被覆盖。

默认先尝试硬件解码，遇到解码失败时输出诊断，并在输入可重新读取的情况下从头软解重试一次。`--avhw` 强制硬解，失败即退出；`--avsw` 直接使用软解。编码、滤镜和输出错误不会触发软解重试。

### 独立音频输出

`--audio-only` 从视频或音频文件中读取第一条音轨，使用 AudioToolbox 编码为 AAC-LC。输出从第一个有效音频样本开始计时。默认封装为 MP4，与输出文件扩展名无关。

```bash
vtenc --audio-only -i movie.mkv -o soundtrack.m4a
vtenc --audio-only -i recording.wav -o recording.mp4 --audio-bitrate 128 --audio-samplerate 48000
vtenc --audio-only -i recording.flac -f adts -o recording.aac
```

默认码率为 192 kbit/s，保留源采样率。支持单声道和立体声，输出采样率为 44100 或 48000 Hz，其他源采样率可用 `--audio-samplerate` 显式转换。MP4 文件会校验 AAC 首尾的有效样本范围。使用 `-f` 时，首尾补样信息取决于所选封装器，例如 ADTS 会保留补样。写入标准输出可用 `-f adts -o -`。

### 常用视频处理

```bash
# 裁剪四边各 8 像素，并缩放到 1280×720
vtenc -i input.mp4 -o resized.mp4 \
  --crop 8,8,8,8 --output-res 1280x720

# YADIF 场倍反交错
vtenc -i interlaced.mp4 -o progressive.mp4 --vpp-yadif mode=bob

# 还原 3:2 telecine 素材，明确指定上场优先
vtenc -i telecine.mp4 -o film.mp4 --vpp-ivtc --field-order tff

# 烧录外部 ASS 字幕
vtenc -i input.mp4 -o subtitled.mp4 \
  --vpp-subburn filename=subtitles.ass

# 转为 30000/1001 fps 的固定帧率
vtenc -i input.mp4 -o cfr.mp4 --avsync cfr --fps 30000/1001
```

裁剪边距和输出尺寸须满足所选滤镜的要求。YADIF、Bob 和 IVTC 互斥；自动场序需要明确的输入元数据，缺失时应根据素材指定场序。色彩转换、HDR→SDR 和字幕高级参数可分别通过 `--help vpp-colorspace`、`--help vpp-subburn` 查看。

### 码率与质量控制

`--bitrate`、`--quality`、`--cbr` 和 `--vbr` 四选一。码率单位为十进制 kbit/s；`--quality 0..1` 是 VideoToolbox 原生质量参数，不等同于 CRF 或 CQP。原生 VBR 要求 macOS 26+，各控制项仍需硬件会话支持。

```bash
# 平均码率目标与两秒窗口上限
vtenc -i input.mp4 -o capped.mp4 \
  --bitrate 5000 --max-bitrate 8000 --bitrate-window 2

# 原生 CBR；文件转码时必须显式指定固定帧率
vtenc -i input.mp4 -o cbr.mp4 \
  --avsync cfr --fps 30000/1001 --cbr 5000

# 原生质量控制
vtenc -i input.mp4 -o quality.mp4 --codec hevc --quality 0.7
```

### 管道与脚本

```bash
# 输出 MPEG-TS 到 stdout，日志写入单独文件
vtenc -i input.mp4 -o - -f mpegts > output.ts 2> encode.log

# 将进度、诊断和任务结果写成 JSON Lines
vtenc -i input.mp4 -o output-json.mp4 --json-status 2> status.jsonl
```

`-o -` 必须显式指定封装格式，且该封装器需要支持不可寻址的输出。stdout 只写媒体数据，任务日志、诊断和进度均写入 stderr。stdout 开始输出后不再从头重试；脚本应检查退出码，因为已输出的字节无法撤回。

| 退出码 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 运行时或硬件错误 |
| `2` | 参数错误 |
| `130` | 用户取消 |

## 功能简介

| 功能 | 说明 |
| --- | --- |
| 硬件编码 | H.264 8-bit、HEVC 8/10-bit；硬件不可用时明确报错 |
| 解码选择 | 自动硬解与一次软解重试，也可强制硬解或软解 |
| 输入与封装 | 自动探测媒体输入，按文件名或显式格式选择输出；支持原始 NV12/P010、合成帧及 Y4M 文件或 stdin |
| 编码控制 | 平均码率、原生 Quality / CBR / VBR、码率上限、VBR 缓冲容量、速度倾向预设、GOP 上限及指定关键帧 |
| 几何处理 | 裁剪、缩放和显示宽高比设置 |
| 反交错与电影还原 | Bob、YADIF、IVTC，支持显式场序和混合素材处理 |
| 色彩与 HDR | 位深、范围、矩阵、传递函数与色域转换；Hable、Mobius、Reinhard、BT.2390 HDR→SDR；颜色标签与 HDR10 静态元数据控制 |
| 字幕 | 外部 UTF-8 ASS/SSA 烧录，支持字体选择、偏移、透明度、裁剪和多层字幕 |
| 时间轴 | 保留源可变帧率，或显式转换为 CFR / forcecfr |
| 音频 | 默认复制第一条音轨，也可移除音频或转为 AAC-LC；AAC 支持单声道／双声道 |
| 自动化 | 参数关键词帮助、JSON Lines 状态输出、stdout 媒体输出及明确退出码 |

具体组合以参数帮助和运行时检查为准。Dolby Vision 输入不受支持。原始 NV12/P010 输入需要显式指定尺寸和帧数，帧率默认 30 fps，可用 `--fps` 设置；Y4M 或其他缺少必要色彩信息的输入需要补充颜色解释。AAC 转码要求文件输出，stdout 可使用音频复制或无音频模式。

## 开发环境建立

在原生 arm64 macOS 终端中准备以下工具，不使用 Rosetta：

- Xcode Command Line Tools 或 Xcode，提供支持 C++20 的 Apple Clang 和 macOS SDK。
- CMake 3.25+、Ninja。
- FFmpeg 开发库（`libavformat`、`libavcodec`、`libavutil`、`libswresample`）、libass、FreeType。
- 发行打包另需 Python 3.9+、Meson、pkg-config，以及下载依赖源码的网络连接。

使用 Homebrew 安装开发依赖并获取源码：

```bash
# 尚未安装 Apple 命令行工具时执行
xcode-select --install

brew install cmake ninja ffmpeg libass freetype
git clone https://github.com/zyzsdy/VTEnc.git
cd VTEnc
./scripts/bootstrap.sh
```

`bootstrap.sh` 检查平台和工具后构建 Debug，不安装依赖，也不自动运行测试。SDK 使用系统选定的开发工具路径。

也可将 CMake 和 Ninja 安装到项目内的 Python 虚拟环境；FFmpeg、libass、FreeType 仍需单独安装：

```bash
python3 -m venv build/tools
source build/tools/bin/activate
python -m pip install 'cmake>=3.25' ninja
./scripts/bootstrap.sh
```

如使用自定义依赖安装目录，配置时指定前缀：

```bash
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/dependencies
```

头文件和动态库应来自同一依赖安装。预设的 macOS 部署目标为 13.0，但本机构建的实际运行系统要求还取决于所链接依赖及使用的功能。

## 开发构建和运行

所有命令从仓库根目录执行，构建产物位于 `build/`。

### Debug

```bash
cmake --preset dev
cmake --build --preset dev --parallel
./build/dev/vtenc --version
./build/dev/vtenc --help
./build/dev/vtenc --help vpp
```

使用真实素材运行转码：

```bash
./build/dev/vtenc -i input.mp4 -o build/output.mp4 --codec hevc
```

也可用合成帧生成一个短视频；此命令会实际使用硬件编码器：

```bash
./build/dev/vtenc --synthetic --input-res 1920x1080 --frames 120 \
  --fps 30 --codec h264 -o build/demo.mp4
```

按需执行 CLI 检查：

```bash
ctest --preset dev -R '^vtenc.cli$'
```

### Release 与本地安装

```bash
cmake --preset release
cmake --build --preset release --parallel
./build/release/vtenc --version
cmake --install build/release --prefix "$PWD/build/install"
./build/install/bin/vtenc --help
```

`dev` 为 Debug 构建，`release` 为 Release 构建；两者都使用本机依赖。`distribution` 用于下述包含依赖的发行包。

## 打包

### 生成 macOS ARM64 发行包

在 Apple Silicon Mac 上安装打包工具，然后从仓库根目录执行：

```bash
brew install cmake ninja meson pkgconf python
python3 scripts/build-dependencies.py
cmake --preset distribution
cmake --build --preset distribution --parallel
python3 scripts/package-release.py
```

依赖构建使用固定版本及校验值，安装到 `build/distribution/prefix/`。发行包包含所需第三方动态库，不依赖接收方的 Homebrew 安装。

`build/packages/` 中生成以下文件：

| 文件 | 内容 |
| --- | --- |
| `vtenc-<版本>-macos-arm64.zip` | 程序、动态库、使用说明、许可证和构建清单 |
| `vtenc-<版本>-dependency-sources.tar.gz` | 对应的第三方依赖源码及构建材料 |
| `vtenc-<版本>-SHA256SUMS.txt` | 发行文件的 SHA-256 校验清单 |

分发时应一起提供二进制包、对应依赖源码包和校验清单，并保留第三方许可证及声明。打包脚本拒绝覆盖已有的同版本目录；重新打包前请先移走 `build/packages/` 中该版本的旧目录。

### 签名与公证

默认打包使用 ad-hoc 签名，不包含 Apple 公证。如需 Developer ID 签名及公证，先准备有效的 Developer ID Application 证书和已配置的 `notarytool` 钥匙串配置，再执行：

```bash
security find-identity -v -p codesigning
python3 scripts/package-release.py \
  --identity 'Developer ID Application: YOUR NAME (TEAMID)' \
  --notary-profile VTEnc-notary
```

仅签名时可省略 `--notary-profile`。提交公证后，脚本等待 Apple 返回结果，并将结果文件保存到 `build/packages/`；公证不通过时退出报错。证书私钥和公证凭据应保存在本机钥匙串中。
