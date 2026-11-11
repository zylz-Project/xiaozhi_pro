#ifndef _SPEAKER_PROTECTION_H
#define _SPEAKER_PROTECTION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Pre-DAC protection for small speakers: a 120 Hz high-pass removes low
// frequencies the driver cannot reproduce, a short-term RMS guard keeps dense
// music from overdriving the amp, and a 1 ms look-ahead peak limiter holds
// near-full-scale material below clipping before it reaches the codec.
class SpeakerProtection {
public:
    void Configure(int sample_rate, bool voice_clarity = false, bool smooth_limiter = false,
                   float makeup_gain_db = 0.0f);
    void Process(std::vector<int16_t>& pcm, float source_gain = 1.0f);
    void Reset();

private:
    static constexpr float kHighPassHz = 120.0f;
    // Leaves ~1.5 dB of digital headroom for the analog path and rounding.
    static constexpr float kPeakThreshold = 0.84f;
    static constexpr float kReleaseSeconds = 0.120f;
    // Music sits 8-12 dB louder than speech in short-term RMS; the guard only
    // engages on that sustained energy and leaves normal speech untouched.
    static constexpr float kLoudnessThreshold = 0.25f;  // -12 dBFS RMS
    static constexpr float kLoudnessFloor = 0.63f;      // at most -4 dB
    static constexpr float kLoudnessAttackSeconds = 0.030f;
    static constexpr float kLoudnessReleaseSeconds = 0.500f;
    static constexpr float kRmsSeconds = 0.150f;
    static constexpr float kClarityMakeupGain = 1.0592537f;  // +0.5 dB, before protection.

    bool configured_ = false;
    bool voice_clarity_ = false;
    bool smooth_limiter_ = false;
    bool loudness_makeup_ = false;
    float makeup_gain_ = 1.0f;
    // Q20 gains make the moving sum exact, with no long-run float drift.
    static constexpr uint32_t kUnityGain = 1U << 20;
    std::vector<uint32_t> gain_window_;
    uint32_t gain_sum_ = 0;
    size_t gain_position_ = 0;
    float gain_scale_ = 1.0f;
    size_t startup_samples_ = 0, startup_position_ = 0;

    struct PeakingFilter {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void Configure(int sample_rate, float hz, float gain_db, float q) {
            // Normalized RBJ peaking EQ; coefficients are computed at setup.
            constexpr double kPi = 3.14159265358979323846;
            const double w0 = 2.0 * kPi * hz / sample_rate;
            const double a = std::pow(10.0, gain_db / 40.0);
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + alpha / a;
            b0 = static_cast<float>((1.0 + alpha * a) / a0);
            b1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
            b2 = static_cast<float>((1.0 - alpha * a) / a0);
            a1 = b1;
            a2 = static_cast<float>((1.0 - alpha / a) / a0);
        }
        float Process(float input) {
            const float output = b0 * input + z1;
            z1 = b1 * input - a1 * output + z2;
            z2 = b2 * input - a2 * output;
            return output;
        }
    };
    std::array<PeakingFilter, 2> clarity_filters_;

    // Second-order Butterworth high-pass, transposed direct form II.
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 1.0f;
    float z1_ = 0.0f, z2_ = 0.0f;

    // One-millisecond delay line doubles as the peak-detection window.
    std::vector<float> lookahead_;
    size_t position_ = 0;
    struct Peak {
        uint32_t sample;
        float value;
    };
    std::vector<Peak> peaks_;
    size_t peak_head_ = 0, peak_count_ = 0;
    uint32_t sample_clock_ = 0;
    unsigned rms_countdown_ = 0;
    float loudness_target_ = 1.0f;
    float gain_ = 1.0f;
    float release_coeff_ = 0.0f;

    float mean_square_ = 0.0f;
    float loudness_gain_ = 1.0f;
    float rms_coeff_ = 0.0f;
    float loudness_attack_coeff_ = 1.0f;
    float loudness_release_coeff_ = 0.0f;
};

