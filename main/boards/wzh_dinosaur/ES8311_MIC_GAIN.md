# ES8311 麦克风增益参考

## 有效范围（es8311_mic_gain_t 枚举）

定义位置：[managed_components/espressif__esp_codec_dev/device/es8311/es8311_reg.h:82-93](../../managed_components/espressif__esp_codec_dev/device/es8311/es8311_reg.h#L82-L93)

| 档位 | 枚举常量 | dB |
|------|------|------|
| 0 | `ES8311_MIC_GAIN_0DB`  | **0 dB** |
| 1 | `ES8311_MIC_GAIN_6DB`  | **6 dB** |
| 2 | `ES8311_MIC_GAIN_12DB` | **12 dB** |
| 3 | `ES8311_MIC_GAIN_18DB` | **18 dB** |
| 4 | `ES8311_MIC_GAIN_24DB` | **24 dB** |
| 5 | `ES8311_MIC_GAIN_30DB` | **30 dB** |
| 6 | `ES8311_MIC_GAIN_36DB` | **36 dB** |
| 7 | `ES8311_MIC_GAIN_42DB` | **42 dB** |

## 取值逻辑（向下取档）

`esp_codec_dev_set_in_gain()` 内部实现位于 [es8311.c:415-442](../../managed_components/espressif__esp_codec_dev/device/es8311/es8311.c#L415-L442)，按区间向下取整：

| 传参 `db` | 实际写入寄存器 |
|------|------|
| `< 6` | **0 dB** |
| `≥6, <12` | **6 dB** |
| `≥12, <18` | **12 dB** |
| `≥18, <24` | **18 dB** |
| `≥24, <30` | **24 dB** |
| `≥30, <36` | **30 dB** |
| `≥36, <42` | **36 dB** |
| `≥42` | **42 dB** （上限） |

## 当前项目增益

| 文件 | 值 | 实际生效 |
|------|------|------|
| [es8311_audio_codec.cc:19](../audio/codecs/es8311_audio_codec.cc#L19) | `input_gain_ = 30` | **30 dB** |

## 修改指南

直接改 [es8311_audio_codec.cc:19](../audio/codecs/es8311_audio_codec.cc#L19) 中的 `input_gain_` 值：

- **30** → 太小，改 36（下一档 +6dB）
- **36** → 还小，改 42（最大增益）
- **42** → 就是上限，破音需要回流硬件
