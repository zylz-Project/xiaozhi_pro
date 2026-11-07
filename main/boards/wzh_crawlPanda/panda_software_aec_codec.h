#pragma once

#include <esp_timer.h>
#include <mutex>
#include <utility>
#include <vector>

#include "codecs/es8311_audio_codec.h"
#include "panda_echo_reference.h"

// Keeps the ES8311 hardware mono. The second AFE input is a bounded digital
// copy of the post-protection samples sent to the DAC, not a hardware ADC.
class PandaSoftwareAecCodec final : public Es8311AudioCodec {
public:
    template <typename... Args>
    explicit PandaSoftwareAecCodec(Args&&... args) : Es8311AudioCodec(std::forward<Args>(args)...) {
        input_reference_ = true;
        input_channels_ = 2;
    }

    bool InputData(std::vector<int16_t>& data) override {
        if (data.size() % 2)
            return false;
        mono_input_.resize(data.size() / 2);
        if (!AudioCodec::InputData(mono_input_))
            return false;
        for (size_t i = 0; i < mono_input_.size(); ++i)
            data[2 * i] = mono_input_[i];
        std::lock_guard<std::mutex> lock(reference_mutex_);
        if (esp_timer_get_time() - last_output_us_ > 250000)
            reference_.Clear();
        reference_.PullInterleaved(data.data(), mono_input_.size());
        return true;
    }

    void OutputData(std::vector<int16_t>& data) override {
        // This is the same speaker processing performed by AudioCodec::OutputData;
        // reference and DAC therefore see identical digital samples.
        if (speaker_protection_enabled_) {
            if (output_processing_reset_pending_.exchange(false))
                speaker_protection_.Reset();
            speaker_protection_.Process(data);
        }
        {
            std::lock_guard<std::mutex> lock(reference_mutex_);
            if (esp_timer_get_time() - last_output_us_ > 250000)
                reference_.Clear();
            reference_.Push(data.data(), data.size());
            last_output_us_ = esp_timer_get_time();
        }
        Write(data.data(), data.size());
    }

private:
    std::vector<int16_t> mono_input_;  // audio-input task only
    std::mutex reference_mutex_;
    PandaEchoReference reference_;
    int64_t last_output_us_ = 0;
};