inline void SpeakerProtection::Configure(int sample_rate, bool voice_clarity, bool smooth_limiter,
                                         float makeup_gain_db) {
    if (sample_rate <= 0) {
        return;
    }

    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * static_cast<double>(kHighPassHz) / sample_rate;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    b0_ = static_cast<float>((1.0 + cos_w0) * 0.5 / a0);
    b1_ = static_cast<float>(-(1.0 + cos_w0) / a0);
    b2_ = b0_;
    a1_ = static_cast<float>(-2.0 * cos_w0 / a0);
    a2_ = static_cast<float>((1.0 - alpha) / a0);

    // The speech profile is opt-in; unsupported low rates keep the original
    // protection. Do not place a 2.5 kHz filter beyond the Nyquist limit.
    voice_clarity_ = voice_clarity && sample_rate >= 8000;
    loudness_makeup_ = makeup_gain_db > 0.0f && sample_rate >= 8000 && sample_rate <= 48000;
    makeup_gain_ =
        loudness_makeup_ ? std::pow(10.0f, std::clamp(makeup_gain_db, 0.0f, 21.0f) / 20.0f) : 1.0f;
    startup_samples_ =
        (voice_clarity_ || loudness_makeup_) ? static_cast<size_t>(sample_rate / 200) : 0;
    if (voice_clarity_) {
        clarity_filters_[0].Configure(sample_rate, 300.0f, -1.0f, 0.8f);
        clarity_filters_[1].Configure(sample_rate, 2500.0f, 2.0f, 0.8f);
    }

    const size_t lookahead =
        std::max<size_t>(1, static_cast<size_t>(std::lround(sample_rate * 0.001)));
    lookahead_.assign(lookahead, 0.0f);
    // Include the delayed output sample as well as the look-ahead samples.
    // The monotonic ring visits each sample at most twice, with no per-sample
    // allocation or full-window peak scan.
    peaks_.resize(lookahead + 1);
    // The peak window includes x[n-L] in every detector result from n-L to n.
    // Averaging those L+1 gains therefore stays safe for the delayed sample,
    // while spreading an attack over the existing 1 ms instead of stepping it.
    // Bound the optional Q20 sum to 49 * 2^20 for the supported PCM rates.
    smooth_limiter_ =
        ((smooth_limiter && voice_clarity_) || loudness_makeup_) && sample_rate <= 48000;
    gain_window_.assign(smooth_limiter_ ? lookahead + 1 : 0, kUnityGain);
    gain_scale_ = smooth_limiter_ ? 1.0f / (gain_window_.size() * kUnityGain) : 1.0f;
    release_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kReleaseSeconds * sample_rate)));
    rms_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kRmsSeconds * sample_rate)));
    loudness_attack_coeff_ =
        static_cast<float>(1.0 - std::exp(-1.0 / (kLoudnessAttackSeconds * sample_rate)));
    loudness_release_coeff_ =
        static_cast<float>(1.0 - std::exp(-1.0 / (kLoudnessReleaseSeconds * sample_rate)));
    configured_ = true;
    Reset();
}

inline void SpeakerProtection::Reset() {
    z1_ = z2_ = 0.0f;
    std::fill(lookahead_.begin(), lookahead_.end(), 0.0f);
    position_ = 0;
    peak_head_ = peak_count_ = sample_clock_ = rms_countdown_ = 0;
    loudness_target_ = 1.0f;
    gain_ = 1.0f;
    std::fill(gain_window_.begin(), gain_window_.end(), kUnityGain);
    gain_sum_ = static_cast<uint32_t>(gain_window_.size()) * kUnityGain;
    gain_position_ = 0;
    mean_square_ = 0.0f;
    loudness_gain_ = 1.0f;
    startup_position_ = 0;
    for (auto& filter : clarity_filters_) {
        filter.z1 = filter.z2 = 0.0f;
    }
}

