# WZH Dinosaur

`wzh_dinosaur` 是基于 ESP32-S3、ES8311、五路舵机和 W25Q256 默认音频的恐龙板型。
兼容性标识和构建名按仓库规则使用 `wzh-dinosaur`（标识不允许下划线）。

## 模式

- 在线：设备处于 `connecting`、`listening`、`speaking` 或录音测试状态，即正在建立会话、听用户说话或播放回复。本地声音会暂停；服务端的 `llm.emotion` 和 `self.dinosaur.react_to_user_emotion` 负责驱动情绪动作。
- 离线：设备没有进行语音交互时，包括 Wi-Fi 已连接但处于 `idle` 待机的情况。本地自主动作继续运行，并从 W25Q256 现有 TOC 中只读播放默认 OGG/Opus 音频。

因此，“在线/离线”描述的是恐龙当前是否正在对话，而不是单纯描述 Wi-Fi 是否连接。切入在线对话时会停止并清理尚未播完的本地声音，避免干扰麦克风和服务端 TTS。

本板型不包含旧工程的固定 SSID、HTTP 上传、设备绑定、Audio Hub 清单同步或网络下载音频逻辑。外部 Flash 中没有有效 `PNDA` TOC 时，离线动作仍可运行，但不播放本地声音。

## 情绪到动作

| 模型情绪 | 动作 |
| --- | --- |
| happy / laughing / winking | happy |
| loving / embarrassed / relaxed | affection |
| sad / crying / sleepy | sleepy |
| angry / confident / cool | proud |
| surprised / shocked / fearful | startled |
| thinking / confused / curious | discover |
| neutral 或未知 | listen |

情绪识别本身发生在服务端大模型侧：固件不凭几个本地关键词假装做情绪识别，只消费模型给出的情绪结果。若要利用音色、语速等声学信息，需要服务端 ASR/情绪模型把结果提供给对话模型；设备端目前上传的是语音，不在本地运行情绪神经网络。

## 构建

```sh
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
```

构建成功只能验证软件集成；首次烧录后仍需检查电源锁存、电池分压、五路舵机方向和机械限位、ES8311 录放音，以及外部 Flash 默认音频。
