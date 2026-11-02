#ifndef VOICE_BARGE_IN_H_
#define VOICE_BARGE_IN_H_

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Called under AudioService's queue lock, on post-AEC 16 kHz mono PCM only.
// VAD and an adaptive energy floor must agree for 180 ms. The first 240 ms
// lets playback/AEC settle. One notification is emitted per monitored reply.
class VoiceBargeIn {
public:
    uint32_t Begin() {
        ++token_;
        monitoring_ = true;
        confirmed_ = false;
        elapsed_samples_ = speech_samples_ = 0;
        noise_rms_ = 80.0f;
        return token_;
    }
    void Cancel() {
        ++token_;
        monitoring_ = confirmed_ = false;
    }
    bool Observe(const int16_t* pcm, size_t samples, bool vad_speech) {
        if (!monitoring_ || confirmed_ || pcm == nullptr || samples == 0) {
            return false;
        }
        uint64_t square_sum = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int64_t value = pcm[i];
            square_sum += value * value;
        }
        const float rms = std::sqrt(static_cast<float>(square_sum) / samples);
        elapsed_samples_ = std::min<size_t>(elapsed_samples_ + samples, 48000);
        if (!vad_speech) {
            noise_rms_ += (std::min(rms, 1000.0f) - noise_rms_) * 0.05f;
        }
        if (elapsed_samples_ <= 3840 || !vad_speech || rms < std::max(260.0f, noise_rms_ * 3.0f)) {
            speech_samples_ = 0;
            return false;
        }
        speech_samples_ += samples;
        if (speech_samples_ < 2880) {
            return false;
        }
        confirmed_ = true;
        return true;
    }
    bool Commit(uint32_t token) {
        if (!pending(token)) {
            return false;
        }
        monitoring_ = confirmed_ = false;
        return true;
    }
    bool monitoring() const { return monitoring_; }
    bool pending(uint32_t token) const { return monitoring_ && confirmed_ && token == token_; }
    uint32_t token() const { return token_; }

private:
    uint32_t token_ = 0;
    bool monitoring_ = false, confirmed_ = false;
    size_t elapsed_samples_ = 0, speech_samples_ = 0;
    float noise_rms_ = 80.0f;
};

// The current protocol has no per-reply ID. After abort, require the next
// transcript before admitting another TTS reply. Both transports share this
// gate, including early binary packets and queued start/sentence callbacks.
class BargeInReplyGate {
public:
    void Begin() { waiting_.store(true); }
    void Cancel() { waiting_.store(false); }
    void OnTranscript() { waiting_.store(false); }
    bool blocked() const { return waiting_.load(); }

private:
    std::atomic<bool> waiting_{false};
};

#endif  // VOICE_BARGE_IN_H_
