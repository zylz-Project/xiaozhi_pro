#ifndef WZH_INTERACTION_H_
#define WZH_INTERACTION_H_

#include <cstdint>

namespace wzh {
enum class PetInteraction : uint8_t {
    kNone,
    kTouch1,
    kTouch2,
    kTouch3,
    kTouch4,
    kPlayful,
    kHeld,
    kReleased,
    kPickedUp,
    kShaken,
    kFlipped,
    kPutDown,
};

// Five banks of 11 event clips; each bank keeps the PetInteraction order.
// Owned by the motion task: rejected/overwritten requests never consume a variant.
class InteractionMotionSelector {
public:
    static constexpr unsigned kEventCount = static_cast<unsigned>(PetInteraction::kPutDown);
    static constexpr unsigned kVariantCount = 5;
    static constexpr unsigned kClipCount = kEventCount * kVariantCount;

    int Select(PetInteraction interaction, uint32_t random = 0) {
        const auto event = static_cast<unsigned>(interaction);
        if (event == 0 || event > kEventCount) {
            return -1;
        }
        const unsigned index = event - 1;
        auto& pool = pools_[index];
        if (pool.remaining == 0) {
            pool.remaining = (1U << kVariantCount) - 1U;
        }
        const uint8_t candidates = pool.remaining & ~(1U << pool.last);
        unsigned count = 0;
        for (unsigned variant = 0; variant < kVariantCount; ++variant) {
            if (candidates & (1U << variant))
                ++count;
        }
        unsigned choice = random % count;
        for (unsigned variant = 0; variant < kVariantCount; ++variant) {
            if ((candidates & (1U << variant)) && choice-- == 0) {
                pool.remaining &= ~(1U << variant);
                pool.last = variant;
                return static_cast<int>(variant * kEventCount + index);
            }
        }
        return -1;
    }

private:
    struct Pool {
        uint8_t remaining = 0;
        uint8_t last = kVariantCount;  // Sentinel outside valid bits, safe to shift.
    };
    Pool pools_[kEventCount];
};

inline const char* InteractionName(PetInteraction interaction) {
    static constexpr const char* names[] = {
        "none", "touch1",   "touch2",    "touch3", "touch4",  "playful",
        "held", "released", "picked_up", "shaken", "flipped", "settled",
    };
    const auto index = static_cast<unsigned>(interaction);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}
}  // namespace wzh
#endif
