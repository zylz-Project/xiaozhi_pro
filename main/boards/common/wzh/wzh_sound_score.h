#ifndef WZH_SOUND_SCORE_H_
#define WZH_SOUND_SCORE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace wzh {
enum class SoundKind : uint8_t {
    kCall,
    kAffection,
    kLaugh,
    kSearch,
    kStretch,
    kSneeze,
    kSleep,
    kChew,
    kSteps,
    kRoar,
    kAmbient
};
struct SoundScore {
    const char* name;
    SoundKind kind;
    uint32_t duration_ms;
    uint32_t seed;
    const uint8_t* envelope;
    size_t count;
};
#include "wzh_sound_scores.inc"

inline const SoundScore* FindSoundScore(const char* name) {
    for (const auto& score : kSoundScores) {
        const std::string_view n(name);
        const size_t length = std::strlen(score.name);
        if (n == score.name || (n.size() == length + 5 && n.substr(0, length) == score.name &&
                                n.substr(length) == ".opus"))
            return &score;
    }
    return nullptr;
}
inline SoundKind ClassifySoundKind(const char* name, const char* category) {
    if (category && std::strcmp(category, "ambient") == 0)
        return SoundKind::kAmbient;
    if (!name)
        return SoundKind::kCall;
    if (const auto* score = FindSoundScore(name))
        return score->kind;
    const std::string_view n(name);
    if (n.find("咀嚼") != n.npos || n.find("吃") != n.npos)
        return SoundKind::kChew;
    if (n.find("脚步") != n.npos || n.find("step") != n.npos)
        return SoundKind::kSteps;
    if (n.find("叫声音效合集") != n.npos || n.find("怒吼") != n.npos || n.find("长啸") != n.npos ||
        n.find("咆哮") != n.npos || n.find("roar") != n.npos)
        return SoundKind::kRoar;
    if (n.find("睡") != n.npos || n.find("困") != n.npos)
        return SoundKind::kSleep;
    if (n.find("喷嚏") != n.npos || n.find("sneeze") != n.npos)
        return SoundKind::kSneeze;
    if (n.find("伸懒腰") != n.npos || n.find("stretch") != n.npos)
        return SoundKind::kStretch;
    if (n.find("笑") != n.npos || n.find("laugh") != n.npos)
        return SoundKind::kLaugh;
    if (n.find("撒娇") != n.npos || n.find("亲近") != n.npos || n.find("nuzzle") != n.npos)
        return SoundKind::kAffection;
    if (n.find("寻找") != n.npos || n.find("search") != n.npos)
        return SoundKind::kSearch;
    return SoundKind::kCall;
}

// RFC 6716 TOC: return packet duration, independent of container page/byte size.
// All current pack packets are 20 or 60 ms; reject malformed/unsupported durations.
inline int OpusPacketDurationMs(const uint8_t* data, size_t size) {
    if (!data || !size)
        return 0;
    const unsigned config = data[0] >> 3;
    const unsigned code = data[0] & 3;
    const unsigned frames = code == 0 ? 1 : code < 3 ? 2 : size > 1 ? data[1] & 63 : 0;
    const unsigned samples = config >= 16        ? (120U << (config & 3))
                             : config >= 12      ? (480U << (config & 1))
                             : (config & 3) == 3 ? 2880
                                                 : (480U << (config & 3));
    const unsigned total = samples * frames;
    if (frames == 0 || total > 5760 || total % 48 != 0)
        return 0;
    const int ms = total / 48;
    return ms == 5 || ms == 10 || ms == 20 || ms == 40 || ms == 60 || ms == 80 || ms == 100 ||
                   ms == 120
               ? ms
               : 0;
}

// Selection clocks and motion read immutable metadata captured at enqueue time.
struct SoundPlayback {
    bool playing = false;
    float level = 0;
    float attack = 0;
    uint32_t elapsed_ms = 0;
    uint32_t duration_ms = 0;
    int sound_index = -1;
    const SoundScore* score = nullptr;
    SoundKind kind = SoundKind::kCall;
    uint32_t sequence = 0;
    bool audible = false;
};
}  // namespace wzh
#endif
