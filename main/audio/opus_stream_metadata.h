#ifndef OPUS_STREAM_METADATA_H_
#define OPUS_STREAM_METADATA_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Local container metadata only; never serialized into either network protocol.
struct OpusStreamMetadata {
    bool start = false;
    bool end = false;
    uint32_t skip_48k = 0;
    uint32_t keep_end_48k = UINT32_MAX;
    int16_t gain_q8 = 0;
};

inline uint32_t OpusPacketSamples48k(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return 0;
    const unsigned config = data[0] >> 3;
    constexpr unsigned kSilk[] = {480, 960, 1920, 2880};
    const unsigned frame = config < 12   ? kSilk[config & 3]
                           : config < 16 ? 480U << (config & 1)
                                         : 120U << (config & 3);
    const unsigned code = data[0] & 3;
    if ((code == 2 || code == 3) && size < 2)
        return 0;
    const unsigned count = code == 0 ? 1 : code == 3 ? data[1] & 63 : 2;
    const unsigned samples = frame * count;
    return count != 0 && samples <= 5760 ? samples : 0;
}

// Apply before resampling: Ogg Opus granules and preskip always use 48 kHz.
inline void TrimOpusPcm(std::vector<int16_t>& pcm, int rate, const OpusStreamMetadata& info) {
    if (rate <= 0) {
        pcm.clear();
        return;
    }
    const size_t first = std::min<uint64_t>(pcm.size(), uint64_t(info.skip_48k) * rate / 48000);
    const size_t end =
        info.keep_end_48k == UINT32_MAX
            ? pcm.size()
            : std::min<uint64_t>(pcm.size(), uint64_t(info.keep_end_48k) * rate / 48000);
    if (end <= first) {
        pcm.clear();
        return;
    }
    if (first != 0)
        std::move(pcm.begin() + first, pcm.begin() + end, pcm.begin());
    pcm.resize(end - first);
}

#endif
