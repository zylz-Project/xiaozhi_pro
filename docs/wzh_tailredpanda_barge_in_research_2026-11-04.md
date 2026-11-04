# 小熊猫普通人声打断：可靠性调研（2026-11-04）

后续实施更新：已补充 S3 成熟方案对照，并将显式 NSNet2/VADNet 接入打断候选，同时补齐模型
打包和帧状态对齐。详见[神经前端候选与自审](wzh_tailredpanda_neural_barge_in_2026-11-04.md)。
下文为实施前的调研记录，实机验收仍未完成。

## 结论与当前证据

普通人声打断仍未通过实机验收。应当补齐播放参考/声学链路验证、残余回声处理和近端人声确认，
不能继续把“VAD 为语音、能量超过门槛、与参考相关性较低”当作用户开口的充分证据。
以下为源码核对和一手资料调研，不是新固件已修好的声明；本次没有改动运行时代码或烧录样机。

11 月 4 日用户提供的两轮播报日志都执行了 `Voice interruption`：

- 第一轮起播后，`ref_ready=1`、`settle=300ms`、`clipped=0/200`；随后错误取消。
- 取消后的残余观测分别为 RMS 371 / gate 199 / correlation 32%，以及 RMS 305 / gate 160 /
  correlation 31%。这些是周期日志保留的观测，不能当成触发瞬间所有帧的精确记录。
- 用户确认没有开口，是自身播报误触发。参考有信号、ADC 未削顶，也不足以排除播放失真、
  时序不匹配、非线性残余、运动噪声或判定器问题；不能据此断言某一项就是硬件根因。
- 片段没有启动配置、固件哈希，无法确认实际装机参数。实际发生取消说明它不是仅记录的影子
  模式；不能继续沿用 11 月 3 日采集时的装机状态描述。
- `vad=0` 出现在取消后的周期日志，不能证明触发时 VAD 也为 0。

本地源码快照 `0c0c9fb`：ESP-SR 锁定 2.4.7；打断入口使用 FD_HIGH_PERF/VERYAGGR、
AFE 增益 1、ADC 6 dB；`ns_init=false`，当前生成配置为 WebRTC VAD。ES8311 是一个实际
麦克风加内部 DAC 数字参考（MR），不是双麦阵列，也不是功放输出的独立模拟采样。

`VoiceBargeIn::Observe()` 对相关性低于 0.55 的残余允许累计 180ms 并直接上报取消。
真实回声经过声学路径和非线性处理后可以低相关，故现有逻辑缺少可靠的近端来源确认。
降低相关性门槛、提高 RMS 或延长起播保护都会改变漏检/误检平衡，不能作为已证明的修复。

## 一手资料发现

1. 乐鑫已有专门的全双工 AEC，包含线性消除和非线性残余处理。VERYAGGR 抑制更强，
   同时可能更伤近端人声，不能把最大抑制等同于最佳打断体验。
   [ESP-SR AEC](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html)
2. 官方 AFE 区分 AEC、NS 和 VAD。NSNet2 明确支持 S3，官方基准有
   `AEC -> NS(nsnet2) -> VAD(vadnet1_medium)` 的 MR/VC 管线。FD 基准没有列 NSNet，
   因而不能直接假定 FD 下设一个开关就一定获得此处理顺序，需检查实际 pipeline 或独立接入。
   [NSNet](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/nsnet/README.html)、
   [AFE 基准](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html)
3. WebRTC AEC3 分别估计残余回声频谱、噪声和近端信号，并使用触发/保持状态判断近端占优，
   还处理延迟、饱和和混响。它提供了架构参考，但不是能把其中几行阈值搬到 S3 就获得同等能力。
   [残余回声估计](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_processing/aec3/residual_echo_estimator.cc)、
   [近端检测](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_processing/aec3/dominant_nearend_detector.cc)、
   [AEC3 接口](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_processing/aec3/echo_canceller3.h)
4. 乐鑫要求验证参考采样位置、增益/延迟、播放失真以及麦克风与扬声器的隔振。数字参考能用，
   但不包含它后面的功放、扬声器和结构产生的失真；当前 `clipped=0` 只验证了已有 ADC 削顶
   检查，不是功放/扬声器 THD 检验。
   [S3 麦克风与参考设计指南](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/Espressif_Microphone_Design_Guidelines.html)、
   [乐鑫音频 FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/audio-development-framework.html)
