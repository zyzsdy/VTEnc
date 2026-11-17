# P3 Bob 反交错验证

日期：2026-11-17。实际设备：Apple M6，macOS 27.2.1，arm64。

**结论分层：Metal Bob、原始输入硬编、倍频时间轴及派生载体的硬解/音轨链路已验证；两个原始交错 MP4 的硬解转码未通过。**不把测试夹具路径称为原始文件全硬件转码成功。

## 原始样片硬解限制

直接运行 `--deinterlace bob --field-order tff`：

| 原始文件 | 结果 |
| --- | --- |
| `deinterlace/interlaced_tff_h264.mp4` | 强制硬解会话创建失败，OSStatus=-12911 |
| `deinterlace/telecine_32_tff_h264.mp4` | 强制硬解会话创建失败，OSStatus=-12911 |

两者均为 720×480、180 帧、30000/1001 fps、TFF 的 H.264 交错编码。错误为 SDK 的 `kVTVideoDecoderMalfunctionErr`，发生在实际解码回调之前；不输出成品，也不改用软件解码。分别检查默认输出属性和显式 NV12 / Metal 兼容输出属性，结果相同。这是本机、本次码流配置的实际失败，不推断所有 M 系列或其他交错编码配置永久不可用。

交错编码路径要求解码器支持并回读确认公开的 `FieldMode=BothFields`，拒绝不支持的属性，防止隐式反交错或单场输出。因为原始样片未能创建硬解会话，本机没有完成该路径的逐帧验证。对于编码标记为逐行、用户明确解释为交错栅格的输入，不请求与该编码模式无关的 FieldMode 属性，直接把解码得到的完整栅格交给 Bob。

