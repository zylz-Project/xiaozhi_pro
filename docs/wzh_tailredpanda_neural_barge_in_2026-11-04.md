# 小熊猫人声打断：成熟方案对照与神经前端候选（2026-11-04）

**历史记录：这版已经由用户烧录，最新反馈显示实时过载且不能打断，未通过验收。**
日志出现 45145/32000 us、AFE FEED ringbuffer full 和 CPU1 IDLE1 看门狗告警。
下文记录首次候选时的软件检查，不代表实机通过；旧候选包已被本轮修订替代。
请使用[实时性修订与自审](wzh_tailredpanda_barge_in_realtime_2026-11-04.md)中的新候选。

## 网上方案与选择

| 路线 | 已核实的能力 | 本轮结论 |
| --- | --- | --- |
| ESP-SR FD AEC + NSNet2 + VADNet | S3 支持这些组件；NSNet2 为 16 kHz、512 样本步进，官方有独立调用接口；VADNet 提供检测与清状态接口 | 使用工程已依赖的 2.4.7，显式接入 AEC 后处理，先做可验证的候选 |
| SeekAudio S3 前端 | 提供 AEC/降噪对照工程及近端静音、近端单讲、纯回声、双讲的状态 API | 值得后续独立对照；核心二进制仅供评估，公布效果是厂商自测，未接入或重新分发该库 |
| WebRTC AEC3 | 有渲染参考、延迟处理、残余回声估计、近端占优检测等完整模块 | 可作完整 AEC 的主机对照；S3 的移植与资源预算未验证，不把几行检测阈值等同于整套算法 |
| SpeexDSP | 成熟的开放 AEC/后处理接口，明确要求参考与录音正确同步 | 可作传统算法对照；单纯换库不能解决错误参考或播放失真 |

一手资料：

