#ifndef WZH_SOUND_VARIANTS_H_
#define WZH_SOUND_VARIANTS_H_

#include <array>
#include "wzh_motion_profile.h"
#include "wzh_sound_score.h"

namespace wzh {
// One independent shuffle per source/file: 32 external entries + 32 paired
// entries. Interleaving recordings must not restart or consume another bag.
// Call only when a recording actually starts; entropy is supplied by the board.
template <unsigned Capacity>
class SoundVariantSelector {
public:
    int Draw(const SoundPlayback& sound, uint32_t random, unsigned count = Capacity) {
        if (!count || count > Capacity)
            return -1;
        for (auto& item : variations_) {
            if (item.used && item.score == sound.score &&
                (sound.score ? (item.index < 32) == (sound.sound_index < 32)
                             : item.index == sound.sound_index))
                return item.bag.Draw(count, random);
        }
        auto& item = variations_[replacement_++ % variations_.size()];
        item = {};
        item.used = true;
        item.score = sound.score;
        item.index = sound.sound_index;
        return item.bag.Draw(count, random);
    }

private:
    struct Variation {
        const SoundScore* score = nullptr;
        int index = -1;
        MotionShuffleBag<Capacity> bag;
        bool used = false;
    };
    std::array<Variation, 64> variations_{};
    unsigned replacement_ = 0;
};
}  // namespace wzh
#endif
