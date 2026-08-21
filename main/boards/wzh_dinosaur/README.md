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

## 按键行为

- 模拟电源键双击：开启/关闭对话。
- 模拟电源键长按约 1.5 秒：关闭电源锁存。
- GPIO0 单击：配网状态下进入配网，正常状态下同样可切换对话。
- GPIO0 双击：执行一次 `happy` 开心动作；离线时同时尝试播放本地声音。

## 配网

开机没有已保存 WiFi 时，开启自配热点 `Dino-XXXX`，手机连接后访问
`192.168.4.1` 使用自定义配网页（扫描 WiFi、输入密码、一键连接），
不再使用小智默认配网页。凭据写入小智 WifiManager，保存后自动连接。

## 对话时音频随动

在线 TTS 播放期间，动作引擎使用 AudioService 输出的真实音频包络驱动
尾巴摆动/头部轻晃，说话随音频舞动。

## 构建

```sh
python3 scripts/build.py wzh_dinosaur --name wzh-dinosaur
```

构建成功只能验证软件集成；首次烧录后仍需检查电源锁存、电池分压、五路舵机方向和机械限位、ES8311 录放音，以及外部 Flash 默认音频。
