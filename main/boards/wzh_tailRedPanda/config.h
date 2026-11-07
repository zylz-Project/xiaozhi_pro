#ifndef _WZH_TAILREDPANDA_CONFIG_H_
#define _WZH_TAILREDPANDA_CONFIG_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_adc/adc_oneshot.h>

// ES8311 同时服务于在线语音和离线本地声音。二者共用 XiaoZhi AudioService，
// 不允许离线模块再次安装 I2S 驱动。
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_NC
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_48
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_46
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_14
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_2
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_38
#define AUDIO_CODEC_PA_PIN GPIO_NUM_NC
// V4-2.2: NS4150 VCC is supplied from +3.3V through R36 (2 ohms).
#define AUDIO_CODEC_PA_VOLTAGE 3.3f
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO GPIO_NUM_0

// 电源锁存、模拟电源键和电池分压。
// POWER_CTRL_GPIO 拉高维持整机供电，长按电源键后拉低完成关机。
#define POWER_CTRL_GPIO GPIO_NUM_7
#define POWER_BUTTON_GPIO GPIO_NUM_6
#define POWER_BUTTON_ADC_CHANNEL ADC_CHANNEL_5
#define POWER_BUTTON_PRESS_MV 1000
#define POWER_BUTTON_RELEASE_MV 1200  // 释放 IO6 实测约 1224~1299 mV
#define POWER_MAX_SAMPLE_GAP_MS 250   // 超过此间隔视为接触状态未知
#define POWER_LONG_PRESS_MS 1500
#define POWER_DEBOUNCE_MS 50
#define POWER_POLL_MS 20
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_DIVIDER_RATIO 2
#define BATTERY_READ_INTERVAL_MS 1000  // 按时间戳采样，不随轮询耗时漂移
// 演示模式开关：1 = 续航优先，低于 3.10V 才停止舵机，3.25V 恢复，
// 2.90V 紧急断电；0 = 常规保护（3.30V / 3.40V / 3.00V）。
// 演示值低于电芯截止电压会加速老化并有掉电风险，量产前改回 0。
#define BATTERY_DEMO_MODE 0
#if BATTERY_DEMO_MODE
#define BATTERY_LOW_MV 3100
#define BATTERY_RECOVER_MV 3250
#define BATTERY_CRITICAL_MV 2900
#else
#define BATTERY_LOW_MV 3300
#define BATTERY_RECOVER_MV 3400
#define BATTERY_CRITICAL_MV 3000
#endif
#define BATTERY_EMPTY_VOLTAGE_MV 3200
#define BATTERY_FULL_VOLTAGE_MV 4200

// 原理图 V4-2.2：人体感应数字输入、SC7A20H 独立 I2C1（I2C0 留给 ES8311）。
#define HUMAN_SENSOR_GPIO GPIO_NUM_41
#define POSTURE_I2C_SDA_GPIO GPIO_NUM_43
#define POSTURE_I2C_SCL_GPIO GPIO_NUM_44

// 四路触摸电极经 4-bit R-2R 网络汇入 IO5（ADC1_CH4）。编码 bit0..bit3
// 分别对应 SW1..SW4；连续 5 个 20ms 样本相同后才确认状态变化。
#define TOUCH_GPIO GPIO_NUM_5
#define TOUCH_ADC_CHANNEL ADC_CHANNEL_4
#define TOUCH_STABLE_SAMPLES 5

// 三路小熊猫舵机。
// head:  0=向右 90=中位 180=向左
// tail_lr: 0=左 90=中位 180=右
// tail_ud: 0=上 90=中位 180=下，默认坐姿取 150
#define SERVO_POWER_GPIO GPIO_NUM_4
#define SERVO_HEAD_GPIO GPIO_NUM_18
#define SERVO_TAIL_LR_GPIO GPIO_NUM_15
#define SERVO_TAIL_UD_GPIO GPIO_NUM_16
#define SERVO_HEAD_DEFAULT 90
#define SERVO_TAIL_LR_DEFAULT 90
// 尾巴上下：默认坐姿 150°，软件范围放开到完整 0°～180°。
// 0° 为最上翘，180° 为最下垂；上电/静止/收势仍回 150°。
#define SERVO_TAIL_UD_DEFAULT 150
#define SERVO_HEAD_MIN 30
#define SERVO_HEAD_MAX 150
#define SERVO_TAIL_LR_MIN 6
#define SERVO_TAIL_LR_MAX 174
#define SERVO_TAIL_UD_MIN 0
#define SERVO_TAIL_UD_MAX 180
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_FREQ_HZ 50
#define SERVO_DUTY_RES LEDC_TIMER_13_BIT

// W25N01GVZEIG (SPI NAND, 128 MB) 中保存已经烧录好的默认声音，运行时只读。
// 前 4 KiB 存放 PNDA TOC，音频数据从 PANDA_FLASH_DATA_START 开始。
// 驱动由板级 w25n01gv.c 提供（引脚与下方宏一致：CS=10 CLK=9 MOSI=47 MISO=21）。
#define PANDA_FLASH_SIZE (128U * 1024U * 1024U)
#define PANDA_FLASH_ERASE_SIZE (128U * 1024U)  // 一个 NAND 块
#define PANDA_FLASH_DATA_START PANDA_FLASH_ERASE_SIZE
#define PANDA_FLASH_CS_PIN GPIO_NUM_10
#define PANDA_FLASH_CLK_PIN GPIO_NUM_9
#define PANDA_FLASH_MOSI_PIN GPIO_NUM_47
#define PANDA_FLASH_MISO_PIN GPIO_NUM_21

// 自定义配网热点前缀：开机没有已保存 WiFi 时，开启 "RedPanda-XXXX" 热点配网。
#define PANDA_AP_PREFIX "RedPanda"

#endif  // _WZH_TAILREDPANDA_CONFIG_H_
