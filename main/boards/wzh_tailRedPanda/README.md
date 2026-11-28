# 摇尾小熊猫 · 星宝

板型 `wzh-tailredpanda` 使用 ESP32-S3、ES8311 和三路 180° 舵机。配网热点为 `RedPanda-XXXX`，设备页面在 `http://192.168.4.1`。待机唤醒词为“你好星宝”。[三款快速上手](../../../docs/wzh/product/family_quickstart.md)

## 尾巴与触摸

SW1 到 SW4 依次为头、背部、肚子和尾巴。默认隔离 SW4，开发者页面可查看 IO5 的采样电压并在排除故障后恢复 SW4。设置保存在本机，切换后需先确认松开再接收新触摸。其他触点按下需五个一致样本，松开需连续确认 400 ms。对话中的触摸部位在语音空档发送给当前会话。

头尾协作表演包含侧摆、抬尾、落尾和完整尾弧。普通未配套声音在 2 秒及以内可用短编舞，手调录音配套沿用录制轨迹。动作结束回到坐姿并释放舵机。[三款动作库与展示节奏](../../../docs/wzh/motion/motion_libraries.md)

## 装配

| 轴 | GPIO | 默认角 | 软件范围 | 方向 |
| --- | --- | --- | --- | --- |
| 头部 | IO18 | 90° | 30–150° | 角度增大向左 |
| 尾巴左右 | IO15 | 90° | 6–174° | 角度增大向右 |
| 尾巴上下 | IO16 | 150° | 0–180° | 角度增大向下 |

舵机电源为 IO4。尾巴上下的 150° 是坐姿中位，须核对装壳后的 0° 上翘、180° 下垂和头尾组合间隙。[硬件复核](../../../docs/wzh/hardware/hardware_review.md) · [感应与保护](../../../docs/wzh/hardware/sensing_and_protection.md)

## 构建与验收

使用 ESP-IDF v6.0.2。发布固件启用关键词确认打断和小熊猫音频设置，运行日志通过 USB Serial/JTAG 输出。

```sh
python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda
```

实机重点检查 SW4 隔离和恢复后的连续触摸、尾巴坐姿与机械间隙、`startled_flinch` 动作、带载供电以及播放时的打断表现。[产品验收表](../../../docs/wzh/product/product_acceptance.md)

## 历史实机记录

2026-11-04 用户试听反馈官方音乐音质偏低。当时的 Opus 日志记录约 27.4 kbps、单声道、CELT/SWB 12 kHz。日志未报解码失败或数字满幅，音源与扬声器的影响仍需同曲对照。[试听与诊断记录](../../../docs/wzh/archive/2026-11/wzh_tailredpanda_music_feedback_2026-11-04.md)

同日较早的人声打断候选版出现 32 ms 帧处理超时、AFE 缓冲溢出和 CPU1 看门狗告警。这组日志可用于复核新版本的实时预算。[打断实时性记录](../../../docs/wzh/archive/2026-11/wzh_tailredpanda_barge_in_realtime_2026-11-04.md)