inline void SpeakerProtection::Process(std::vector<int16_t>& pcm, float source_gain) {
    if (!configured_ || pcm.empty()) {
        return;
    }
    for (int16_t& sample : pcm) {
        const float input = static_cast<float>(sample) * (source_gain / 32768.0f);
        float high_passed = b0_ * input + z1_;
        z1_ = b1_ * input - a1_ * high_passed + z2_;
        z2_ = b2_ * input - a2_ * high_passed;

        if (voice_clarity_) {
            for (auto& filter : clarity_filters_) {
                high_passed = filter.Process(high_passed);
            }
            high_passed *= kClarityMakeupGain;
        }
        // Constant pre-limiter makeup fills unused headroom without a rising
        // silence/noise gain. Both RMS control and peak limiting see this gain.
        high_passed *= makeup_gain_;

        mean_square_ += (high_passed * high_passed - mean_square_) * rms_coeff_;
        if (rms_countdown_ == 0) {
            loudness_target_ =
                mean_square_ > kLoudnessThreshold * kLoudnessThreshold
                    ? std::max(kLoudnessFloor, kLoudnessThreshold / std::sqrt(mean_square_))
                    : 1.0f;
            rms_countdown_ = 16;
        }
        --rms_countdown_;
        loudness_gain_ +=
            (loudness_target_ - loudness_gain_) *
            (loudness_target_ < loudness_gain_ ? loudness_attack_coeff_ : loudness_release_coeff_);

        // Limit the signal that will actually reach the DAC. Applying two
        // independent gains to the original peaks wastes loudness headroom.
        // Keep the original topology for boards without the opt-in profile.
        const bool rms_before_limiter = voice_clarity_ || loudness_makeup_;
        const float limiter_input = rms_before_limiter ? high_passed * loudness_gain_ : high_passed;

        const float delayed = lookahead_[position_];
        lookahead_[position_] = limiter_input;
        position_ = (position_ + 1) % lookahead_.size();

        // Unsigned age subtraction also works across the 32-bit clock wrap.
        while (peak_count_ && sample_clock_ - peaks_[peak_head_].sample > lookahead_.size()) {
            peak_head_ = (peak_head_ + 1) % peaks_.size();
            --peak_count_;
        }
        const float magnitude = std::fabs(limiter_input);
        while (peak_count_ &&
               peaks_[(peak_head_ + peak_count_ - 1) % peaks_.size()].value <= magnitude) {
            --peak_count_;
        }
        peaks_[(peak_head_ + peak_count_) % peaks_.size()] = {sample_clock_++, magnitude};
        ++peak_count_;
        const float peak = peaks_[peak_head_].value;
        const float target = peak > kPeakThreshold ? kPeakThreshold / peak : 1.0f;
        // Apply attenuation before a transient exits the delay, then release
        // smoothly. Keeping the delayed sample in the peak ring avoids overshoot.
        gain_ = std::min(target, gain_ + (1.0f - gain_) * release_coeff_);

        float output_gain = gain_;
        if (smooth_limiter_) {
            // Round down so quantization cannot raise the safe gain envelope.
            const uint32_t quantized = static_cast<uint32_t>(gain_ * kUnityGain);
            gain_sum_ -= gain_window_[gain_position_];
            gain_sum_ += quantized;
            gain_window_[gain_position_] = quantized;
            gain_position_ = (gain_position_ + 1) % gain_window_.size();
            output_gain = static_cast<float>(gain_sum_) * gain_scale_;
        }
        float out = delayed * output_gain * (rms_before_limiter ? 1.0f : loudness_gain_);
        if (startup_position_ < startup_samples_) {
            // Start the smoothstep on the first delayed sample, rather than
            // consuming the ramp while look-ahead is still emitting zeros.
            const float t = static_cast<float>(startup_position_) / (startup_samples_ - 1);
            out *= t * t * (3.0f - 2.0f * t);
            if (sample_clock_ > lookahead_.size()) {
                ++startup_position_;
            }
        }
        // The FIR is bounded analytically; cap float rounding at the same
        // existing ceiling rather than permitting a one-sample overshoot.
        const float ceiling = smooth_limiter_ ? kPeakThreshold : 1.0f;
        out = std::clamp(out, -ceiling, ceiling);
        sample = static_cast<int16_t>(std::lrintf(out * 32767.0f));
    }
}

#endif  // _SPEAKER_PROTECTION_H
