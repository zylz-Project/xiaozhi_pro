# WZH 产品本地播报：录音生成、音包接入与验收（2026-11-05）

## 结论

三个产品改为内置**本产品音色**的离线播报，覆盖配网、添加设备引导、验证码数字、
就绪、升级、低电量与关机。播放不依赖联网，设备运行时不调用任何 TTS 接口。

| 产品 | 音色 | 音包目录 |
|---|---|---|
| `wzh_dinosaur` | 阳光青年 | `main/assets/wzh_system_voices/dinosaur` |
| `wzh_crawlPanda` | 樱桃丸子 | `main/assets/wzh_system_voices/panda` |
| `wzh_tailRedPanda` | 樱桃丸子 | `main/assets/wzh_system_voices/panda` |

## 录音来源与身份校验

录音由火山引擎「豆包语音合成大模型」产品体验页按本仓库文案逐条合成，使用用户在
专用 Edge 窗口中自行完成的登录会话；未读取、未导出任何账号口令、验证码或 Cookie。

- 页面试听样本（预置 mp3）**不是**本仓库文案，未使用。
- 每条录音另存同名的 `*.source.json`，记录提供商、页面地址、所选音色标签、
  请求实际使用的音色标识、文案与时长。
- 导入时 `prepare_wzh_system_prompts.py --verify-official-web-source` 逐条比对
  「音色标签 + 文案 + 提供商 + 页面」，不一致即拒绝；导入后重新计算哈希。
- 音包记录 `source_identity_verified`，表示**来源音色已核实**。这与「实机试听
  确认」是两件事，后者仍是待办验收项。

## 统一文案（`main/assets/wzh_system_prompts.json`，共 17 条）

| key | 文案 |
|---|---|
| `0`～`9` | 零。一。二。三。四。五。六。七。八。九。 |
| `wificonfig` | 请用手机帮我配网。 |
| `activation` | 请在小智控制台添加设备，验证码是： |
| `welcome` | 你好呀，很高兴见到你！ |
| `success` | 我准备好啦！ |
| `upgrade` | 正在更新，请勿断电。 |
| `low_battery` | 我的电量不足啦，请帮我充充电吧！ |
| `power_off` | 我要休息啦，下次再一起玩吧！ |

对原提示音的取舍：

- 配网与升级只保留必要信息，避免把原本约 2 秒的短提示换成长篇朗读。
- 添加设备的地址与操作步骤仍以配网页面显示为准，语音只做引导，不念 IP。
- 保留开机叮咚、动物叫声、唤醒/退出提示及警示音，不把非语言音效改成人声。

## 接入方式

- `main/boards/*/config.json` 三个产品（含 `wzh-tailredpanda-barge-in`）启用
  `CONFIG_WZH_SYSTEM_PROMPTS=y`。
- `scripts/cmake/wzh_system_prompts.cmake` 按板型选择 `dinosaur` / `panda` 音包，
  构建时逐项校验（清单、文案、哈希、Ogg/Opus 单声道、时长上限）。
- 替换的是**同名文件**：仍使用原 `Lang::Sounds::OGG_*` 符号，不会同时内嵌两套
  同名符号，也不影响非 WZH 板型。
- `masters/` 保存原始 mp3，CMake 只匹配 `*.ogg`，原始文件不会进固件。

## 音量与语速统一

网页逐条合成的原始录音本身只差约 8～10 dB（正常语句差异）。真正的问题是早期转换
使用了 `loudnorm` 的自动模式，对 1 秒级的数字短句判断失准，把部分条目压掉了
20～35 dB。现改为确定性处理，不再改动原始动态：

1. 语速按产品配置：恐龙 `tempo=0.92`（放慢约 8%，用户反馈原速偏赶），
   两款熊猫保持 `tempo=1.0` 原始语速。时间拉伸保音高，不做重录。
