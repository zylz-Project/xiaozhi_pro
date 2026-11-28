# 恐龙 ES8311 麦克风增益

发布变体 `wzh-dinosaur-270-servo` 启用关键词确认打断和 DAC 播放参考。此时 `Es8311AudioCodec` 使用恐龙板的 `AUDIO_CODEC_REFERENCE_MIC_GAIN_DB`，当前为 12 dB。无播放参考的共用编解码器路径使用 36 dB。板级校准从 [config.h](config.h) 调整，构造及生效路径见[板级编解码器配置](wzh_dinosaur.cc)和[ES8311 封装](../../audio/codecs/es8311_audio_codec.cc)。

驱动以 6 dB 为步长向下取档，最低 0 dB，最高 42 dB。例如 12–17 dB 选择 12 dB 档。`esp_codec_dev` 的 `device/es8311/es8311.c` 包含 `es8311_set_mic_gain()` 和对应寄存器写入。

实机调整时使用相同音量分别记录近讲、远讲、播放中说话和舵机运动时的录音。检查削波、回声、唤醒和打断表现。保持 24 kHz 硬件时钟和单路 ES8311 音频服务。 [恐龙打断实机记录](../../../docs/wzh/audio/dinosaur_barge_in_research_2026-11-10.md)
