#ifndef NEURAL_AUDIO_FRONTEND_H_
#define NEURAL_AUDIO_FRONTEND_H_

#include <array>
#include <cstddef>
#include <cstdint>

// ESP-SR 2.4.x NSNet headers do not declare C linkage themselves.
extern "C" {
#include <esp_nsn_models.h>
#include <esp_vadn_models.h>
#include <model_path.h>
}

// Single owner: the AFE fetch task. The SDK models allocate only during setup;
// framing and session resets reuse these fixed buffers.
class NeuralAudioFrontend {
public:
    static constexpr size_t kFrameSamples = 512;
    using Output = void (*)(void* context, const int16_t* pcm, size_t samples, bool speech);

    NeuralAudioFrontend() = default;
    ~NeuralAudioFrontend() { Close(); }
    NeuralAudioFrontend(const NeuralAudioFrontend&) = delete;
    NeuralAudioFrontend& operator=(const NeuralAudioFrontend&) = delete;

    bool Initialize(srmodel_list_t* models);
    void Close();
    bool Ready() const { return ns_data_ != nullptr && vad_data_ != nullptr; }
    void Reset();
    bool Process(const int16_t* pcm, size_t samples, Output output, void* context);

private:
    const esp_nsn_iface_t* ns_iface_ = nullptr;
    esp_nsn_data_t* ns_data_ = nullptr;
    const esp_vadn_iface_t* vad_iface_ = nullptr;
    model_iface_data_t* vad_data_ = nullptr;
    alignas(16) std::array<int16_t, kFrameSamples> input_{};
    alignas(16) std::array<int16_t, kFrameSamples> filtered_{};
    size_t input_samples_ = 0;
};

#endif  // NEURAL_AUDIO_FRONTEND_H_
