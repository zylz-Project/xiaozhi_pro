#include "neural_audio_frontend.h"

#include <algorithm>
#include <cstring>

bool NeuralAudioFrontend::Initialize(srmodel_list_t* models) {
    Close();
    if (models == nullptr || models->num <= 0) {
        return false;
    }
    char* ns_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, nullptr);
    // This framing/reset contract is specific to NSNet2, not arbitrary future models.
    if (ns_name == nullptr || std::strcmp(ns_name, "nsnet2") != 0) {
        return false;
    }
    ns_iface_ = esp_nsnet_handle_from_name(ns_name);
    if (ns_iface_ == nullptr || ns_iface_->create == nullptr || ns_iface_->destroy == nullptr ||
        ns_iface_->process == nullptr || ns_iface_->get_samp_chunksize == nullptr ||
        ns_iface_->get_samp_rate == nullptr) {
        Close();
        return false;
    }
    ns_data_ = ns_iface_->create(ns_name);
    // Keep NSNet2's residual-noise suppression, but avoid running a second
    // neural network on every 32 ms frame. WebRTC VAD consumes exact 10 ms blocks.
    vad_data_ = vad_create_with_param(VAD_MODE_2, 16000, 10, 60, 100);
    if (!Ready() || ns_iface_->get_samp_chunksize(ns_data_) != static_cast<int>(kFrameSamples) ||
        ns_iface_->get_samp_rate(ns_data_) != 16000) {
        Close();
        return false;
    }
    Reset();
    return true;
}

void NeuralAudioFrontend::Close() {
    if (vad_data_ != nullptr) {
        vad_destroy(vad_data_);
    }
    if (ns_data_ != nullptr) {
        ns_iface_->destroy(ns_data_);
    }
    vad_data_ = nullptr;
    ns_data_ = nullptr;
    ns_iface_ = nullptr;
    input_samples_ = vad_samples_ = 0;
    input_.fill(0);
    filtered_.fill(0);
    vad_input_.fill(0);
}

void NeuralAudioFrontend::Reset() {
    input_samples_ = vad_samples_ = 0;
    if (Ready()) {
        // NSNet2 has a 1024-sample analysis window and no reset API. Advance
        // two silent hops to evict the preceding session's waveform, discard
        // both outputs, then reset VAD trigger state. No model recreation in the task.
        for (int i = 0; i < 2; ++i) {
            input_.fill(0);
            ns_iface_->process(ns_data_, input_.data(), filtered_.data());
        }
        vad_reset_trigger(vad_data_);
    }
    input_.fill(0);
    filtered_.fill(0);
    vad_input_.fill(0);
}

bool NeuralAudioFrontend::Process(const int16_t* pcm, size_t samples, Output output,
                                  void* context) {
    if (!Ready() || pcm == nullptr || output == nullptr || samples == 0) {
        return false;
    }
    while (samples > 0) {
        const size_t count = std::min(samples, kFrameSamples - input_samples_);
        std::copy_n(pcm, count, input_.data() + input_samples_);
        input_samples_ += count;
        pcm += count;
        samples -= count;
        if (input_samples_ == kFrameSamples) {
            ns_iface_->process(ns_data_, input_.data(), filtered_.data());
            size_t offset = 0;
            while (offset < filtered_.size()) {
                const size_t take = std::min(filtered_.size() - offset, kVadSamples - vad_samples_);
                std::copy_n(filtered_.data() + offset, take, vad_input_.data() + vad_samples_);
                offset += take;
                vad_samples_ += take;
                if (vad_samples_ == kVadSamples) {
                    const bool speech =
                        vad_process_with_trigger(vad_data_, vad_input_.data()) == VAD_SPEECH;
                    output(context, vad_input_.data(), vad_input_.size(), speech);
                    vad_samples_ = 0;
                }
            }
            input_samples_ = 0;
        }
    }
    return true;
}
