#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace voice_tts {
struct WavPcmView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t sample_rate = 0;
};

inline uint16_t WavU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

inline uint32_t WavU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Strictly accept complete mono signed-16 PCM at the expected speaker rate.
inline bool ParseWavPcm(const uint8_t* wav, size_t size, uint32_t expected_rate,
                        WavPcmView& output) {
    output = {};
    if (!wav || size < 12 || std::memcmp(wav, "RIFF", 4) || std::memcmp(wav + 8, "WAVE", 4) ||
        uint64_t(WavU32(wav + 4)) + 8 != size)
        return false;
    bool have_format = false;
    bool have_data = false;
    WavPcmView candidate;
    for (size_t offset = 12; offset < size;) {
        if (size - offset < 8)
            return false;
        const uint8_t* chunk = wav + offset;
        const uint32_t length = WavU32(chunk + 4);
        offset += 8;
        if (uint64_t(length) + (length & 1U) > size - offset)
            return false;
        if (!std::memcmp(chunk, "fmt ", 4)) {
            if (have_format || length < 16 || WavU16(wav + offset) != 1 ||
                WavU16(wav + offset + 2) != 1 || WavU32(wav + offset + 4) != expected_rate ||
                WavU32(wav + offset + 8) != expected_rate * 2 || WavU16(wav + offset + 12) != 2 ||
                WavU16(wav + offset + 14) != 16)
                return false;
            have_format = true;
            candidate.sample_rate = expected_rate;
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (have_data || !length || (length & 1U))
                return false;
            have_data = true;
            candidate.data = wav + offset;
            candidate.size = length;
        }
        offset += length + (length & 1U);
    }
    if (!have_format || !have_data)
        return false;
    output = candidate;
    return true;
}
}  // namespace voice_tts
