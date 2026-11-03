#ifndef VOICE_BARGE_IN_H_
#define VOICE_BARGE_IN_H_

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Called under AudioService's queue lock, on post-AEC 16 kHz mono PCM only.
// VAD and an adaptive energy floor must agree for 180 ms. Calibrate residual
// echo for 600 ms of actual referenced playback, rather than from TTS start.
// Input and post-AEC observations share the audio queue lock.
class VoiceBargeIn {
public:
    uint32_t Begin() {
        ++token_;
        monitoring_ = true;
        confirmed_ = false;
        elapsed_samples_ = speech_samples_ = 0;
        noise_rms_ = 80.0f;
        echo_rms_ = residual_rms_ = 0.0f;
        last_input_us_ = last_reference_us_ = last_clipped_us_ = -1;
        return token_;
    }
    void Cancel() {
        ++token_;
        monitoring_ = confirmed_ = false;
    }
    void UpdateReference(float rms, bool clipped, int64_t now_us, bool muted = false) {
        if (!monitoring_) {
            return;
        }
        if (!ReferenceReady(now_us, muted)) {
            ResetEvidence();
        }
        last_input_us_ = now_us;
        if (rms > 32.0f) {
            last_reference_us_ = now_us;
        }
        if (clipped) {
            last_clipped_us_ = now_us;
        }
        if (!ReferenceReady(now_us, muted)) {
            ResetEvidence();
        }
    }
    bool ReferenceReady(int64_t now_us, bool muted = false) const {
        // Never let a single historical spike arm the rest of a reply. Allow
        // short phoneme gaps, but fail closed on lost/stalled input or clipping.
        const auto fresh = [now_us](int64_t time, int64_t age) {
            return time >= 0 && now_us >= time && now_us - time <= age;
        };
        return fresh(last_input_us_, 120000) && (muted || fresh(last_reference_us_, 120000)) &&
               !fresh(last_clipped_us_, 240000);
    }
    bool Observe(const int16_t* pcm, size_t samples, bool vad_speech, int64_t now_us,
                 bool muted = false) {
        if (!monitoring_ || pcm == nullptr || samples == 0) {
            return false;
        }
        if (!ReferenceReady(now_us, muted)) {
            ResetEvidence();
            return false;
        }
        if (confirmed_) {
            return false;
        }
        uint64_t square_sum = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int64_t value = pcm[i];
            square_sum += value * value;
        }
        const float rms = residual_rms_ = std::sqrt(static_cast<float>(square_sum) / samples);
        if (elapsed_samples_ < 9600) {
            elapsed_samples_ += samples;
            if (!muted) {
                echo_rms_ = std::max(echo_rms_, rms);
            }
            speech_samples_ = 0;
            return false;
        }
        if (!vad_speech) {
            noise_rms_ += (std::min(rms, 1000.0f) - noise_rms_) * 0.05f;
        }
        if (!vad_speech || rms < threshold()) {
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
    bool Commit(uint32_t token, int64_t now_us, bool muted = false) {
        if (!pending(token, now_us, muted)) {
            return false;
        }
        monitoring_ = confirmed_ = false;
        return true;
    }
    bool monitoring() const { return monitoring_; }
    bool pending(uint32_t token, int64_t now_us, bool muted = false) const {
        return monitoring_ && confirmed_ && token == token_ && ReferenceReady(now_us, muted);
    }
    uint32_t token() const { return token_; }
    float residual_rms() const { return residual_rms_; }
    float threshold() const { return std::max({260.0f, noise_rms_ * 3.0f, echo_rms_ * 2.5f}); }

private:
    uint32_t token_ = 0;
    bool monitoring_ = false, confirmed_ = false;
    size_t elapsed_samples_ = 0, speech_samples_ = 0;
    float noise_rms_ = 80.0f;
    float echo_rms_ = 0.0f, residual_rms_ = 0.0f;
    int64_t last_input_us_ = -1, last_reference_us_ = -1, last_clipped_us_ = -1;

    void ResetEvidence() {
        if (confirmed_) {
            ++token_;  // Invalidate an already queued main-task event.
        }
        confirmed_ = false;
        elapsed_samples_ = speech_samples_ = 0;
        echo_rms_ = 0.0f;
    }
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