参考：[Apple FieldMode](https://developer.apple.com/documentation/videotoolbox/kvtdecompressionpropertykey_fieldmode)、[CoreVideo FieldDetail](https://developer.apple.com/documentation/corevideo/kcvimagebufferfielddetailkey) 及当前 SDK 的 CVImageBuffer / VTDecompressionProperties / VTErrors 头文件。实现为原创，无第三方滤镜源码引入。

## 实现与约束

- `--deinterlace bob` 显式启用；默认保持原先逐行路径，已识别交错输入仍须启用 Bob。
- `--field-order auto|tff|bff`：auto 从图像/格式元数据解析，无信息或冲突时失败；显式 tff/bff 覆盖源解释。原始帧无场序元数据，必须显式给出。
- 对 Y 与 UV 分别限定场行，只在同场内垂直插值；源已有亮度行在不缩放时保持原采样位置，缺行由相邻同场行重建，边界延伸。色度使用每场的 left / center / top-left 相位，再取样到逐行 left 位置，不能把 UV 当作逐行数据混合两场。
- Bob 场重建与现有裁剪、缩放、矩阵/range/位深变换在同一 Metal kernel 中完成，无 CPU 像素滤镜回退。要求源高度、上下裁剪边距为 4 的倍数，裁剪后至少保留 4 行。
- 每输入帧产生两帧，PTS 为 `t` 与 `t+d/2`，duration 均为 `d/2`；用整数有理数运算，拒绝舍入、无效时间或溢出。文件封装视频时间基加倍，与音轨时间基求精确公倍数；A/V 原点和音轨数据不变。
- 解码显示顺序重排后，保留当前帧和一帧前瞻，依次提交两个场输出；一帧 GPU 在途，输出池上限仍为 3。每场完成硬编排空后再继续，取消/失败清理沿用原有策略。
- 原始 NV12/P010 模式新增 Bob，`--frames` / `--fps` 表示输入场对，输出帧数/帧率加倍；仍是同位深、BT.709 limited range SDR、每场 left 色度位置。输出 Annex B 和精确时间戳 CSV，不自动调用外部解码器或封装器。

## GPU 数值与时间规则

显式 `vtenc_gpu_filter_test` 新增 **288 组 Bob 场采样组合**，覆盖 NV12/P010、limited/full、8/10-bit 输出、TFF/BFF 对应的空间场选择、三种色度位置、裁剪/非整数放大/缩小与矩阵转换。两场夹具故意采用明显不同的亮度和色度，能检出跨场混合。CPU double 参考逐像素比对最大误差 **1 个目标码值**，逐行输出 FieldCount=1；原有 240 组 GPU 检查仍通过。

无硬件时间规则测试覆盖 1001/30000、1/25 和不等间隔时长，保留 1/5 秒起点，两个场无缝衔接且区间终点不变；无效、零、负、舍入与溢出时间拒绝。TFF/BFF 自动元数据映射、显式覆盖和缺失场序拒绝均检查。CLI 检查覆盖非法模式/场序、原始场序缺失、合成输入冲突、高度对齐及倍频计数/时间基溢出。

## 实际样片像素与硬编

测试脚本明确用 FFmpeg **离线解码原始样片生成测试夹具**。VTEnc 接收已有原始帧，执行 Metal Bob 和 VideoToolbox 硬编；该路径不声称验证了原始压缩码流的硬解。独立 CPU 参考按行保留/插值，并按 1/8 相位权重重采样场色度，未调用生产滤镜代码。

| 用例 | 实际输出 | 帧数 | CPU 参考平均 PSNR |
| --- | --- | ---: | ---: |
| 普通交错样片原始 NV12，TFF | H.264 8-bit，60000/1001p | 360 | 38.93 dB |
| Hard telecine 样片原始 NV12，TFF | H.264 8-bit，60000/1001p | 360 | 38.91 dB |
| 普通交错原始栅格，显式 BFF 覆盖 | HEVC Main 8-bit，60000/1001p | 360 | 38.95 dB |
| Telecine 素材提升位深后的 P010 夹具，TFF | HEVC Main10，60000/1001p | 360 | 39.88 dB |

BFF 用例用于确认底场先出、场对顺序交换，不把原 TFF 素材宣称为自然 BFF 来源。P010 用例验证 10-bit GPU/硬编，不是原生 10-bit 交错码流硬解验证。

四个用例均完整处理 180 个输入场对。CSV 中全部 PTS/duration 精确为 1001/60000 的整数倍；按已验证帧率无重编码封装后，独立 FFprobe/FFmpeg 检查 360 帧、60000/1001 fps、6.006 秒时长、正确 Profile/位深与严格解码。全部解码帧的 interlaced_frame/repeat_pict 为 0。H.264 逐行标记正确；HEVC 的 field_seq_flag=0、general_progressive_source_flag=1、general_frame_only_constraint_flag=1，通过 trace_headers 核验，未仅依赖 FFprobe 是否显示 field_order。

PSNR 包含有损硬编，阈值为 38 dB；滤镜本身的精度由上述逐像素 GPU 测试验证。四个原始输入用例的 CLI 峰值 RSS 约 **35.28～38.14 MiB**；不包含全部 GPU、驱动或独立媒体服务内存，也不是长时间稳定性或零拷贝承诺。

## 硬解、调度与音轨验证

为了实际覆盖容器路径，另以 VTEnc 原有硬编将普通交错样片的原始栅格编码为**逐行标记的交错栅格载体**，再无重编码加入 AAC 和 1/5 秒视频起点。此派生文件可硬解，但其编码结构已与原始 MBAFF 文件不同。

| 输入 | Bob 输出 | 验证 |
| --- | --- | --- |
| 派生交错栅格载体 + AAC | 360 帧，裁剪后缩放到 360×240，HEVC 10-bit | 真实硬解/Metal/硬编，260 个音频包、PTS、priming/trim 与 PCM 相同 |
| 原 VFR 样片，显式按 TFF 强制解释 | 440 帧，裁剪后缩放到 320×180，HEVC 10-bit | 每个 VFR 区间精确平分，518 个音频包、PTS 与 PCM 相同 |

两者都保留源视频 1/5 秒起点，不按轨归零。第二项用于时间轴和音轨验证，不声称其源内容天然交错。六个成功 GPU/硬编或派生载体用例合计 **2,240 个输出帧**；原始两个压缩文件的硬解受阻单独列在报告中。

缺失场序、UV 场对齐不合法的裁剪均失败且不发布输出；临时封装开始后发送 SIGINT 返回 130，无成品和临时目录残留。Bob 保留 hard telecine 重复场，不执行 IVTC，不恢复 24000/1001p；未实现 soft telecine 重复场展开、混合模式检测、场匹配或运动自适应。单场线性重建的垂直细节损失与闪烁属于当前算法限制。

## 复现与回归

```bash
source build/tools/bin/activate
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R '^vtenc.cli$'
ctest --preset dev -R '^vtenc.timeline$'
./build/dev/vtenc_gpu_filter_test
python3 tests/bob_hardware.py --vtenc build/dev/vtenc
# 共享原始编码、文件调度和 GPU 路径的相关回归
python3 tests/encode_hardware.py --vtenc build/dev/vtenc
python3 tests/transcode_hardware.py --vtenc build/dev/vtenc
python3 tests/filter_hardware.py --vtenc build/dev/vtenc
cmake --preset release
cmake --build --preset release
```

Debug / Release 构建、CLI、时间轴、P1/P2/P3 相关回归通过，无编译警告；未执行全量 CTest。Release 另以原始交错夹具检查真实 Bob 编码。硬件测试仍不注册到默认 CTest。

本次 Bob 报告：`build/hardware-tests/bob-xvvx7h_s/verification.json`。相关回归：`p1-hjftroxr`、`p2-x009jf_c`、`p3-qr3_y4sv`，均位于 `build/hardware-tests/`。报告与较大的原始/CPU 参考夹具保留在 `build/`，不提交。脚本遇到本次已确认的原始硬解创建失败时记录 `blocked_hardware_decode` 并继续独立验证，不将该项标成通过；其他意外错误直接使测试失败。
