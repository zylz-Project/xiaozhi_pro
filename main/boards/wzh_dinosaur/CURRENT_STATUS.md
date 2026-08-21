# WZH Dinosaur 当前实现状态

本文说明 `wzh_dinosaur` 板型目前已经实现的功能、设备开机后的实际行为，以及仍需真机验证的部分。

## 当前形态

当前版本是一套无屏幕、五舵机、支持本地默认音频，并能根据用户情绪做出身体动作的恐龙固件。

它没有把旧工程的 `main` 目录整体复制进 XiaoZhi。旧工程中的硬件能力被放进独立板级模块，避免与 XiaoZhi 的入口、Wi-Fi、音频任务和协议实现冲突。

当前包含：

- ES8311 麦克风输入和扬声器输出。
- W25Q256 只读默认音频。
- 头部转动、颈部俯仰、颈部侧倾、尾巴上下和尾巴左右五路舵机。
- 电源锁存、电池电压检测和长按关机。
- 在线对话及大模型情绪动作。
- 离线自主动作及本地声音。

当前不包含：

- 固定 Wi-Fi 名称和密码。
- HTTP 文件上传服务。
- Audio Hub 清单同步。
- 从网络下载或更新音频。
- 旧工程自己的 `app_main` 和独立 I2S 播放任务。

## 开机后的行为

设备开机后依次初始化 I2C、ES8311、电源检测、W25Q256 和五路舵机。

舵机回到默认位置后，恐龙先执行一次 `discover` 探索动作。空闲时头、颈和尾巴会做幅度较小的连续摆动，避免保持完全静止。

按键行为：

- GPIO0 单击：配网状态下进入配网，正常状态下切换对话状态。
- GPIO0 双击：执行一次 `happy` 开心动作；离线时同时尝试播放本地声音。
- 模拟电源键长按约 1.5 秒：关闭电源锁存。

## 离线模式

设备没有进行语音交互时进入离线模式。即使 Wi-Fi 已连接，只要设备处于 `idle` 待机、没有听用户说话或播放回复，也按离线模式运行。Wi-Fi 断开和配网状态同样属于离线模式。

离线状态下，恐龙会保持空闲小动作，并每隔约 7～16 秒随机执行一个动作。可选动作包括：

- `discover`：探索和观察。
- `affection`：亲近和撒娇。
- `happy`：开心摆动。
- `proud`：昂头和展示。
- `eat`：重复进食动作。
- `listen`：左右倾听。
- `sleepy`：低头困倦。

离线声音直接从 W25Q256 中已有的 `PNDA` TOC 读取。固件支持现有版本 1 和版本 2 的 TOC，并通过 XiaoZhi 的音频解码队列播放 OGG/Opus 数据，不会联网下载音频。

声音会优先根据文件名和动作匹配，例如：

| 动作 | 优先匹配的名称 |
| --- | --- |
| eat | 咀嚼、eat、chew |
| listen | 脚步、step、foot |
| sleepy | 入睡、sleep |
| startled | 警觉、startle |
| happy | 雀跃、happy、joy |
| affection | 亲近、comfort、nuzzle |
| proud | 叫声、call、roar |

如果外部 Flash 中没有有效 TOC，恐龙仍会正常做离线动作，但不播放本地声音。

## 在线模式

设备进入 `connecting`、`listening`、`speaking` 或录音测试状态时才进入在线模式。语音录制、ASR、对话和 TTS 继续使用 XiaoZhi 原有流程。

切入在线状态时会停止并清理正在播放或等待播放的 W25Q256 本地声音，避免干扰麦克风或与服务端 TTS 同时占用扬声器。身体动作由服务端情绪事件或大模型 MCP 调用驱动。对话结束回到 `idle` 后立即恢复离线自主模式。

设备提供以下 MCP 工具：

- `self.dinosaur.react_to_user_emotion`：根据用户当前情绪执行身体动作。
- `self.dinosaur.action`：执行指定动作。
- `self.dinosaur.get_mode`：返回当前 `online` 或 `offline` 模式。

设备收到 XiaoZhi 的 `llm.emotion` 后也会自动执行对应动作。

## 情绪与动作映射