- [乐鑫 NSNet](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/nsnet/README.html)、
  [VADNet 独立 API 测试](https://github.com/espressif/esp-sr/blob/master/test_apps/esp-sr/main/test_vadnet.cpp)、
  [乐鑫 AFE 基准](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html)。
- [SeekAudio 对照工程](https://github.com/seekaudio/seekaudio_aec_test)、
  [其 B 组 FD AEC/NSNet2 接线](https://github.com/seekaudio/seekaudio_aec_test/blob/main/main/fd_aec_backend.cpp)、
  [近端分类 API](https://github.com/seekaudio/seekaudio_aec_test/blob/main/components/seekaudio_aec/include/seekaudio_aec.h)、
  [仓库许可证与二进制例外](https://github.com/seekaudio/seekaudio_aec_test/blob/main/LICENSE)。
- [WebRTC 近端检测](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_processing/aec3/dominant_nearend_detector.cc)、
  [残余回声估计](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_processing/aec3/residual_echo_estimator.cc)。
- [Speex 回声消除与参考同步](https://www.speex.org/docs/manual/speex-manual/node7.html#SECTION00740000000000000000)。

小智 #2251 也有播报误打断、收紧后又漏检人声的报告。其评论建议 AEC 后接神经降噪；这只是
方向参考，不是官方修复，也没有在本机上验证通过。
[同类反馈](https://github.com/78/xiaozhi-esp32/issues/2251)

## 落地结构

```mermaid
flowchart LR
    MR[麦克风＋DAC 数字参考] --> AEC[原 FD AEC]
    AEC --> NS[显式 NSNet2 后处理]
    NS --> VAD[对处理后 PCM 做 VADNet]
    VAD --> Frame[对齐 60 ms PCM 和检测状态]
    Frame --> Gate[参考有效性、能量、回声否决、持续时长]
    Gate --> App[主任务复核后取消回复]
```

新增 `VOICE_BARGE_IN_NEURAL_FRONTEND`，只在 `wzh-tailredpanda-barge-in` 入口启用。
普通入口关闭该开关。扬声器维持上一轮 `2a73ad6` 的音质与响度配置，输入参考、ADC 6 dB、
输出时钟和原协议保留；本轮没有通过整体压低扬声器音量来规避误触发。

实现的关键点：

1. 保留 AFE/WakeNet 的现有链路和 FD AEC；AEC 输出通过独立 NSNet2 API，再送独立 VADNet。
   AFE 内部仍可有 WebRTC VAD，但其检测结果不作为新链路的最终人声状态，也不创建第二个
   AFE 内的 VADNet 实例。WakeNet 和本地命令路径不重复跑这组后处理。
2. 神经模型以 512 样本处理；送编码器仍是原来的 960 样本。每包的语音状态覆盖所有贡献片段，
   必须都为语音才标为 speech，避免最近 32 ms 状态重标较早的整包音频。该保守拼帧可能增加
   起音确认时间及短词漏检，须实测；已有 720 ms 压缩前缀继续保留。
3. 新适配层用两块固定、16 字节对齐的 512 样本缓冲，逐帧不新建容器或模型。模型仅在初始化
   分配；reset 复用缓冲，推进两次静音 hop 清出 NSNet2 的 64 ms 窗口，再清 VAD 历史。
   这不是重新加载模型，模型内部抑制历史与实测起音效果仍需核对。
4. 检查模型名称、帧长、采样率、声道和接口指针。缺模型/分配失败时释放部分资源，保留常规
   AFE；`CanMonitorVoiceInterruption()` 阻止进入自动打断，避免悄悄退回已知误触发的判据。
5. 将会话 generation 传到处理与输出阶段，推理期间发生退出/重置时丢弃过期结果。模型实例
   在模型映射释放前关闭，重复关闭安全。
6. 该实验入口的处理任务栈从 4 KB 增到 8 KB，优先按原规则放 PSRAM。按每批输入的实时预算
   记录神经处理超时，最多每 5 秒告警一次；运行时 CPU、栈余量和内部 SRAM 仍需实机测量。

## 自审发现并修复的资源缺口

原 `scripts/build_default_assets.py` 只打包 WakeNet/MultiNet。仅在 sdkconfig 选择 NSNet2
和 VADNet 时，生成资源里仍没有它们，因此只看代码编译成功会遗漏实际加载失败。

已补全：读取选中的模型 → 验证 `_MODEL_INFO_`/index/data 文件 → 加入 srmodels.bin →
资源生成依赖包含模型文件。必需模型缺失或打包失败会明确失败。默认没有选择神经模型时保持
原来的 WakeNet/MultiNet 打包路径。

对最终生成的 assets 实际解包，检查外层长度/校验、srmodels 模型表、各文件边界，并将
NSNet2/VADNet 的全部六个文件尺寸和 SHA-256 与组件中的源模型逐项比较。已确认包含：
`mn7_cn`、`wn9l_ni3hao3xing1bao3_tts3`、`nsnet2`、`vadnet1_medium`、`fst`。
候选 app 为 2,846,912 字节，assets 为 4,535,862 字节，适用原 16 MB Flash / 8 MB assets 分区。

更新候选时需要匹配的 app 和 assets；仅更新 app、沿用旧资源不会获得新模型，日志会明确禁用
自动打断。资源格式和 NVS API 没有更改，外置本地声音不需要重新烧写。

## 软件检查与边界

- 全量 238 项主机检查通过（75.656 秒）。23 项相关检查在 ASan/UBSan 编译环境下通过；
  最终显式缓冲对齐后，5 项神经前端检查再次启用 ASan/UBSan 通过。
- 测试覆盖后处理顺序、不同输入分块、reset 清旧波形、模型/格式/分配失败、清理、VAD/PCM
  对齐、推理中会话失效、保留前缀及既有两种传输的共享取消/聆听语义。
- 测试使用确定性的 SDK 替身，验证生产接线代码；它们不验证神经模型的真实声学分类准确率。
  真实 SDK 接口和模型符号由 S3 固件构建验证。
- 使用本机 IDF 6.0.1 构建通过：S3 神经打断入口 2,846,912 字节、普通入口 2,839,264 字节、
  C3/4G 入口 1,607,728 字节。普通入口资源的 SHA-256 与上一轮音质版完全一致；普通入口和
  C3 均确认关闭神经前端。6.0.2 本机未安装。
- 兼容性构建后已恢复 S3 神经打断候选；当前 build 目录与该测试入口对应。普通入口另存作对照。
- C/C++ 格式及差异空白检查通过。没有复制第三方评估二进制，也没有改变已有依赖版本。

NSNet2 和 VADNet 是成熟的基础组件，但清晰残余 TTS 也属于语音；本轮并未获得独立的双讲
真值接口。现有相关性检查仍只是回声否决启发式，**新候选不保证从此不自打断**。若实测仍
不够，应以同一批原始 MR 录音对照完整回声感知/双讲前端，并重新评估参考、播放失真与隔振，
不能继续把“检测到语音”直接等同于“已经证明是用户”。

## 稍后的实机对照

按用户安排本轮不烧录。后续至少分别采集：纯播报、人声单讲、播报中正常人声双讲，覆盖起播、
句间、较长故事、常用及较大音量、动作开启/停止。统计误取消、漏检、响应延迟、首字和新 STT。
同时检查超时日志、最小 SRAM/栈余量、连续对话、唤醒、双击退出和重连。高音量时的声学参考和
硬件失真是否合格，仍需测量；本轮不以降低用户响度作为通过条件。
