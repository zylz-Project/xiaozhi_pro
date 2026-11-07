#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {}

AudioCodec::~AudioCodec() {}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    if (speaker_protection_enabled_) {
        if (output_processing_reset_pending_.exchange(false)) {
            speaker_protection_.Reset();
        }
#if CONFIG_AUDIO_PRODUCT_QUALITY
        speaker_protection_.Process(data, output_stream_gain_);
#else
        speaker_protection_.Process(data);
#endif
    }
#if CONFIG_AUDIO_PRODUCT_QUALITY
    if (!data.empty())
        last_output_sample_ = data.back();
#endif
    Write(data.data(), data.size());
}

void AudioCodec::ResetOutputProcessing() { output_processing_reset_pending_.store(true); }

void AudioCodec::EnableSpeakerProtection(bool enable) {
    speaker_protection_enabled_ = enable;
    if (enable) {
#if CONFIG_AUDIO_PRODUCT_QUALITY
        finish_padding_.assign(std::max(1, output_sample_rate_ / 1000), 0);
#endif
#if CONFIG_AUDIO_SPEAKER_VOICE_CLARITY
#if CONFIG_AUDIO_SPEAKER_SMOOTH_LIMITER
        speaker_protection_.Configure(output_sample_rate_, true, true, speaker_makeup_gain_db_);
        ESP_LOGI(TAG,
                 "Speaker limiter: 1 ms FIR-smoothed attack, peak ceiling and volume retained");
#else
        speaker_protection_.Configure(output_sample_rate_, true, false, speaker_makeup_gain_db_);
#endif
        ESP_LOGI(TAG,
                 "Speaker clarity: 300 Hz -1 dB, 2.5 kHz +2 dB, makeup +0.5 dB, 5 ms startup ramp; "
                 "120 Hz high-pass, RMS guard -> 1 ms peak limiter");
#else
        speaker_protection_.Configure(output_sample_rate_, false, false, speaker_makeup_gain_db_);
        ESP_LOGI(TAG, "Speaker protection: 120 Hz high-pass, RMS guard, 1 ms peak limiter");
#endif
        if (speaker_makeup_gain_db_ > 0.0f) {
            ESP_LOGI(TAG,
                     "Speaker loudness: +%.1f dB before RMS guard and smoothed peak limiter; "
                     "DAC ceiling retained",
                     speaker_makeup_gain_db_);
        }
    }
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    // Zero is an intentional mute, including after a developer-page restart.
    output_volume_ = std::clamp(output_volume_, 0, 100);

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    if (!enable) {
        // Drop the look-ahead tail so a re-enabled DAC cannot replay stale PCM.
        ResetOutputProcessing();
    }
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

void AudioCodec::SetOutputStreamGain(int16_t gain_q8) {
#if CONFIG_AUDIO_PRODUCT_QUALITY
    if (gain_q8 != output_gain_q8_) {
        output_gain_q8_ = gain_q8;
        output_stream_gain_ = std::pow(10.0f, std::clamp(gain_q8 / 256.0f, -60.0f, 12.0f) / 20.0f);
    }
#else
    (void)gain_q8;
#endif
}

size_t AudioCodec::FinishOutput() {
#if CONFIG_AUDIO_PRODUCT_QUALITY
    if (!speaker_protection_enabled_ || !output_enabled_)
        return 0;
    size_t written = 0;
    const bool cancelled = output_processing_reset_pending_.exchange(false);
    if (!cancelled && !finish_padding_.empty()) {
        std::fill(finish_padding_.begin(), finish_padding_.end(), 0);
        speaker_protection_.Process(finish_padding_, output_stream_gain_);
        written += std::max(0, Write(finish_padding_.data(), finish_padding_.size()));
        last_output_sample_ = finish_padding_.back();
    }
    // Finish from the last actual DAC sample. Cancellation intentionally discards
    // the old look-ahead rather than flushing stale speech into the next session.
    if (last_output_sample_ != 0) {
        std::array<int16_t, 240> ramp{};
        const size_t count = std::clamp(output_sample_rate_ / 200, 2, 240);
        for (size_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(i + 1) / count;
            ramp[i] = std::lrintf(last_output_sample_ * (1.0f - t * t * (3.0f - 2.0f * t)));
        }
        written += std::max(0, Write(ramp.data(), count));
    }
    speaker_protection_.Reset();
    last_output_sample_ = 0;
    return written;
#else
    return 0;
#endif
}