| 用户或模型情绪 | 恐龙动作 |
| --- | --- |
| happy、laughing、winking、funny、excited、delighted、joyful、proud | happy |
| loving、embarrassed、relaxed、grateful、gratitude、love | affection |
| sad、crying、sleepy、tired、bored、depressed、lonely | sleepy |
| angry、confident、cool、annoyed、frustrated | proud |
| surprised、shocked、fearful、scared、worried | startled |
| thinking、confused、curious | discover |
| neutral、normal 或未知情绪（不触发动作） | （忽略） |

## 情绪如何由大模型感知

当前的主要判断链路是：

1. 用户语音上传到服务端并转换为文字。
2. 大模型结合当前文字、前后文和对话状态判断用户情绪。
3. 用户表现出明显情绪时，大模型调用 `self.dinosaur.react_to_user_emotion`。
4. 设备把情绪转换成五路舵机动作。

设备端没有用少量关键词假装完成完整的情绪识别。仅靠文字能够判断大部分语义情绪，但无法可靠区分反讽、哭腔、紧张语速或低落音色。作为兜底，设备会对用户的转写文本做一次轻量关键词匹配（如“满分/太棒/好困/吓死”），让明显的情绪也能在服务端情绪链路缺省时立刻触发动作；同时 `llm.emotion=neutral` 这种界面复位信号会被忽略，避免说完话后恐龙莫名做一个“听”动作。

如果后续希望利用声学情绪，推荐在服务端增加语音情绪模型，提取音高、能量、语速和声音类别，将分析结果连同 ASR 文本一起提供给大模型。设备端协议和动作接口不需要因此改变。

## 当前动作实现

当前使用关键帧编舞引擎，8 类动作各有 1~2 套编舞（discover/affection/happy 各两套变体）：

- discover
- affection
- happy
- proud
- eat
- listen
- startled
- sleepy

借鉴旧 dinosaur 工程的动作节奏，但按本板角度语义重新编舞：

- **KEEP 轴级联**：关键帧中 `kKeep` 表示该轴沿用上一帧，形成“头先动、脖子后随、尾巴最后”的动物时序。
- **三种缓动**：`kSmooth`（三次平滑）、`kFastOut`（快速冲出后减速，适合探头/甩尾）、`kLinear`（保持姿态）。
- **随机变奏**：每次表演随机镜像方向、幅度（0.92~1.10）和节奏（0.90~1.15），避免复读同一套动作。
- **动作中呼吸微动**：关键帧插值过程中叠加互质频率的小幅正弦，让表演中的恐龙仍然“活着”。
- **待机生命脉冲**：静止时每 5~14 秒随机插入一次转头、甩尾、嗅闻或精神一振，打破待机正弦的可预测性。
- 每个动作先短促“看向用户/食物”建立互动，结束时自然收势回默认姿态，不再硬性定格。
- 离线随机动作间隔缩短到 5~11 秒，且权重偏向活泼（discover/affection/happy）。

角度均为本板舵机直接角度（tilt 0=最高 70=默认 180=最低；tail_ud 55=最低 90=默认 180=最高）。增加动作时应先根据真机方向和机械限位校准角度。

## 构建状态

板型目录名称为 `wzh_dinosaur`。因为仓库中的兼容性标识不允许下划线，对外构建名和上报名称使用 `wzh-dinosaur`。

```sh
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
```

当前版本已经完成：

- 53 项构建脚本主机测试。
- ESP32-S3 编译和链接。
- 单一 `DECLARE_BOARD(WzhDinosaur)` 工厂检查。
- C/C++ 格式检查。

## 真机待验证项

软件构建成功不能代替硬件验证。首次烧录后需要依次检查：

1. 五路舵机的实际方向。
2. 每个动作是否超过机械限位或发生结构碰撞。
3. 舵机电源开启后是否造成 ESP32-S3 掉电或复位。
4. ES8311 麦克风录音和扬声器播放。
5. W25Q256 中已有 TOC、地址和默认音频是否与固件读取格式一致。
6. 电池分压计算和实际电量显示。
7. 长按关机和电源锁存逻辑。
8. 在线 TTS 与动作是否同步，以及切换在线/离线时是否符合预期。
