#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {}

AudioCodec::~AudioCodec() {}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    if (speaker_protection_enabled_) {
        if (output_processing_reset_pending_.exchange(false)) {
            speaker_protection_.Reset();
        }
        speaker_protection_.Process(data);
    }
    Write(data.data(), data.size());
}

void AudioCodec::ResetOutputProcessing() { output_processing_reset_pending_.store(true); }

void AudioCodec::EnableSpeakerProtection(bool enable) {
    speaker_protection_enabled_ = enable;
    if (enable) {
#if CONFIG_AUDIO_SPEAKER_VOICE_CLARITY
        speaker_protection_.Configure(output_sample_rate_, true);
        ESP_LOGI(TAG,
                 "Speaker clarity: 300 Hz -1 dB, 2.5 kHz +2 dB, makeup +0.5 dB, 5 ms startup ramp; "
                 "120 Hz high-pass, RMS guard -> 1 ms peak limiter");
#else
        speaker_protection_.Configure(output_sample_rate_);
        ESP_LOGI(TAG, "Speaker protection: 120 Hz high-pass, RMS guard, 1 ms peak limiter");
#endif
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
