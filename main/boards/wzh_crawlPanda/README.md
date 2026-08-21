# WZH Crawl Panda

`wzh_crawlPanda` 是基于 ESP32-S3、ES8311、三路舵机和 W25N01GV 默认音频的爬爬熊猫板型。
兼容性标识和构建名按仓库规则使用 `wzh-crawlpanda`（标识不允许下划线）。

硬件与旧工程 `crawlPanda` 一致：

- ES8311 双工音频（麦克风 + 扬声器）。
- 三路 180° 舵机：头部（IO17）、左前肢（IO15，爬行方向角度减小）、右前肢（IO16，爬行方向角度增大）。
- 模拟电源键（IO6）长按关机、电源锁存（IO7）和电池分压（IO3）。
- W25N01GVZEIG（SPI NAND，128 MB，IO10/9/47/21）存放已烧录的默认 OGG/Opus 声音。

## 与旧工程的关系

旧工程的 `main` 目录没有整体复制进 XiaoZhi。硬件能力被拆成独立板级模块，避免与
XiaoZhi 的入口、Wi-Fi、音频任务和协议实现冲突。对话（ASR/LLM/TTS）完全使用
XiaoZhi 的协议链路，动作引擎则移植自旧工程 `auto_run.cc` 的 living-motion 实现。

保留的“灵动”能力：

- 27 段关键帧动作剪辑（待机、好奇、吃竹、宝宝、骄傲、亲昵、玩耍、环境、收尾）。
- 关键帧插值 + 平滑缓动，动作连续流动、软着陆无硬停。
- 常驻微动层：呼吸、头部凝视漂移、前肢交替微摆、肌肉微颤。
- 随机镜像、随机节奏、洗牌包挑选，避免复读同一套动作。
- 本地声音播放期间按音频活跃度舞动（前肢交替扒动 + 呼吸起伏，Opus 包大小估算包络）。

不包含旧工程的固定 SSID、HTTP 上传、设备绑定、Audio Hub 清单同步或网络下载音频。

## 模式

- 在线：设备处于 `connecting`、`listening`、`speaking` 或录音测试状态。本地声音
  暂停；服务端 `llm.emotion` 和 `self.panda.react_to_user_emotion` 驱动情绪动作；
  说话期间爬爬熊猫随 TTS 轻摆。
- 离线：设备没有进行语音交互时，包括 Wi-Fi 已连接但处于 `idle` 待机的情况。
  自主 living-motion 继续运行，随机播放 W25N01GV 现有 TOC 中的动物/环境声音，并按
  声音活跃度舞动。

因此，“在线/离线”描述的是爬爬熊猫当前是否正在对话，而不是单纯描述 Wi-Fi 是否连接。
切入在线对话时会停止并清理尚未播完的本地声音，避免干扰麦克风和服务端 TTS。

## 情绪到动作

| 模型情绪 | 爬爬熊猫动作 |
| --- | --- |
| happy / laughing / winking | happy（玩耍爬行/前爪敲击） |
| loving / embarrassed / relaxed | affection（拥抱/撒娇/求抱） |
| sad / crying / sleepy | sleepy（收尾/困倦） |
| angry / confident / cool | proud（撑起/巡视/叫声） |
| surprised / shocked / fearful | startled（好奇窥探） |
| thinking / confused / curious | discover（好奇） |
| neutral 或未知 | （忽略） |

设备提供 MCP 工具：`self.panda.react_to_user_emotion`、`self.panda.action`、
`self.panda.get_mode`。作为兜底，也会对用户转写文本做轻量关键词情绪判断。

## 按键行为

- GPIO0 单击：配网状态下进入配网，正常状态下切换对话。
- GPIO0 双击：执行一次 `happy` 开心动作；离线时同时尝试播放本地声音。
- 模拟电源键长按约 1.5 秒：关闭电源锁存。

## 构建

```sh
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
```

构建成功只能验证软件集成；首次烧录后仍需检查电源锁存、电池分压、三路舵机方向和
机械限位（尤其左右前肢的爬行方向是否与注释一致）、ES8311 录放音，以及外部 Flash
NAND 只读：板级 `w25n01gv.c` 驱动负责初始化与读取，TOC 位于块 0 前 4 KiB，
音频数据从第 2 个块（偏移 128 KiB）开始逐页读取。