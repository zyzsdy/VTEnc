# Metal IVTC 验证记录

日期：2026-11-17。真实 macOS ARM64 / Apple Silicon 本机执行；未运行全量 CTest。

## 移植范围

参考 rigaya/NVEnc `5bb35d860bc5f877b01fe60e4de5ac756b9b80ed` 的
`NVEncFilterIvtc.cu/.cpp`。MIT 声明见仓库 `THIRD_PARTY_NOTICES.md`。
移植 C/P/N 评分、32×8 块归约、场重组、三帧残余梳齿修复、亮度 SAD 与五帧去一帧。
CPU 只读取块统计、选择匹配并调度，像素运算在 Metal 上完成。

增加流式 RFF 展开与保守混合路由，复用 Metal 场重组；CPU 管理逐帧标记、待配对场和有理数时间区间。
不是完整 NVEnc IVTC 兼容层：不含全片预扫描、cadence lock、五帧 BWDIF 或双场序置信度分类。
VTEnc 针对干净 P→N 候选增加连续性保护；混合段允许排空后切换场序。时间轴保持各源周期总时长，尾部不完整周期全部保留，不强制全片 CFR。
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
python3 tests/rff_hardware.py --vtenc build/dev/vtenc
ctest --preset dev -R '^vtenc.hevc-cadence$'
python3 tests/hevc_rff_hardware.py --vtenc build/dev/vtenc
```

本机 CMake 位于 `build/tools/bin`；运行 Python 硬件脚本时将 `/opt/homebrew/bin` 加入 PATH。
构建成功，编译无新增诊断。链接仍提示本机 FFmpeg dylib 的最低系统版本 27.0 高于项目部署目标 13.0；此次仅验证当前机器，不据此宣称 macOS 13 运行兼容性。

- CLI 检查通过，涵盖裸选项、可选参数、互斥选项、未知/重复/越界参数及原始输入拒绝。
- 时间轴检查通过，包含有理数四等分、2/3/4/6 场分时、展开和去重组合时间基、溢出拒绝，以及 C/P/N 排序。
- `vtenc_ivtc_test`：146 个用例通过。检查 GPU SAD、分块评分、C/P/N 重组与残余修复；覆盖 NV12/P010、limited/full、TFF/BFF、非整线程块尺寸、P010 低位为零。另以已知电影帧生成五种相位的硬 telecine，检查中间输出逐像素恢复与无漏帧/重帧；验证 1–11 帧长度、VFR、非零 PTS、match-only 及所有 EOF 余数。
- 新增 RFF 覆盖：独立显示场列表验证 4→5 展开、16→16 电影还原，NV12/P010 与 TFF/BFF，编码图片 CFR/3:2 变时长，完整帧双/三重复，1–7 图片尾部，源场序断裂、混合段 TFF→BFF 切换，以及五种周期偏移的逐行→soft→hard→逐行切换。异常 repeat、未知逐帧标记、缺失 RFF 尾帧时长、冲突配置均拒绝；真正交错视频不降帧。
- 前次共享 Metal 回归（本次未修改着色器，不重复全量执行）：252 色彩转换、200 tone mapping、288 Bob、240 基础像素用例通过，共 980 个；最大误差 1 码值。

## 独立码流验证

`tests/ivtc_hardware.py` 使用 FFmpeg/ffprobe 独立解码、检查包时间戳与码流场标志。
测试夹具准备仅执行解码、参考滤镜或 remux；新增测试视频由 VTEnc 硬件编码生成。
最终运行报告：`build/hardware-tests/ivtc-a61vnv1k/report.json`（本机生成物，不提交）。

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


## RFF 与混合码流验证

运行 `tests/rff_hardware.py`，最终报告为 `build/hardware-tests/rff-_w6h2ycz/report.json`。
视频夹具先由 VTEnc 硬件编码为 H.264 all-I，再替换 SPS VUI 并逐图片插入合法 `picture_timing` SEI；不改 slice 像素码流，也不使用软件视频编码。FFmpeg 独立读取 RFF 标记、严格解码并比较参考图片；packet PTS/duration 使用显式 1/60000 时间基，避免裸 H.264 帧率估计误差。

- 12 个成功输出：TFF/BFF × 强制硬解/软解 × 三种输入/选项组合。所有编码端确认实际硬件使用，强制硬解用例也确认实际硬件解码。
- 混合夹具：5 个逐行图片＋8 个 soft 图片＋10 个 hard 栅格＋5 个逐行图片。28 个编码图片展开为 30 个显示帧，输出 26 个依次连续的已知电影/视频图片；逐行段保持 30000/1001，电影段为 24000/1001。
- 纯 soft：8→10→8，使用 `--field-order auto`；`expand=off,mixed=true` 则 8→8，保留原始 3/2 场变时长而非重新分配为 CFR。
- 所有用例逐包核对 PTS/duration、0.2 秒起点及片尾；混合用例的 119 个 AAC 包按 SHA-256、PTS、duration 完全相同。每帧独立解码像素与已知参考相同（报告用 100 表示零 MSE，不将它当作有限 PSNR 测量）。这只验证当前结构化夹具，不代表任意压缩影片无损还原。
- 上一阶段曾以 HEVC `pic_struct=5` 验证显式拒绝路径；本次已由下面的 HEVC 正向解析与错误输入测试替代，不再拒绝合法 HEVC RFF。

修复了纯 soft H.264 的显示场标记被当作交错编码能力的问题：现在以 SPS `frame_mbs_only_flag` 决定是否请求 VideoToolbox `BothFields`。此前纯 soft 用例报 `OSStatus=-12900`；修复后强制硬解、Metal、硬编完整码流均通过。

## HEVC picture-timing 与重排验证

HEVC 新增独立 C++20 元数据解析器 `src/apple/hevc_cadence.cpp`，按 [ITU-T H.265](https://www.itu.int/rec/T-REC-H.265) 的 SPS/VUI 与 Annex D picture-timing 语法实现，不使用 FFmpeg 私有 ABI，也不引入软件视频编码。当前 [FFmpeg HEVC SEI 解析](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/hevc/sei.c) 不保留 3:2 `pic_struct=5/6`，因此硬解与软解共同使用此解析结果。

- hvcC 的 1/2/3/4 字节 NAL 长度和 Annex B 均支持；处理防竞争字节、多个 SEI、扩展 payload type/size、子层 PTL、scaling list、显式/预测 RPS、长参考及 VUI 可选项。
- 每个完整 access unit 从 slice 的 PPS ID 定位 SPS，支持带内参数集更新、多个 PPS/SPS 和多 slice；不沿用前一图片的 SEI。VUI 缺失时按 PTL 推导 `frame_field_info_present_flag`。
- `pic_struct=0/3/4` 不增加场，5/6 增加一场并分别使用 TFF/BFF，7/8 增加 2/4 场。`source_scan_type` 参与源扫描类型判断；`duplicate_flag` 不等于 RFF，不增加显示场。HRD 延迟语法位于这些字段之后；本实现只读取 cadence，不使用 HRD 改写容器时间戳。
- 软解用 `AV_CODEC_FLAG_COPY_OPAQUE` 将每个包的解析结果传给对应解码帧，跨 frame threading 和 B 帧重排；没有对应标记时直接失败。原生路径在显示队列排序前绑定标记。VideoToolbox `BothFields` 根据 SPS 的 `field_seq_flag` 设置，不把 soft telecine 的显示场当作编码场。
- 多层 NAL、独立单场图片/`field_seq_flag=1`、保留枚举、截断或缺失必需 SEI 明确拒绝。目标仍是完整帧栅格的 soft/hard telecine，不宣称支持独立场图片拼接。完整 slice/HRD 码流一致性由解码器负责。

`vtenc.hevc-cadence`：111 个解析/错误用例通过；覆盖以上语法分支、参数集切换、HRD 数据不误读为 `pic_struct`、跨 access unit 的 SEI 清空和非法输入拒绝。

`tests/hevc_rff_hardware.py` 报告：`build/hardware-tests/hevc-rff-ned_zlp8/report.json`。

| 用例 | 覆盖与结果 |
| --- | --- |
| Main/Main10 × TFF/BFF × soft/mixed × 硬解/软解 | 24 个输出；混合 28→30→26，纯 soft 8→10→8；关闭展开时 8→8 保留原图片区间 |
| MPEG-TS Annex B × 8/10-bit × 硬解/软解 | 4 个视频输出；整数时间轴按 TS 原始包时长验证，不假定 90 kHz 能精确表示每个 RFF 场 |
| 含 B 帧的真实 HEVC 8/10-bit 样片 × 硬解/软解 | 4 个输出，各 82 帧；测试 remux 工具按显示 PTS 插入不同 RFF 标记，不改变 slice 字节、不重新编码；逐个显示帧核对 RFF 标记以及输出 PTS/duration |
| RFF 结尾的 1/3 图片 × 8/10-bit × 硬解/软解 | 8 个输出；最后图片时长及剩余单场区间精确保留，单场 EOF 有诊断 |
| 缺失必需 SEI/保留 pic_struct × 强制硬解/软解/自动 | 6 个明确失败；无成品，不触发软件解码重试 |

共 40 个成功输出和 6 个错误路径检查。视频始终硬件编码，所有强制硬解用例确认实际硬件解码。结构化夹具逐帧比较已知参考，最低 PSNR 为 56.01 dB；MP4 混合用例逐包保留 119 个 AAC payload、PTS、duration。Annex B 用例只测视频，避免把 TS ADTS→MP4 AAC 的正常封装转换误报为 payload 改变。B 帧用例使用 `expand=off,cycle=0` 单独验证重排后的元数据对应，RFF 展开与混合去重由其他正向用例验证。

本次亦重跑 H.264 RFF 的 12 个成功输出；原有 hard-IVTC 像素核心未修改，不据此重复全量 GPU/CTest。
