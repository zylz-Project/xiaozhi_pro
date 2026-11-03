#ifndef ECHO_REFERENCE_GUARD_H_
#define ECHO_REFERENCE_GUARD_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Called under the capture lock. Keep 512 ms of the actual 16 kHz AFE reference,
// box-filtered to 2 kHz. Search delay rather than assuming fetch/ADC alignment.
// Fixed storage and at most 905 * 120 dot-product terms per 60 ms output frame.
// This is only a heuristic veto: nonlinear/processed echo may be decorrelated.
class EchoReferenceGuard {
public:
    void Reset() {
        position_ = count_ = partial_count_ = 0;
        partial_sum_ = 0;
    }
    void Feed(const int16_t* mr, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            partial_sum_ += mr[i * 2 + 1];
            if (++partial_count_ == 8) {
                reference_[position_] = partial_sum_ / 8.0f;
                position_ = (position_ + 1) % reference_.size();
                count_ = std::min(count_ + 1, reference_.size());
                partial_count_ = 0;
                partial_sum_ = 0;
            }
        }
    }
    // A negative result means insufficient/non-audio reference. Do not treat
    // that as independent near-end speech. Absolute correlation handles phase
    // inversion and gain changes; subtracting the mean rejects DC offsets.
    float Correlation(const int16_t* pcm, size_t samples) const {
        const size_t points = std::min<size_t>(samples / 8, 120);
        if (points < 80 || count_ < points) {
            return -1.0f;
        }
        std::array<float, 120> signal;
        float mean = 0.0f;
        for (size_t i = 0; i < points; ++i) {
            int sum = 0;
            for (size_t j = 0; j < 8; ++j) {
                sum += pcm[(i * 8) + j];
            }
            signal[i] = sum / 8.0f;
            mean += signal[i];
        }
        mean /= points;
        float signal_energy = 0.0f;
        for (size_t i = 0; i < points; ++i) {
            signal[i] -= mean;
            signal_energy += signal[i] * signal[i];
        }
        if (signal_energy < points) {
            return -1.0f;
        }
        const size_t first = (position_ + reference_.size() - count_) % reference_.size();
        auto at = [this, first](size_t i) { return reference_[(first + i) % reference_.size()]; };
        const size_t newest = count_ - points;
        float sum = 0.0f, squares = 0.0f;
        for (size_t i = 0; i < points; ++i) {
            const float value = at(newest + i);
            sum += value;
            squares += value * value;
        }
        float maximum_squared = -1.0f;
        // The usual AFE delay is nearer the newest reference. A strong match
        // already vetoes echo; avoid scanning older history in that case.
        for (size_t offset = newest;; --offset) {
            const float energy = squares - sum * sum / points;
            if (energy > points * 16.0f) {
                float product = 0.0f;
                for (size_t i = 0; i < points; ++i) {
                    product += signal[i] * at(offset + i);
                }
                maximum_squared =
                    std::max(maximum_squared, product * product / (signal_energy * energy));
                if (maximum_squared >= 0.81f) {
                    return std::sqrt(std::min(maximum_squared, 1.0f));
                }
            }
            if (offset == 0) {
                break;
            }
            const float old = at(offset + points - 1), next = at(offset - 1);
            sum += next - old;
            squares += next * next - old * old;
        }
        return maximum_squared < 0 ? -1.0f : std::sqrt(std::min(maximum_squared, 1.0f));
    }

private:
    std::array<float, 1024> reference_;
    size_t position_ = 0, count_ = 0, partial_count_ = 0;
    int32_t partial_sum_ = 0;
};

#endif  // ECHO_REFERENCE_GUARD_H_