5. 小智仓库 #2251 有相似的用户一手报告：播报误打断，另一版不再误打断但也难以被人声打断。
   评论建议 AEC 后加神经降噪，但该讨论没有提供在本机硬件上通过的修复证据，不能照搬其音量
   数字或把评论当成官方验收。它说明问题类型相似，不证明根因相同。
   [小智 #2251](https://github.com/78/xiaozhi-esp32/issues/2251)
6. 调研时注册表最新稳定版是 ESP-SR 2.6.1；公开更新以 WakeNet/模型和接口变化为主，
   未见对本例单麦误打断的明确修复承诺。2.4.7 已含 FD AEC，升级只能做独立对照。
   [2.6.1 注册表](https://components.espressif.com/components/espressif/esp-sr/versions/2.6.1/readme)、
   [更新日志](https://github.com/espressif/esp-sr/blob/master/CHANGELOG.md)

## 建议实施顺序

### 1. 先验证参考和声学条件

采集同一时轴的 MR 输入和处理后 PCM，在低/常用/较高音量下分别标注纯播报、只有用户人声、
同时说话，并比较动作开启/停止时的变化。核对实际配置与镜像哈希，测参考与麦克风延迟、
起播收敛和残余尾音；检查功放/扬声器失真，必要时限定经过实测的音量范围。
现有两秒一次的 RMS 不能替代这些证据。

采集工具已有包序号和采样位置，可暴露丢帧。`device_us` 是回调时刻，不是硬件采样时钟。
用户说“已说”的聊天时间只能粗略标注，下一轮应采用事先约定的录音内时间段/固定句子。
原始录音按本地私有数据保存；此前 `/tmp` 目录在本轮已不存在，不能继续依赖旧录音或缓存
镜像进行重分析/烧录。新的采集和候选镜像应放在持久的本地忽略目录，并保存清单/哈希。

### 2. 优先试验当前 S3 能支持的处理管线

候选为正确对齐的 FD AEC，随后 NSNet2，再由处理后的 PCM 运行 VADNet 和回声感知判定。
先保持当前 ESP-SR 版本，分开对比 NSNet2、VADNet 及其组合，避免同时升级依赖造成混淆。
现有组件已经有 S3 NSNet2/VADNet 接口和模型，不需要先换 MCU。

必须确认 NSNet 真正位于判定使用的 VAD 之前。若独立 NSNet 放在 AFE fetch 后，就不能继续
沿用降噪前的 `result->vad_state`。帧长、延迟、前缀缓存、CPU、内部 SRAM 和 PSRAM 均需测量。
官方其他板卡/模型的资源数值不能当成本机保证。

这是最值得先做的软件对照，不是可靠性承诺。NSNet 主要去噪；VADNet 主要辨别语音活动。
清晰的 TTS 仍属于语音，两者都不能单独替代使用播放参考的近端/双讲检测。

### 3. 增加可撤销的确认阶段

工程建议：疑似人声先进入候选阶段，短暂平滑降低播放或暂停、保留当前回复和待播放队列，
在实际参考衰减且旧 AFE 缓存与混响尾音排出后，检查是否仍有连续人声；确认后才执行协议
abort、清队列并转聆听，否则恢复播报。保留打断前的人声前缀。

这是针对本机的待测设计推断，不是已验证的官方算法。它可提高“永久取消”前的可辨性，
但纯回声候选频繁时仍会出现可听见的短暂降音/停顿；短词可能在确认前结束，尾音和动作噪声
也可能造成误判。验证窗口必须由实测延迟决定，不能简单固定为某个毫秒数即宣称可靠。
验收必须同时统计误暂停和误取消；不能仅把误取消藏成频繁暂停。

### 4. 若需要高音量/远距离的稳定全双工

评估双麦采集、合理参考采样/隔振和专用 AEC 前端，或在主机侧用完整的 AEC3 做录音对照。
XMOS 等前端提供 AEC、延迟估计和近端/远端相关元数据，属于需要硬件集成与声学验证的路线，
不能当成现有 S3 的直接替换库。
[XMOS AEC](https://www.xmos.com/documentation/XM-010445-UG/html/doc/rst/audio_processing/aec.html)、
[延迟估计与校正](https://www.xmos.com/documentation/XM-010445-UG/html/doc/rst/audio_processing/adec.html)

现有 ES8311 的第二路用于内部数字参考；添加第二麦克风需要新的采集能力，不能只改引脚。
是否需要硬件改版，取决于第一、二步的测量结果。

## 实机验收口径

- 纯播报覆盖起播、句间停顿、长句、不同音量和动作，统计误候选、误降音/暂停和误取消。
- 用户按固定距离、正常音量说不同短句，统计成功率、漏检和从开口到停播的延迟。
- 确认首字保留、新 STT/新回复正确；重复打断、唤醒、双击退出、静音、重连均正常。
- 先确定双方可接受的音量/距离范围及误触发指标，再宣称达到该范围内的可靠性。
  编译和合成 PCM 测试不能替代上述验收。

现有唤醒词/按键可作为过渡交互，但不等于完成“普通人声直接打断”的原始目标。
