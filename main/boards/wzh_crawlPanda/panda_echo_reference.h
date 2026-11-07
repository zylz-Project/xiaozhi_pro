#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Caller serializes access. Mono 24 kHz DAC samples; the fixed delay is an
// experimental DMA/acoustic alignment estimate, not a hardware measurement.
class PandaEchoReference {
public:
    static constexpr size_t kDelaySamples = 960;  // 40 ms at 24 kHz
    static constexpr size_t kCapacity = 4800;     // 200 ms maximum

    void Clear() {
        read_ = size_ = 0;
        delay_remaining_ = kDelaySamples;
    }
    void Push(const int16_t* samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (size_ == kCapacity) {
                read_ = (read_ + 1) % kCapacity;
                --size_;
            }
            data_[(read_ + size_) % kCapacity] = samples[i];
            ++size_;
        }
    }
    void PullInterleaved(int16_t* output, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            int16_t reference = 0;
            if (delay_remaining_) {
                --delay_remaining_;
            } else if (size_) {
                reference = data_[read_];
                read_ = (read_ + 1) % kCapacity;
                --size_;
            }
            output[2 * i + 1] = reference;
        }
    }

private:
    std::array<int16_t, kCapacity> data_{};
    size_t read_ = 0, size_ = 0;
    size_t delay_remaining_ = kDelaySamples;
};
