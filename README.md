# VTEnc

面向 Apple Silicon M 系列的原生 VideoToolbox 硬件编码器，借鉴 NVEncC 的 CLI、能力探测和模块划分，使用 C++20 + 少量 Objective-C++。

本仓库独立于 NegativeEncoder；未来通过命令行协议集成。功能范围以截至 M6 的硬件支持为目标，每项能力以当前系统的真实探测为准，禁止软件视频编解码回退。

## 当前状态

开发框架阶段，**尚不能对视频文件进行转码**。现有功能：

- CMake / Ninja 的 Debug、Release 构建与安装目标。
- 纯 C++ CLI、参数校验，以及独立的 Apple 框架适配库。
- H.264 8-bit、HEVC 8/10-bit 的硬件会话探测与支持属性枚举。
- Objective-C++ / ARC 的 Metal 设备查询。
- 不依赖硬件编码器的 CLI 冒烟检查。

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

后续终端使用该环境前执行 `source build/tools/bin/activate`。Python 仅用于安装开发工具，`vtenc` 本身没有 Python 运行时依赖；`build/` 整体不纳入 Git。

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

退出码：`0` 成功、`1` 运行时或硬件错误、`2` 参数错误。结果写入 stdout，诊断写入 stderr。当前只提供文本输出，JSON 协议留待后续实现。

## 代码布局

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
