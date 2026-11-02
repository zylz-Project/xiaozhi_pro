#ifndef _SPEAKER_PROTECTION_H
#define _SPEAKER_PROTECTION_H

#include <algorithm>
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
    void Configure(int sample_rate);
    void Process(std::vector<int16_t>& pcm);
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

    bool configured_ = false;

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

inline void SpeakerProtection::Configure(int sample_rate) {
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

    const size_t lookahead =
        std::max<size_t>(1, static_cast<size_t>(std::lround(sample_rate * 0.001)));
    lookahead_.assign(lookahead, 0.0f);
    // Include the delayed output sample as well as the look-ahead samples.
    // The monotonic ring visits each sample at most twice, with no per-sample
    // allocation or full-window peak scan.
    peaks_.resize(lookahead + 1);
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
    mean_square_ = 0.0f;
    loudness_gain_ = 1.0f;
}

inline void SpeakerProtection::Process(std::vector<int16_t>& pcm) {
    if (!configured_ || pcm.empty()) {
        return;
    }
    for (int16_t& sample : pcm) {
        const float input = static_cast<float>(sample) * (1.0f / 32768.0f);
        const float high_passed = b0_ * input + z1_;
        z1_ = b1_ * input - a1_ * high_passed + z2_;
        z2_ = b2_ * input - a2_ * high_passed;

        const float delayed = lookahead_[position_];
        lookahead_[position_] = high_passed;
        position_ = (position_ + 1) % lookahead_.size();

        // Unsigned age subtraction also works across the 32-bit clock wrap.
        while (peak_count_ && sample_clock_ - peaks_[peak_head_].sample > lookahead_.size()) {
            peak_head_ = (peak_head_ + 1) % peaks_.size();
            --peak_count_;
        }
        const float magnitude = std::fabs(high_passed);
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

        const float out = std::clamp(delayed * gain_ * loudness_gain_, -1.0f, 1.0f);
        sample = static_cast<int16_t>(std::lrintf(out * 32767.0f));
    }
}

#endif  // _SPEAKER_PROTECTION_H
