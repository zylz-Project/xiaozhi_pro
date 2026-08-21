# WZH Tail Red Panda

`wzh_tailRedPanda` 是基于 ESP32-S3、ES8311、三路舵机和 W25Q256 默认音频的小熊猫板型。
兼容性标识和构建名按仓库规则使用 `wzh-tailredpanda`（标识不允许下划线）。

硬件与小熊猫原工程 `tailRedPanda` 一致：

- ES8311 双工音频（麦克风 + 扬声器）。
- 三路 180° 舵机：头部（IO18）、尾巴左右（IO15）、尾巴上下（IO16）。
- 模拟电源键（IO6）长按关机、电源锁存（IO7）和电池分压（IO3）。
- W25N01GVZEIG（SPI NAND，128 MB，IO10/9/47/21）存放已烧录的默认 OGG/Opus 声音。

## 与旧工程的关系

旧工程的 `main` 目录没有整体复制进 XiaoZhi。硬件能力被拆成独立板级模块，避免与
XiaoZhi 的入口、Wi-Fi、音频任务和协议实现冲突。对话（ASR/LLM/TTS）完全使用
XiaoZhi 的协议链路，动作引擎则移植自旧工程 `auto_run.cc` 的 living-motion 实现。

保留的“灵动”能力：

- 28 段关键帧动作剪辑（好奇、吃、宝宝、骄傲、亲昵、玩耍、环境、收尾、惊起）。
- 连续目标流 + 指数追踪，动作连续流动、软着陆无硬停。
- 常驻微动层：呼吸、头部凝视漂移、尾巴轻摆、肌肉微颤。
- 随机镜像、随机节奏、洗牌包挑选，避免复读同一套动作。
- 本地声音播放期间按音频活跃度舞动（Opus 包大小估算包络）。

不包含旧工程的固定 SSID、HTTP 上传、设备绑定、Audio Hub 清单同步或网络下载音频。
NAND 只读：板级 `w25n01gv.c` 驱动负责初始化与读取，TOC 位于块 0 前 4 KiB，
音频数据从第 2 个块（偏移 128 KiB）开始，逐页/逐块读取。

## 模式

- 在线：设备处于 `connecting`、`listening`、`speaking` 或录音测试状态。本地声音
  暂停；服务端 `llm.emotion` 和 `self.panda.react_to_user_emotion` 驱动情绪动作；
  说话期间小熊猫随 TTS 轻摆。
- 离线：设备没有进行语音交互时，包括 Wi-Fi 已连接但处于 `idle` 待机的情况。
  自主 living-motion 继续运行，随机播放 W25Q256 现有 TOC 中的动物/环境声音，并按
  声音活跃度舞动。

因此，“在线/离线”描述的是小熊猫当前是否正在对话，而不是单纯描述 Wi-Fi 是否连接。
切入在线对话时会停止并清理尚未播完的本地声音，避免干扰麦克风和服务端 TTS。

## 情绪到动作

| 模型情绪 | 小熊猫动作 |
| --- | --- |
| happy / laughing / winking | happy（玩耍） |
| loving / embarrassed / relaxed | affection（亲昵） |
| sad / crying / sleepy | sleepy（收尾/困倦） |
| angry / confident / cool | proud（骄傲） |
| surprised / shocked / fearful | startled（惊起） |
| thinking / confused / curious | discover（好奇） |
| neutral 或未知 | （忽略） |

设备提供 MCP 工具：`self.panda.react_to_user_emotion`、`self.panda.action`、
`self.panda.get_mode`。作为兜底，也会对用户转写文本做轻量关键词情绪判断。

## 按键行为

- 模拟电源键双击：开启/关闭对话（与旧工程一致，双击电源键切换聊天）。
- 模拟电源键长按约 1.5 秒：关闭电源锁存。
- GPIO0 单击：配网状态下进入配网，正常状态下同样可切换对话。
- GPIO0 双击：执行一次 `happy` 开心动作；离线时同时尝试播放本地声音。

## 配网

开机没有已保存 WiFi 时，开启自配热点 `Panda-XXXX`，手机连接后访问
`192.168.4.1` 使用自定义配网页（扫描 WiFi、输入密码、一键连接），
不再使用小智默认配网页。凭据写入小智 WifiManager，保存后自动连接。

## 对话时音频随动

在线 TTS 播放期间，动作引擎使用 XiaoZhi AudioService 输出的真实音频包络
驱动尾巴/头部舞动（说话越响摆得越欢），不再使用固定模拟电平。

## 构建

```sh
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda
```

构建成功只能验证软件集成；首次烧录后仍需检查电源锁存、电池分压、三路舵机方向和
机械限位、ES8311 录放音，以及外部 Flash 默认音频。`startled_flinch` 剪辑为新增
编舞，需在真机上确认幅度不会超出机械限位。