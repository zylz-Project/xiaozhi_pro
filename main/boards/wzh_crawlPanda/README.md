# 爬爬大熊猫 · 小安

板型 `wzh-crawlpanda` 使用 ESP32-S3、ES8311 和三路 180° 舵机。配网热点为 `CrawlPanda-XXXX`，设备页面在 `http://192.168.4.1`。待机唤醒词为“你好小安”。[三款快速上手](../../../docs/wzh/product/family_quickstart.md)

## 触摸与动作

SW1 到 SW4 依次为头、肚子、背部前侧、背部后侧。四触点经 IO5 的 R-2R 电阻网络采样，按下需要五个一致样本，松开需连续确认 400 ms。对话中的触摸部位在语音空档发送给当前会话。

前爪支持交替爬行和同步爬行。普通未配套的短录音使用紧凑编舞，剩余时长最多 2.6 秒。较长脚步声可以使用完整步态，手调录音配套沿用录制轨迹。待机表演按当前展示节奏运行，动作结束后回中并释放舵机。[三款动作库与展示节奏](../../../docs/wzh/motion/motion_libraries.md)

## 装配

| 轴 | GPIO | 默认角 | 软件范围 | 方向 |
| --- | --- | --- | --- | --- |
| 头部 | IO17 | 90° | 30–150° | 角度增大向左 |
| 左前爪 | IO15 | 90° | 8–172° | 爬行时角度减小 |
| 右前爪 | IO16 | 90° | 8–172° | 爬行时角度增大 |

舵机电源为 IO4。开机电量确认后才允许动作。共享主板接线、触摸电压、欠压保护和 SC7A20H 姿态调试见[硬件复核](../../../docs/wzh/hardware/hardware_review.md)和[感应与保护](../../../docs/wzh/hardware/sensing_and_protection.md)。

## 构建与验收

使用 ESP-IDF v6.0.2。发布固件启用关键词确认打断，运行日志通过 USB Serial/JTAG 输出。

```sh
python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda
```

实机重点检查两只前爪的方向、交替与同步爬行的牵引力、头爪组合间隙以及舵机带载时的供电。通用验收项包括 IO5 单点和组合触摸、开关机、Wi-Fi 对话、ES8311 录放音和外置 Flash 播放。[产品验收表](../../../docs/wzh/product/product_acceptance.md)
