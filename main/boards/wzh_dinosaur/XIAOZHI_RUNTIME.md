# 恐龙固件运行指南

## 启动与任务

`app_main` 初始化 NVS 后进入 `Application`。构建只链接一个板级工厂。恐龙构造函数建立 ES8311 的 I2C0 总线并启动电源监测，`Application::Initialize()` 启动共享 AudioService 后调用 `OnAudioServiceReady()`。此时请求开机音效，初始化本地录音目录、动作控制器、传感器和入口工具。网络随后启动。舵机运动另等电池采样和保护放行。[应用入口](../../main.cc) · [板级初始化](wzh_dinosaur.cc) · [开机电池检测](../../../docs/wzh/hardware/startup_battery_fast_path_2026-11-09.md)

| 运行位置 | 职责 |
| --- | --- |
| Application 主事件循环 | 切换设备状态、建立和关闭会话、处理协议消息与播放代次 |
| AudioService | 共享麦克风、AFE 和 Opus 编解码，管理播放和采样队列并发布扬声器 PCM 包络 |
| `dino_motion` | 处理最新动作请求，插值、限位并管理回中 |
| `dino_power` | 采样 IO6 电源键、IO3 电池与 IO5 触摸，检查舵机看门狗并负责关机 |
| `wzh_sensors` | 处理 IO41 人体感应和 SC7A20H 姿态采样，维持独立落体保护 |

驱动与传感器回调通过事件或队列向对应任务传递数据。修改状态路径时检查状态机及共享音频任务。[状态机](../../device_state_machine.cc) · [AudioService](../../audio/README.md)

## 联网与对话

恐龙保留设备热点、Wi-Fi STA 和 ML307 AT 网络。已保存 Wi-Fi 凭据和 Wi-Fi 偏好时优先 Wi-Fi，否则探测到模组先选择 4G。4G 未就绪或断线持续约 15 秒时，只有存在 Wi-Fi 凭据才自动切换并播报。用户页可以提交手动切换并查询结果。补做模组探测时仍可使用原 Wi-Fi，确认切换后先结束旧协议，再切换链路。初始引导在选用 4G 时跳过 Wi-Fi 步骤。[页面与配网](../../../docs/wzh/web/README.md) · [ML307 接线与验收](../../../docs/wzh/hardware/dinosaur_ml307_2026-11-15.md)

模组网络就绪允许激活和建立协议连接。语音服务状态在音频通道由服务端确认后置为就绪。对话在连接、聆听和回复状态间切换。普通断网恢复窗口为 20 秒，主动切网为 45 秒。恢复后重新聆听，等待时可双击取消。4G 路径由 `dinosaur_ml307_*` 提供 HTTP 与会话网络适配，Application 与两种网络共用协议语义。[WebSocket](../../../docs/websocket.md) · [MQTT/UDP](../../../docs/mqtt-udp.md)

电源键和 BOOT 键双击统一走 `Application::ToggleConversation()`，长按电源键关机。BOOT 单击只在启动或配网状态进入配网。待机唤醒、回复中的关键词打断与离线动作命令由各自音频入口处理。[语音与触摸控制](../../../docs/wzh/audio/voice_commands.md)

## 互动、声音与运动

- 在线回复的 `llm.emotion`、MCP 请求或本机互动各自转成动作或表达请求。身体命令只驱动指定关节。实际扬声器 PCM 为随动提供能量和停顿，基础姿态单独保存。 [动作控制器](dinosaur_controller.cc) · [MCP 协议](../../../docs/mcp-protocol.md)
- 自主表演先选到实际录音，再按该录音的类别、时长和配套轨迹编舞。短声的紧凑编舞、吼叫时长匹配和成对录音轨迹由动作控制器处理。外置 SPI Flash 启动时探测 NOR/NAND，音频目录兼容 PNDA。 [共享动作库](../../../docs/wzh/motion/motion_libraries.md) · [Flash 驱动](../common/wzh/wzh_flash.c)
- 触摸、姿态、语音和保护会影响动作请求。对话触摸在本轮语音空档发送部位，背部顺摸和逆摸携带方向。普通动作结束后从最后输出姿态回中并释放。保护触发时直接释放。 [硬件、触摸和角度](README.md) · [感应与保护](../../../docs/wzh/hardware/sensing_and_protection.md)

## 电源与调试

正常长按或空闲关机时停止旧互动，健康且正在通电的舵机进入有限回中，随后断开舵机电源并在告别语音后切断 IO7 锁存。严重欠压立即切电。IO3 百分比是分压估算，主板未接充电状态检测脚。 [电源任务](dinosaur_power.cc) · [运行日志说明](../../../docs/wzh/product/运行日志说明.md)

上板验收先核对 [五轴方向与 270° 校准](README.md#装配和角度)，再测装壳后的组合动作、带载电源和 ES8311 录放音。4G 弱信号重连时同时检查模组、链路和播放日志，分别记录设备联通、会话恢复、首包延迟及听感。 [产品验收表](../../../docs/wzh/product/product_acceptance.md)
