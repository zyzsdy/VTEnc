# VTEnc 仓库协作约定

本文件适用于整个 VTEnc 独立仓库。VTEnc 与 NegativeEncoder 是并列项目，不继承其仓库配置或协作约定。

## 技术边界

- 目标为 macOS ARM64 / Apple Silicon M 系列，功能范围以截至 M6 的硬件能力为上限，以实际系统和会话探测为准，不根据芯片名称猜测能力。
- 使用 C++20。CLI、参数验证、调度、时间轴以及 VideoToolbox / CoreMedia / CoreVideo 的 C API 使用 `.cpp`。
- 仅在调用 Objective-C API（如 Metal、AVFoundation）时使用 `.mm`，开启 ARC；公开头文件保持 C++ 接口，不暴露 Objective-C 对象。
- 原生调用 VideoToolbox，编码端强制硬件编码并检查实际使用的后端，不得引入软件视频编码器。压缩输入统一使用 FFmpeg libavformat 解封装，不按扩展名或格式白名单预筛选，探测失败直接报告错误。解码支持 `--avhw` 强制硬解、`--avsw` 使用 libavcodec 软解；默认先硬解，硬解失败时输出诊断并从头软解重试一次。不得静默回退，编码、滤镜与输出错误不触发软解重试。
- 借鉴 NVEncC 的模块划分和 CLI 习惯，不机械映射 NVIDIA 专有参数。引入上游代码前核查许可证并保留声明。
- CoreFoundation 对象采用 RAII；异步回调不得让 C++ 异常跨越 C ABI，错误必须传回调度线程。时间戳使用整数有理数。

## 开发与测试

- 从本目录使用 CMake Presets 构建，生成物放在 `build/`，不提交本机 SDK 路径和构建缓存。
- 除非用户明确要求，否则不要自动执行全量测试；只运行与当前修改有关的测试及必要检查。
- CLI 检查使用 `ctest --preset dev -R '^vtenc.cli$'`；硬件探测需要在真实 Apple Silicon 机器上显式执行。
- 区分属性被枚举、会话创建成功和真实码流验证，不把前两者描述为完整编码能力验证。

## 错误处理

- 使用者一般为高级用户，从第一性原理出发明确问题根因和程序约束。
- 遵循 **Let it crash**：尽早暴露错误，并报告操作上下文及必要诊断信息（如 OSStatus）。
- 不静默吞掉异常、不伪造成功、不以默认值隐藏异常。CLI 最外层可以输出诊断并返回非零退出码。

## Git 提交

- 完成任务后使用当前 Git 身份提交，不擅自修改身份。
- 摘要采用 `<type>: <subject>`，type 使用 `feat`、`fix`、`chore`、`ci`、`docs` 等，subject 使用简体中文。
- 除非用户明确要求，否则只提交、不推送；复杂任务可以按合理阶段分次提交。
