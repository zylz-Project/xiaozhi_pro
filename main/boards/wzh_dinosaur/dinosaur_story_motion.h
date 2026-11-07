#ifndef WZH_DINOSAUR_STORY_MOTION_H_
#define WZH_DINOSAUR_STORY_MOTION_H_

// Physical tilt/lean/head/tail-UD/tail-LR, duration, easing. Each changing axis
// shares a segment clock; spatial arcs come from the authored poses themselves.
namespace story_motion {
using wzh_dino::Keyframe;
using wzh_dino::MotionEase;

constexpr Keyframe kHorizonLadder[] = {
    {42, 118, 150, 126, 54, 600, MotionEase::kSmooth},
    {8, 156, 108, 168, 24, 800, MotionEase::kSmooth},
    {8, 156, 108, 168, 24, 380, MotionEase::kLinear},
    {36, 90, 36, 146, 94, 900, MotionEase::kSmooth},
    {112, 24, 158, 116, 164, 850, MotionEase::kSmooth},
    {152, 62, 116, 82, 128, 750, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 950, MotionEase::kSmooth},
};
constexpr Keyframe kForeheadOffer[] = {
    {46, 122, 150, 124, 52, 600, MotionEase::kSmooth},
    {118, 142, 64, 150, 30, 900, MotionEase::kSmooth},
    {174, 100, 90, 164, 122, 950, MotionEase::kSmooth},
    {174, 100, 90, 164, 122, 550, MotionEase::kLinear},
    {132, 42, 138, 138, 152, 950, MotionEase::kSmooth},
    {84, 72, 106, 108, 112, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 950, MotionEase::kSmooth},
};
constexpr Keyframe kSpiralInvite[] = {
    {22, 90, 126, 128, 90, 650, MotionEase::kSmooth},
    {58, 158, 104, 164, 28, 750, MotionEase::kSmooth},
    {138, 164, 46, 124, 76, 850, MotionEase::kSmooth},
    {176, 98, 66, 66, 158, 800, MotionEase::kSmooth},
    {122, 18, 126, 98, 154, 850, MotionEase::kSmooth},
    {36, 24, 148, 170, 56, 900, MotionEase::kSmooth},
    {112, 116, 76, 134, 28, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 950, MotionEase::kSmooth},
};
constexpr Keyframe kCrestSalute[] = {
    {24, 90, 90, 136, 90, 650, MotionEase::kSmooth},
    {0, 90, 90, 180, 90, 800, MotionEase::kSmooth},
    {0, 90, 90, 180, 90, 450, MotionEase::kLinear},
    {28, 154, 36, 168, 28, 1000, MotionEase::kSmooth},
    {42, 22, 158, 144, 164, 1000, MotionEase::kSmooth},
    {86, 114, 76, 116, 54, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 950, MotionEase::kSmooth},
};
constexpr Keyframe kGrazeShare[] = {
    {136, 142, 34, 108, 142, 750, MotionEase::kSmooth},
    {180, 124, 62, 78, 34, 650, MotionEase::kSmooth},
    {148, 108, 78, 98, 126, 500, MotionEase::kSmooth},
    {82, 90, 90, 132, 90, 850, MotionEase::kSmooth},
    {82, 90, 90, 132, 90, 480, MotionEase::kLinear},
    {162, 36, 146, 92, 156, 900, MotionEase::kSmooth},
    {176, 68, 116, 68, 46, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kNearFarAnswer[] = {
    {52, 134, 160, 122, 46, 650, MotionEase::kSmooth},
    {52, 134, 160, 122, 46, 420, MotionEase::kLinear},
    {12, 152, 112, 166, 26, 800, MotionEase::kSmooth},
    {38, 30, 28, 152, 160, 1000, MotionEase::kSmooth},
    {38, 30, 28, 152, 160, 350, MotionEase::kLinear},
    {122, 76, 98, 110, 122, 900, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 950, MotionEase::kSmooth},
};
constexpr Keyframe kGuardAndTrust[] = {
    {26, 126, 142, 160, 28, 550, MotionEase::kSmooth},
    {26, 126, 142, 160, 28, 400, MotionEase::kLinear},
    {58, 154, 38, 146, 48, 800, MotionEase::kSmooth},
    {94, 30, 152, 132, 160, 1000, MotionEase::kSmooth},
    {144, 106, 82, 116, 54, 950, MotionEase::kSmooth},
    {104, 90, 90, 108, 122, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 950, MotionEase::kSmooth},
};
constexpr Keyframe kCrescentRest[] = {
    {108, 142, 42, 112, 138, 850, MotionEase::kSmooth},
    {164, 162, 18, 74, 112, 1100, MotionEase::kSmooth},
    {176, 124, 58, 58, 38, 900, MotionEase::kSmooth},
    {176, 124, 58, 58, 38, 600, MotionEase::kLinear},
    {138, 42, 146, 66, 26, 1200, MotionEase::kSmooth},
    {114, 64, 118, 82, 112, 950, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1200, MotionEase::kSmooth},
};
constexpr Keyframe kScentCompass[] = {
    {128, 128, 34, 116, 146, 700, MotionEase::kSmooth},
    {170, 148, 58, 72, 28, 780, MotionEase::kSmooth},
    {170, 148, 146, 72, 28, 620, MotionEase::kSmooth},
    {116, 24, 158, 128, 164, 950, MotionEase::kSmooth},
    {12, 62, 146, 176, 28, 1000, MotionEase::kSmooth},
    {12, 62, 38, 176, 28, 720, MotionEase::kSmooth},
    {52, 126, 78, 136, 130, 780, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kGentleInvitation[] = {
    {38, 38, 44, 138, 146, 900, MotionEase::kSmooth},
    {108, 38, 134, 154, 146, 1000, MotionEase::kSmooth},
    {156, 144, 96, 124, 32, 1200, MotionEase::kSmooth},
    {156, 144, 96, 124, 32, 450, MotionEase::kLinear},
    {84, 122, 158, 168, 62, 1000, MotionEase::kSmooth},
    {42, 70, 78, 130, 118, 900, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kCometDance[] = {
    {40, 90, 90, 174, 18, 700, MotionEase::kSmooth},
    {18, 148, 160, 126, 164, 850, MotionEase::kSmooth},
    {112, 164, 40, 60, 84, 950, MotionEase::kSmooth},
    {162, 90, 90, 156, 18, 850, MotionEase::kSmooth},
    {74, 18, 156, 178, 164, 1000, MotionEase::kSmooth},
    {10, 58, 30, 124, 142, 900, MotionEase::kSmooth},
    // Complete the upward arc, then settle directly instead of adding another
    // cross-body rebound to an already broad eight-second story.
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kRoyalLookout[] = {
    {24, 90, 90, 136, 90, 750, MotionEase::kSmooth},
    {0, 90, 90, 180, 90, 850, MotionEase::kSmooth},
    {0, 90, 90, 180, 90, 500, MotionEase::kLinear},
    {24, 154, 34, 174, 18, 950, MotionEase::kSmooth},
    {24, 154, 134, 174, 18, 680, MotionEase::kSmooth},
    {38, 24, 158, 154, 168, 1000, MotionEase::kSmooth},
    {90, 114, 74, 114, 52, 950, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kBiteAndOffer[] = {
    {138, 142, 32, 108, 144, 850, MotionEase::kSmooth},
    {180, 122, 62, 78, 34, 750, MotionEase::kSmooth},
    {148, 106, 80, 100, 128, 600, MotionEase::kSmooth},
    {82, 90, 90, 136, 90, 1000, MotionEase::kSmooth},
    {82, 90, 90, 136, 90, 450, MotionEase::kLinear},
    {164, 34, 148, 90, 154, 1000, MotionEase::kSmooth},
    {174, 68, 116, 68, 44, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1100, MotionEase::kSmooth},
};
constexpr Keyframe kQuestionAndAnswer[] = {
    {52, 32, 158, 152, 26, 780, MotionEase::kSmooth},
    {52, 32, 76, 152, 26, 600, MotionEase::kSmooth},
    {126, 142, 76, 110, 164, 900, MotionEase::kSmooth},
    {20, 154, 28, 174, 142, 1000, MotionEase::kSmooth},
    {20, 154, 118, 174, 142, 640, MotionEase::kSmooth},
    {92, 60, 68, 112, 34, 850, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kRecoverConfidence[] = {
    {158, 132, 138, 72, 28, 1000, MotionEase::kSmooth},
    {158, 132, 138, 72, 28, 450, MotionEase::kLinear},
    {116, 40, 32, 118, 154, 1200, MotionEase::kSmooth},
    {28, 60, 68, 172, 142, 1100, MotionEase::kSmooth},
    {18, 142, 148, 164, 38, 1100, MotionEase::kSmooth},
    {92, 112, 108, 116, 82, 900, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1000, MotionEase::kSmooth},
};
constexpr Keyframe kDreamTurn[] = {
    {110, 142, 42, 112, 140, 950, MotionEase::kSmooth},
    {166, 160, 20, 74, 114, 1200, MotionEase::kSmooth},
    {174, 122, 58, 58, 36, 1000, MotionEase::kSmooth},
    {174, 122, 58, 58, 36, 650, MotionEase::kLinear},
    {138, 40, 148, 66, 24, 1300, MotionEase::kSmooth},
    {112, 64, 118, 82, 110, 1050, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 1250, MotionEase::kSmooth},
};
}  // namespace story_motion
#endif  // WZH_DINOSAUR_STORY_MOTION_H_