2. 在目标语速的音频上测量门限 RMS，按固定目标 `-19.5 dBFS` 施加一次线性增益。
3. 用 `alimiter` 把峰值压在 `-1.5 dBFS`，避免个别句子为对齐音量而削顶。
4. 编码为 Opus 后复测，仅在仍有峰值余量时做 ≤3 dB 的头部增益微调。

实测结果：恐龙 17 条 RMS 区间 **-20.0～-19.3 dBFS（差 0.7 dB）**、时长 1.00～3.34 s；
熊猫 **-20.7～-19.3 dBFS（差 1.4 dB）**、时长 0.70～3.97 s。两端峰值均 ≤ -1.5 dBFS。

`pack.json` 记录每条 `rms_dbfs`、`tempo`，构建校验会拒绝偏离目标超过 2.5 dB、
峰值余量不足，或语速与 `wzh_system_prompts.json` 中该产品设置不一致的音包；
早期没有电平元数据的音包按格式、文案与身份校验。

## 低电量与关机

- 独立低优先级任务投递音频，不阻塞应用主循环、音频任务和电源采样。
- 低电量沿用既有稳定电压保护，不改阈值；低电持续时每隔 10 分钟最多提醒一次，
  只在空闲、播放结束、非录音/非维护时播放。
- 无效 ADC 采样不会重新武装；被对话打断且**未播完**才在下次空闲重试。
  若提醒已自然播完、随后才发生重置（例如立刻进入对话），不会再重复播报——
  通过 `AudioService::GetPlaybackDrainedCount()` 区分「播完」与「被打断」。
- 长按关机仍先停互动、有限归中、切舵机电源，再用同一个 AudioService 播放关机语音；
  入队与最终 PCM 排空共 8 秒上限，超时取消并断电。等待期间每秒继续检查电池，
  临界欠压仍立即断电。
- 临界欠压分支保持无语音、无归中等待，不因播报而牺牲电池安全。

## 验证

已完成：

- 录音：恐龙 17 条、熊猫 17 条，均由官方产品页按本仓库文案生成并保存。
- 音包：`main/assets/wzh_system_voices/{dinosaur,panda}`，全部通过脚本校验
  （Ogg/Opus 单声道、时长上限、文案、哈希、来源音色）。
- CMake 选型实测：dinosaur 板取 dinosaur 包，两款熊猫取 panda 包，无重复符号。
- `python3 -m unittest discover -s scripts/tests`：**266 项通过**。
- `clang-format --dry-run -Werror` 与 `git diff --check` 通过。

构建主机依赖：`python3`、`ffmpeg`、`ffprobe`（音包校验用，仅构建期，不进固件）。

尚未完成（需你决定或上机）：

- ESP-IDF 编译与打包（按你的要求未自动执行）；开启音包后需确认 app 分区余量。
- 实机试听：音色、语速、音量、数字清晰度与尾音。
- 实机流程：配网、添加设备/验证码、升级、低电提醒、对话打断后重试、关机完整播报、
  播报期间欠压、唤醒/VAD/打断、AEC、重连，以及声音与动作不重叠。

## 复现与更新录音

```sh
# 逐项校验现有音包（不联网）
python3 scripts/audio/prepare_wzh_system_prompts.py --validate \
  --profile dinosaur --output main/assets/wzh_system_voices/dinosaur

# 用新录音重建音包（需同时提供 <key>.mp3 与 <key>.source.json）
python3 scripts/audio/prepare_wzh_system_prompts.py --profile panda --provider import \
  --source-dir /path/to/recordings --voice-id <实际音色标识> \
  --verify-official-web-source --keep-masters \
  --output main/assets/wzh_system_voices/panda
```

## 资料

- [豆包语音合成大模型](https://www.volcengine.com/product/tts)
- [官方音色列表](https://docs.volcengine.com/docs/doubao-voice/tone-list-3?lang=zh)
- [计费与试用](https://docs.volcengine.com/docs/doubao-voice/billing-overview?lang=zh)
- [小智 WebSocket 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md)
