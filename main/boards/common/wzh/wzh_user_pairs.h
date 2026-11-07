#ifndef WZH_USER_PAIRS_H_
#define WZH_USER_PAIRS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "wzh_sound_score.h"

namespace wzh {
struct UserPairFrame {
    uint16_t duration_ms;
    int16_t angles[5];
    uint8_t ease;
};
struct UserPair {
    const char* name;
    const char* alias;
    const uint8_t* audio;
    uint32_t audio_size;
    uint32_t audio_ms;
    const UserPairFrame* frames;
    uint16_t frame_count;
    uint32_t motion_ms;
    uint8_t axes;
    const SoundScore* score = nullptr;
};

// Registered once by the selected board, before the audio/motion tasks start.
// All bytes and keyframes stay in internal flash; no external Flash writes.
inline const UserPair* user_pairs = nullptr;
inline size_t user_pair_count = 0;
constexpr int kUserPairIndexBase = 32;
inline void RegisterUserPairs(const UserPair* pairs, size_t count) {
    user_pairs = pairs;
    user_pair_count = std::min<size_t>(count, 32);
}
inline const UserPair* UserPairAtIndex(int index) {
    const int offset = index - kUserPairIndexBase;
    return offset >= 0 && static_cast<size_t>(offset) < user_pair_count ? &user_pairs[offset]
                                                                        : nullptr;
}
inline int FindUserPair(const char* name) {
    if (!name)
        return -1;
    for (size_t i = 0; i < user_pair_count; ++i) {
        const auto& pair = user_pairs[i];
        auto matches = [name](const char* candidate) {
            if (!candidate || !*candidate)
                return false;
            const size_t n = std::strlen(candidate);
            return std::strcmp(name, candidate) == 0 ||
                   (std::strlen(name) == n + 5 && std::strncmp(name, candidate, n) == 0 &&
                    std::strcmp(name + n, ".opus") == 0);
        };
        if (matches(pair.name) || matches(pair.alias))
            return kUserPairIndexBase + i;
    }
    return -1;
}
}  // namespace wzh
#endif
