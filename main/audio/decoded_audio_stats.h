#ifndef DECODED_AUDIO_STATS_H_
#define DECODED_AUDIO_STATS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

// Codec-task-owned diagnostics only. Call Observe after successful mono Opus
// decoding, before resampling/EQ. TOC describes coded bandwidth, not source
// fidelity. No packet contents or user audio are retained.
class DecodedAudioStats {
public:
    struct Report {
        int sample_rate = 0;
        uint32_t packets = 0, stereo_packets = 0;
        uint32_t silk_packets = 0, hybrid_packets = 0, celt_packets = 0;
        uint32_t minimum_band_khz = 20, maximum_band_khz = 0;
        uint32_t peak = 0, rail_samples = 0, decode_failures = 0;
        uint64_t samples = 0, bytes = 0;
        uint32_t PayloadBitsPerSecond() const {
            return samples == 0 ? 0 : bytes * 8 * sample_rate / samples;
        }
    };

    void Failed(uint32_t generation) {
        SetGeneration(generation);
        ++current_.decode_failures;
    }
    std::optional<Report> Observe(uint32_t generation, const uint8_t* payload, size_t bytes,
                                  const int16_t* pcm, size_t samples, int sample_rate) {
        SetGeneration(generation);
        if (payload == nullptr || bytes == 0 || pcm == nullptr || samples == 0 ||
            sample_rate < 8000 || sample_rate > 48000) {
            return std::nullopt;
        }
        if (current_.sample_rate != 0 && current_.sample_rate != sample_rate) {
            current_ = {};
        }
        current_.sample_rate = sample_rate;
        ++current_.packets;
        current_.bytes += bytes;
        current_.samples += samples;
        current_.stereo_packets += (payload[0] & 4) != 0;
        // RFC 6716, section 3.1, Table 2. All 32 TOC configurations.
        const unsigned config = payload[0] >> 3;
        uint32_t band;
        if (config < 12) {
            constexpr unsigned kSilkBands[] = {4, 6, 8};
            band = kSilkBands[config / 4];
            ++current_.silk_packets;
        } else if (config < 16) {
            band = config < 14 ? 12 : 20;
            ++current_.hybrid_packets;
        } else {
            constexpr unsigned kCeltBands[] = {4, 8, 12, 20};
            band = kCeltBands[(config - 16) / 4];
            ++current_.celt_packets;
        }
        current_.minimum_band_khz = std::min(current_.minimum_band_khz, band);
        current_.maximum_band_khz = std::max(current_.maximum_band_khz, band);
        for (size_t i = 0; i < samples; ++i) {
            const int32_t value = pcm[i];
            const uint32_t magnitude = value < 0 ? -value : value;
            current_.peak = std::max(current_.peak, magnitude);
            current_.rail_samples += value == -32768 || value == 32767;
        }
        if (current_.samples < static_cast<uint64_t>(sample_rate) * 3) {
            return std::nullopt;
        }
        const Report report = current_;
        current_ = {};
        return report;
    }

private:
    bool initialized_ = false;
    uint32_t generation_ = 0;
    Report current_;
    void SetGeneration(uint32_t generation) {
        if (!initialized_ || generation_ != generation) {
            initialized_ = true;
            generation_ = generation;
            current_ = {};
        }
    }
};

#endif  // DECODED_AUDIO_STATS_H_
