#include "neural_audio_frontend.h"

#include <algorithm>
#include <cstring>

bool NeuralAudioFrontend::Initialize(srmodel_list_t* models) {
    Close();
    if (models == nullptr || models->num <= 0) {
        return false;
    }
    char* ns_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, nullptr);
    char* vad_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, nullptr);
    // This framing/reset contract is specific to NSNet2, not arbitrary future models.
    if (ns_name == nullptr || std::strcmp(ns_name, "nsnet2") != 0 || vad_name == nullptr) {
        return false;
    }
    ns_iface_ = esp_nsnet_handle_from_name(ns_name);
    vad_iface_ = esp_vadn_handle_from_name(vad_name);
    if (ns_iface_ == nullptr || vad_iface_ == nullptr || ns_iface_->create == nullptr ||
        ns_iface_->destroy == nullptr || ns_iface_->process == nullptr ||
        ns_iface_->get_samp_chunksize == nullptr || ns_iface_->get_samp_rate == nullptr ||
        vad_iface_->create == nullptr || vad_iface_->destroy == nullptr ||
        vad_iface_->detect == nullptr || vad_iface_->clean == nullptr ||
        vad_iface_->get_samp_chunksize == nullptr || vad_iface_->get_samp_rate == nullptr ||
        vad_iface_->get_channel_num == nullptr) {
        Close();
        return false;
    }
    ns_data_ = ns_iface_->create(ns_name);
    vad_data_ = vad_iface_->create(vad_name, VAD_MODE_0, 1, 64, 128);
    if (!Ready() || ns_iface_->get_samp_chunksize(ns_data_) != static_cast<int>(kFrameSamples) ||
        ns_iface_->get_samp_rate(ns_data_) != 16000 ||
        vad_iface_->get_samp_chunksize(vad_data_) != static_cast<int>(kFrameSamples) ||
        vad_iface_->get_samp_rate(vad_data_) != 16000 ||
        vad_iface_->get_channel_num(vad_data_) != 1) {
        Close();
        return false;
    }
    Reset();
    return true;
}

void NeuralAudioFrontend::Close() {
    if (vad_data_ != nullptr) {
        vad_iface_->destroy(vad_data_);
    }
    if (ns_data_ != nullptr) {
        ns_iface_->destroy(ns_data_);
    }
    vad_data_ = nullptr;
    ns_data_ = nullptr;
    vad_iface_ = nullptr;
    ns_iface_ = nullptr;
    input_samples_ = 0;
    input_.fill(0);
    filtered_.fill(0);
}

void NeuralAudioFrontend::Reset() {
    input_samples_ = 0;
    if (Ready()) {
        // NSNet2 has a 1024-sample analysis window and no reset API. Advance
        // two silent hops to evict the preceding session's waveform, discard
        // both outputs, then reset VAD history. No model recreation in the task.
        for (int i = 0; i < 2; ++i) {
            input_.fill(0);
            ns_iface_->process(ns_data_, input_.data(), filtered_.data());
        }
        vad_iface_->clean(vad_data_);
    }
    input_.fill(0);
    filtered_.fill(0);
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
            const bool speech = vad_iface_->detect(vad_data_, filtered_.data()) == VAD_SPEECH;
            output(context, filtered_.data(), filtered_.size(), speech);
            input_samples_ = 0;
        }
    }
    return true;
}
