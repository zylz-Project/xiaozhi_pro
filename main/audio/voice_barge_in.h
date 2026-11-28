#ifndef VOICE_BARGE_IN_H_
#define VOICE_BARGE_IN_H_

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "echo_reference_guard.h"

// Called under AudioService's capture lock, on post-AEC 16 kHz mono PCM only.
// Count VAD and above-noise frames that pass the playback-correlation veto.
// The default speech budget depends on the board and interruption policy.
// Wait for a short interval of referenced, unclipped playback before arming.
class VoiceBargeIn {
public:
    uint32_t Begin([[maybe_unused]] size_t speech_gap_limit = 1920) {
        ++token_;
        monitoring_ = true;
        confirmed_ = false;
        elapsed_samples_ = speech_samples_ = speech_gap_samples_ = 0;
        noise_rms_ = 40.0f;
        residual_rms_ = 0.0f;
        correlation_ = -1.0f;
        required_speech_samples_ = kDefaultSpeechSamples;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        confirmed_at_us_ = keyword_at_us_ = -1;
        speech_gap_limit_ = std::clamp(speech_gap_limit, size_t{1920}, size_t{2880});
#endif
        echo_reference_.Reset();
        last_input_us_ = last_reference_us_ = last_clipped_us_ = -1;
        return token_;
    }
    void Cancel() {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        keyword_at_us_ = -1;
#endif
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
        if (!CandidateReferenceReady(now_us, muted)) {
            ResetCandidate();
        }
        last_input_us_ = now_us;
        if (rms > 32.0f) {
            last_reference_us_ = now_us;
        }
        if (clipped) {
            last_clipped_us_ = now_us;
        }
        if (!CandidateReferenceReady(now_us, muted)) {
            ResetCandidate();
        }
    }
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    // Pair both event orders. An acoustic candidate and keyword belong to
    // the current playback token and expire within one utterance fragment.
    uint32_t NoteKeyword(int64_t now_us, bool muted) {
        if (!monitoring_)
            return 0;
        keyword_at_us_ = now_us;
        return MatchedKeyword(now_us, muted);
    }
    uint32_t MatchedKeyword(int64_t now_us, bool muted) const {
        if (keyword_at_us_ < 0 || now_us < keyword_at_us_ ||
            now_us - keyword_at_us_ > kKeywordWaitUs ||
            !pending(token_, now_us, muted) ||
            keyword_at_us_ - confirmed_at_us_ > kNearEndAgeUs)
            return 0;
        return token_;
    }
    bool ExpireKeyword(int64_t now_us) {
        if (!monitoring_ || keyword_at_us_ < 0 || now_us - keyword_at_us_ <= kKeywordWaitUs)
            return false;
        keyword_at_us_ = -1;
        return true;
    }
    int verified_speech_ms() const { return speech_samples_ * 1000 / 16000; }
#endif
    void FeedReference(const int16_t* mr, size_t frames) {
        if (monitoring_) {
            echo_reference_.Feed(mr, frames);
        }
    }
    bool ReferenceReady(int64_t now_us, bool muted = false) const {
        return ReferenceFresh(now_us, muted, 120000);
    }
    bool Observe(const int16_t* pcm, size_t samples, bool vad_speech, int64_t now_us,
                 bool muted = false, size_t min_speech_samples = 0,
                 [[maybe_unused]] bool pair_keyword = false) {
        if (!monitoring_ || pcm == nullptr || samples == 0) {
            return false;
        }
        if (!ReferenceReady(now_us, muted)) {
            if (!CandidateReferenceReady(now_us, muted))
                ResetCandidate();
            return false;
        }
        if (confirmed_) {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
            if (confirmed_at_us_ < 0 || now_us < confirmed_at_us_ ||
                now_us - confirmed_at_us_ > (pair_keyword ? kNearEndAgeUs : 3000000)) {
                // A keyword already observed for this playback can still pair
                // with fresh audio inside its short deadline.
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
        if (elapsed_samples_ < kSettleSamples) {
            elapsed_samples_ += samples;
            speech_samples_ = speech_gap_samples_ = 0;
            return false;
        }
        if (!vad_speech) {
            noise_rms_ += (std::min(rms, 160.0f) - noise_rms_) * 0.05f;
        }
        if (!vad_speech || rms < threshold()) {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
            // Short syllable boundaries can briefly lose VAD or energy. Keep
            // the already verified near-end frames, but never count the gap as
            // speech and never let pauses accumulate without a bound.
            if (speech_samples_ != 0) {
                speech_gap_samples_ += samples;
                if (speech_gap_samples_ > speech_gap_limit_) {
                    speech_samples_ = speech_gap_samples_ = 0;
                }
            }
#else
            speech_samples_ = speech_gap_samples_ = 0;
#endif
            return false;
        }
        constexpr float echo_veto = 0.55f;
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
        correlation_ = muted ? 0.0f : echo_reference_.Correlation(pcm, samples, echo_veto);
#else
        correlation_ = muted ? 0.0f : echo_reference_.Correlation(pcm, samples);
#endif
        if (correlation_ < 0.0f) {
            ResetCandidate();
            return false;
        }
        if (correlation_ >= echo_veto) {
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
        required_speech_samples_ = std::max(kDefaultSpeechSamples, min_speech_samples);
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
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        keyword_at_us_ = -1;
#endif
        return true;
    }
    bool monitoring() const { return monitoring_; }
    bool pending(uint32_t token, int64_t now_us, bool muted = false) const {
        if (!monitoring_ || !confirmed_ || token != token_ ||
            !CandidateReferenceReady(now_us, muted))
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
    float threshold() const {
#if (CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY) && CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        return std::clamp(noise_rms_ * 2.6f, 125.0f, 420.0f);
#else
        return std::clamp(noise_rms_ * 3.0f, 160.0f, 480.0f);
#endif
    }
    float correlation() const { return correlation_; }
    int settling_ms() const { return elapsed_samples_ * 1000 / 16000; }
    int required_speech_ms() const { return required_speech_samples_ * 1000 / 16000; }

private:
    bool ReferenceFresh(int64_t now_us, bool muted, int64_t reference_age_us) const {
        const auto fresh = [now_us](int64_t time, int64_t age) {
            return time >= 0 && now_us >= time && now_us - time <= age;
        };
        return fresh(last_input_us_, 120000) &&
               (muted || fresh(last_reference_us_, reference_age_us)) &&
               !fresh(last_clipped_us_, 240000);
    }

    bool CandidateReferenceReady(int64_t now_us, bool muted) const {
#if (CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY) && CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        // A keyword can finish during a pause in the speaker audio. Keep
        // already verified speech briefly, without verifying any new audio
        // against an old reference or accepting stale/clipped capture.
        if (confirmed_)
            return ReferenceFresh(now_us, muted, 240000);
#endif
        return ReferenceReady(now_us, muted);
    }

#if (CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY) && CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    static constexpr size_t kDefaultSpeechSamples = 960;
    static constexpr size_t kSettleSamples = 1920;
#elif CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    static constexpr size_t kDefaultSpeechSamples = 1920;
    static constexpr size_t kSettleSamples = 4800;
#else
    static constexpr size_t kDefaultSpeechSamples = 2880;
    static constexpr size_t kSettleSamples = 4800;
#endif
    uint32_t token_ = 0;
    bool monitoring_ = false, confirmed_ = false;
    size_t elapsed_samples_ = 0, speech_samples_ = 0, speech_gap_samples_ = 0;
    size_t required_speech_samples_ = kDefaultSpeechSamples;
    float noise_rms_ = 40.0f;
    float residual_rms_ = 0.0f, correlation_ = -1.0f;
    EchoReferenceGuard echo_reference_;
    int64_t last_input_us_ = -1, last_reference_us_ = -1, last_clipped_us_ = -1;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
    static constexpr int64_t kKeywordWaitUs = 480000;
    static constexpr int64_t kNearEndAgeUs = 650000;
#else
    static constexpr int64_t kKeywordWaitUs = 360000;
    static constexpr int64_t kNearEndAgeUs = 550000;
#endif
    int64_t confirmed_at_us_ = -1, keyword_at_us_ = -1;
    size_t speech_gap_limit_ = 1920;
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
