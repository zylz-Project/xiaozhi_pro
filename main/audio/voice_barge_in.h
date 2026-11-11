#ifndef VOICE_BARGE_IN_H_
#define VOICE_BARGE_IN_H_

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "echo_reference_guard.h"

// Called under AudioService's queue lock, on post-AEC 16 kHz mono PCM only.
// VAD, a bounded noise floor and a playback-correlation veto must agree for
// 180 ms, allowing one 60 ms echo-vetoed frame within that candidate. Settle
// for 300 ms of actual referenced, unclipped playback.
// Input and post-AEC observations share the audio queue lock.
class VoiceBargeIn {
public:
    uint32_t Begin() {
        ++token_;
        monitoring_ = true;
        confirmed_ = false;
        elapsed_samples_ = speech_samples_ = speech_gap_samples_ = 0;
        noise_rms_ = 40.0f;
        residual_rms_ = 0.0f;
        correlation_ = -1.0f;
        required_speech_samples_ = 2880;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        confirmed_at_us_ = -1;
#endif
        echo_reference_.Reset();
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
        if (last_input_us_ >= 0 && now_us - last_input_us_ > 120000) {
            elapsed_samples_ = 0;
            echo_reference_.Reset();
        }
        if (!ReferenceReady(now_us, muted)) {
            ResetCandidate();
        }
        last_input_us_ = now_us;
        if (rms > 32.0f) {
            last_reference_us_ = now_us;
        }
        if (clipped) {
            last_clipped_us_ = now_us;
        }
        if (!ReferenceReady(now_us, muted)) {
            ResetCandidate();
        }
    }
    void FeedReference(const int16_t* mr, size_t frames) {
        if (monitoring_) {
            echo_reference_.Feed(mr, frames);
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
                 bool muted = false, size_t min_speech_samples = 2880) {
        if (!monitoring_ || pcm == nullptr || samples == 0) {
            return false;
        }
        if (!ReferenceReady(now_us, muted)) {
            ResetCandidate();
            return false;
        }
        if (confirmed_) {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
            if (confirmed_at_us_ < 0 || now_us < confirmed_at_us_ ||
                now_us - confirmed_at_us_ > 3000000) {
                ResetCandidate();
            } else {
                return false;
            }
#else
            return false;
#endif
        }
        uint64_t square_sum = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int64_t value = pcm[i];
            square_sum += value * value;
        }
        const float rms = residual_rms_ = std::sqrt(static_cast<float>(square_sum) / samples);
        if (elapsed_samples_ < 4800) {
            elapsed_samples_ += samples;
            speech_samples_ = speech_gap_samples_ = 0;
            return false;
        }
        if (!vad_speech) {
            noise_rms_ += (std::min(rms, 160.0f) - noise_rms_) * 0.05f;
        }
        if (!vad_speech || rms < threshold()) {
            speech_samples_ = speech_gap_samples_ = 0;
            return false;
        }
        correlation_ = muted ? 0.0f : echo_reference_.Correlation(pcm, samples);
        if (correlation_ < 0.0f) {
            ResetCandidate();
            return false;
        }
        if (correlation_ >= 0.55f) {
            // A voiced near-end phoneme can briefly resemble playback. Do not
            // count that frame, but tolerate one such 60 ms gap. Sustained
            // correlated playback still discards the candidate completely.
            if (speech_samples_ != 0) {
                speech_gap_samples_ += samples;
                if (speech_gap_samples_ > 960) {
                    ResetCandidate();
                }
            }
            return false;
        }
        speech_samples_ += samples;
        required_speech_samples_ = std::max<size_t>(2880, min_speech_samples);
        if (speech_samples_ < required_speech_samples_) {
            return false;
        }
        confirmed_ = true;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        confirmed_at_us_ = now_us;
#endif
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
        if (!monitoring_ || !confirmed_ || token != token_ || !ReferenceReady(now_us, muted))
            return false;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        return confirmed_at_us_ >= 0 && now_us >= confirmed_at_us_ &&
               now_us - confirmed_at_us_ <= 3000000;
#else
        return true;
#endif
    }
    uint32_t token() const { return token_; }
    float residual_rms() const { return residual_rms_; }
    float threshold() const { return std::clamp(noise_rms_ * 3.0f, 160.0f, 480.0f); }
    float correlation() const { return correlation_; }
    int settling_ms() const { return elapsed_samples_ * 1000 / 16000; }
    int required_speech_ms() const { return required_speech_samples_ * 1000 / 16000; }

private:
    uint32_t token_ = 0;
    bool monitoring_ = false, confirmed_ = false;
    size_t elapsed_samples_ = 0, speech_samples_ = 0, speech_gap_samples_ = 0;
    size_t required_speech_samples_ = 2880;
    float noise_rms_ = 40.0f;
    float residual_rms_ = 0.0f, correlation_ = -1.0f;
    EchoReferenceGuard echo_reference_;
    int64_t last_input_us_ = -1, last_reference_us_ = -1, last_clipped_us_ = -1;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    int64_t confirmed_at_us_ = -1;
#endif

    void ResetCandidate() {
        if (confirmed_) {
            ++token_;  // Invalidate an already queued main-task event.
        }
        confirmed_ = false;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        confirmed_at_us_ = -1;
#endif
        speech_samples_ = speech_gap_samples_ = 0;
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
