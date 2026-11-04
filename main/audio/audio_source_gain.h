#ifndef AUDIO_SOURCE_GAIN_H_
#define AUDIO_SOURCE_GAIN_H_
#include <cstddef>
#include <cstdint>

// Identifies trusted bundled assets; not a cryptographic integrity/security API.
inline uint64_t AudioSourceFingerprint(const uint8_t* data, size_t size) {
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i)
        value = (value ^ data[i]) * UINT64_C(1099511628211);
    return value;
}
#endif
