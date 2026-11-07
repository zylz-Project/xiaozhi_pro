#ifndef WZH_DINOSAUR_REFERENCE_MOTION_H_
#define WZH_DINOSAUR_REFERENCE_MOTION_H_

// Imported from newWebServer_Project/dinosaur/main/auto_run_data.cc.
// Only tail_ud is converted: max(55, 180 - legacy_tail_ud).
// kKeep inherits the previous physical pose. Finish at this board's neutral.
namespace reference_motion {
using wzh_dino::Keyframe;
using wzh_dino::kKeep;
using wzh_dino::MotionEase;

constexpr Keyframe kDiscoverFrames[] = {
    {kKeep, kKeep, 128, kKeep, kKeep, 140, MotionEase::kFastOut},
    {kKeep, kKeep, 128, kKeep, kKeep, 170, MotionEase::kLinear},
    {62, 118, 124, 148, 64, 320, MotionEase::kSmooth},
    {70, 150, 56, 152, 146, 440, MotionEase::kSmooth},
    {78, 34, 142, 146, 40, 520, MotionEase::kSmooth},
    {108, 106, 84, 138, 122, 380, MotionEase::kSmooth},
    {94, 96, 90, 122, 90, 560, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kDiscoverPeekFrames[] = {
    {kKeep, kKeep, 134, kKeep, kKeep, 110, MotionEase::kFastOut},
    {48, 76, 138, 146, 126, 240, MotionEase::kSmooth},
    {48, 76, 138, 150, 126, 210, MotionEase::kLinear},
    {66, 146, 48, 154, 42, 390, MotionEase::kSmooth},
    {76, 36, 144, 148, 148, 520, MotionEase::kSmooth},
    {116, 112, 82, 140, 62, 420, MotionEase::kSmooth},
    {94, 98, 90, 122, 90, 620, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kDiscoverSkyFrames[] = {
    {kKeep, kKeep, 166, kKeep, kKeep, 100, MotionEase::kFastOut},
    {kKeep, kKeep, 166, kKeep, kKeep, 190, MotionEase::kLinear},
    {8, 118, 150, 162, 24, 340, MotionEase::kSmooth},
    {16, 174, 24, 180, 162, 520, MotionEase::kSmooth},
    {38, 6, 174, 168, 18, 580, MotionEase::kSmooth},
    {54, 82, 90, 164, 146, 260, MotionEase::kLinear},
    {116, 108, 78, 146, 42, 460, MotionEase::kSmooth},
    {92, 96, 90, 124, 90, 620, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kAffectionFrames[] = {
    {104, 100, 90, 132, 92, 260, MotionEase::kSmooth},
    {142, 118, 78, 148, 66, 520, MotionEase::kSmooth},
    {164, 110, 86, 152, 122, 360, MotionEase::kSmooth},
    {150, 124, 78, 150, 58, 440, MotionEase::kLinear},
    {116, 104, 88, 142, 114, 520, MotionEase::kSmooth},
    {96, 96, 90, 122, 90, 720, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kAffectionCuddleFrames[] = {
    {96, 66, 124, 138, 128, 300, MotionEase::kSmooth},
    {132, 142, 58, 152, 40, 560, MotionEase::kSmooth},
    {170, 116, 82, 156, 144, 520, MotionEase::kSmooth},
    {158, 92, 90, 154, 52, 520, MotionEase::kLinear},
    {118, 56, 132, 142, 138, 580, MotionEase::kSmooth},
    {96, 94, 90, 122, 90, 760, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kAffectionNuzzleFrames[] = {
    {104, 102, 90, 132, 90, 240, MotionEase::kSmooth},
    {126, 132, 72, 148, 42, 420, MotionEase::kSmooth},
    {138, 48, 138, 154, 148, 480, MotionEase::kSmooth},
    {150, 112, 82, 156, 52, 360, MotionEase::kLinear},
    {116, 76, 118, 144, 136, 520, MotionEase::kSmooth},
    {96, 96, 90, 122, 90, 720, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kAffectionShyFrames[] = {
    {52, 132, 18, 156, 132, 180, MotionEase::kFastOut},
    {28, 156, 12, 170, 42, 320, MotionEase::kSmooth},
    {28, 156, 12, 172, 42, 240, MotionEase::kLinear},
    {176, 118, 82, 176, 154, 620, MotionEase::kSmooth},
    {180, 92, 90, 180, 28, 460, MotionEase::kLinear},
    {154, 72, 112, 172, 168, 360, MotionEase::kSmooth},
    {112, 108, 78, 148, 52, 620, MotionEase::kSmooth},
    {94, 96, 90, 122, 90, 760, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kHappyFrames[] = {
    {54, 92, 90, 156, 90, 240, MotionEase::kFastOut},
    {88, 154, 54, 160, 36, 440, MotionEase::kSmooth},
    {118, 28, 148, 150, 152, 620, MotionEase::kSmooth},
    {142, 108, 82, 156, 54, 500, MotionEase::kSmooth},
    {96, 94, 90, 124, 90, 720, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kHappyDanceFrames[] = {
    {42, 72, 116, 158, 136, 280, MotionEase::kFastOut},
    {108, 166, 42, 152, 30, 560, MotionEase::kSmooth},
    {158, 34, 146, 156, 154, 680, MotionEase::kSmooth},
    {74, 116, 66, 160, 46, 540, MotionEase::kSmooth},
    {96, 96, 90, 124, 90, 760, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kHappyOrbitFrames[] = {
    {42, 90, 108, 146, 90, 180, MotionEase::kFastOut},
    {68, 166, 112, 122, 154, 240, MotionEase::kSmooth},
    {146, 174, 78, 55, 92, 240, MotionEase::kSmooth},
    {178, 92, 66, 55, 24, 260, MotionEase::kSmooth},
    {136, 12, 102, 104, 70, 250, MotionEase::kSmooth},
    {54, 4, 122, 150, 158, 240, MotionEase::kSmooth},
    {18, 86, 108, 132, 88, 280, MotionEase::kSmooth},
    {96, 142, 76, 58, 32, 310, MotionEase::kSmooth},
    {98, 94, 90, 108, 90, 520, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kHappyChaseFrames[] = {
    {kKeep, kKeep, 12, kKeep, kKeep, 110, MotionEase::kFastOut},
    {42, 142, 172, kKeep, kKeep, 210, MotionEase::kSmooth},
    {68, 176, 28, 180, 18, 360, MotionEase::kSmooth},
    {156, 22, 162, 158, 176, 520, MotionEase::kSmooth},
    {112, 88, 90, 172, 34, 260, MotionEase::kLinear},
    {18, 118, 36, 180, 162, 480, MotionEase::kFastOut},
    {138, 164, 146, 162, 12, 560, MotionEase::kSmooth},
    {96, 94, 90, 126, 90, 760, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kProudCallFrames[] = {
    {34, 90, 90, 150, 90, 260, MotionEase::kSmooth},
    {0, 90, 90, 180, 90, 180, MotionEase::kFastOut},
    {0, 90, 90, 180, 138, 520, MotionEase::kLinear},
    {118, 106, 116, 142, 125, 400, MotionEase::kSmooth},
    {112, 74, 66, 140, 58, 420, MotionEase::kSmooth},
    {116, 101, 108, 138, 118, 420, MotionEase::kSmooth},
    {105, 92, 94, 130, 85, 500, MotionEase::kSmooth},
    {97, 92, 92, 110, 90, 650, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kProudSweepFrames[] = {
    {42, 126, 132, 162, 132, 360, MotionEase::kSmooth},
    {16, 136, 142, 172, 146, 180, MotionEase::kLinear},
    {0, 118, 126, 180, 42, 240, MotionEase::kFastOut},
    {6, 46, 34, 178, 154, 500, MotionEase::kLinear},
    {20, 164, 30, 172, 28, 580, MotionEase::kSmooth},
    {34, 18, 152, 168, 160, 620, MotionEase::kSmooth},
    {62, 122, 64, 152, 42, 620, MotionEase::kSmooth},
    {84, 94, 90, 130, 90, 760, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kProudOrbitFrames[] = {
    {24, 92, 96, 164, 88, 320, MotionEase::kSmooth},
    {0, 92, 90, 180, 92, 220, MotionEase::kFastOut},
    {2, 150, 48, 176, 158, 480, MotionEase::kLinear},
    {48, 170, 118, 156, 158, 420, MotionEase::kSmooth},
    {126, 178, 84, 55, 106, 440, MotionEase::kSmooth},
    {154, 150, 72, 55, 42, 300, MotionEase::kLinear},
    {178, 78, 68, 62, 18, 460, MotionEase::kSmooth},
    {132, 6, 106, 138, 76, 440, MotionEase::kSmooth},
    {46, 0, 122, 156, 164, 420, MotionEase::kSmooth},
    {16, 34, 116, 122, 138, 300, MotionEase::kLinear},
    {62, 118, 80, 55, 30, 480, MotionEase::kSmooth},
    {100, 92, 90, 114, 90, 600, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kProudEchoFrames[] = {
    {36, 90, 90, 162, 90, 300, MotionEase::kSmooth},
    {0, 90, 90, 180, 90, 180, MotionEase::kFastOut},
    {0, 104, 118, 180, 154, 560, MotionEase::kLinear},
    {48, 172, 18, 164, 24, 620, MotionEase::kSmooth},
    {72, 18, 176, 156, 168, 360, MotionEase::kSmooth},
    {18, 26, 154, 172, 142, 220, MotionEase::kFastOut},
    {0, 34, 138, 180, 28, 420, MotionEase::kLinear},
    {42, 150, 32, 162, 164, 620, MotionEase::kSmooth},
    {84, 94, 90, 132, 90, 780, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kEatFrames[] = {
    {138, 103, 96, 100, 104, 420, MotionEase::kSmooth},
    {178, 100, 92, 98, 76, 230, MotionEase::kFastOut},
    {148, 104, 98, 100, 110, 260, MotionEase::kSmooth},
    {180, 108, 102, 98, 68, 240, MotionEase::kFastOut},
    {151, 96, 86, 100, 118, 290, MotionEase::kSmooth},
    {176, 92, 84, 98, 72, 250, MotionEase::kFastOut},
    {147, 104, 102, 100, 112, 320, MotionEase::kSmooth},
    {153, 102, 96, 100, 108, 420, MotionEase::kSmooth},
    {142, 94, 90, 102, 90, 340, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kEatSniffFrames[] = {
    {126, 124, 116, 104, 112, 320, MotionEase::kSmooth},
    {145, 132, 108, 102, 66, 240, MotionEase::kSmooth},
    {130, 126, 112, 104, 116, 220, MotionEase::kSmooth},
    {151, 118, 104, 102, 62, 250, MotionEase::kSmooth},
    {180, 110, 98, 98, 126, 280, MotionEase::kFastOut},
    {150, 104, 94, 100, 54, 300, MotionEase::kSmooth},
    {177, 86, 80, 98, 132, 280, MotionEase::kFastOut},
    {148, 78, 84, 100, 50, 320, MotionEase::kSmooth},
    {156, 92, 90, 100, 118, 420, MotionEase::kSmooth},
    {140, 100, 94, 102, 90, 380, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kEatLookUpFrames[] = {
    {180, 86, 82, 98, 58, 170, MotionEase::kFastOut},
    {148, 104, 100, 102, 128, 160, MotionEase::kFastOut},
    {176, 114, 108, 98, 52, 180, MotionEase::kFastOut},
    {150, 120, 106, 102, 136, 180, MotionEase::kFastOut},
    {84, 102, 90, 122, 72, 330, MotionEase::kFastOut},
    {78, 90, 90, 128, 116, 420, MotionEase::kLinear},
    {132, 76, 78, 108, 62, 360, MotionEase::kSmooth},
    {180, 82, 84, 98, 132, 190, MotionEase::kFastOut},
    {149, 104, 98, 100, 54, 170, MotionEase::kFastOut},
    {158, 96, 92, 100, 104, 420, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kEatGrazeFrames[] = {
    {142, 154, 18, 122, 142, 360, MotionEase::kSmooth},
    {180, 166, 8, 146, 24, 260, MotionEase::kFastOut},
    {154, 142, 38, 152, 154, 340, MotionEase::kSmooth},
    {176, 92, 90, 162, 36, 520, MotionEase::kSmooth},
    {180, 24, 172, 168, 166, 260, MotionEase::kFastOut},
    {150, 42, 146, 172, 18, 380, MotionEase::kSmooth},
    {178, 72, 118, 176, 152, 300, MotionEase::kFastOut},
    {148, 108, 76, 162, 42, 460, MotionEase::kSmooth},
    {116, 96, 90, 134, 90, 620, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kListenFrames[] = {
    {kKeep, kKeep, kKeep, 138, 90, 100, MotionEase::kFastOut},
    {76, 90, 128, 138, 90, 220, MotionEase::kFastOut},
    {82, 108, 126, 140, 100, 260, MotionEase::kSmooth},
    {86, 113, 105, 138, 108, 260, MotionEase::kSmooth},
    {88, 113, 105, 138, 108, 380, MotionEase::kLinear},
    {82, 72, 52, 138, 76, 320, MotionEase::kSmooth},
    {84, 90, 90, 135, 90, 360, MotionEase::kSmooth},
    {88, 94, 94, 130, 90, 420, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kListenTrackFrames[] = {
    {kKeep, kKeep, 142, 148, 112, 90, MotionEase::kFastOut},
    {66, 112, 138, 152, 122, 180, MotionEase::kFastOut},
    {72, 126, 118, 154, 132, 420, MotionEase::kSmooth},
    {88, 118, 102, 150, 58, 240, MotionEase::kSmooth},
    {108, 104, 94, 146, 138, 180, MotionEase::kFastOut},
    {98, 64, 48, 150, 42, 260, MotionEase::kSmooth},
    {80, 76, 58, 146, 126, 420, MotionEase::kSmooth},
    {90, 94, 94, 128, 90, 500, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kListenDoubleCheckFrames[] = {
    {62, 90, 90, 144, 90, 100, MotionEase::kFastOut},
    {70, 132, 142, 150, 138, 220, MotionEase::kFastOut},
    {76, 138, 116, 152, 48, 260, MotionEase::kSmooth},
    {76, 138, 116, 152, 48, 160, MotionEase::kLinear},
    {82, 52, 38, 148, 142, 220, MotionEase::kFastOut},
    {92, 62, 58, 144, 44, 300, MotionEase::kSmooth},
    {104, 106, 112, 138, 132, 360, MotionEase::kSmooth},
    {92, 94, 94, 124, 90, 520, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kListenOverheadFrames[] = {
    {kKeep, kKeep, 4, kKeep, kKeep, 90, MotionEase::kFastOut},
    {34, 64, 8, 138, 90, 220, MotionEase::kSmooth},
    {0, 48, 22, 162, 90, 260, MotionEase::kFastOut},
    {0, 48, 22, 172, 90, 460, MotionEase::kLinear},
    {8, 118, 176, 176, 28, 180, MotionEase::kFastOut},
    {18, 172, 154, 180, 166, 520, MotionEase::kSmooth},
    {46, 12, 18, 166, 42, 420, MotionEase::kSmooth},
    {86, 94, 90, 130, 90, 680, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kStartledFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, kKeep, 80, MotionEase::kLinear},
    {42, 74, 62, 156, 142, 140, MotionEase::kFastOut},
    {42, 74, 62, 158, 142, 380, MotionEase::kLinear},
    {54, 92, 126, 156, 54, 260, MotionEase::kSmooth},
    {66, 142, 44, 152, 148, 380, MotionEase::kSmooth},
    {88, 42, 142, 146, 38, 460, MotionEase::kSmooth},
    {108, 104, 86, 136, 118, 560, MotionEase::kSmooth},
    {94, 96, 90, 122, 90, 620, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kStartledJumpFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, kKeep, 70, MotionEase::kLinear},
    {32, 136, 142, 162, 36, 110, MotionEase::kFastOut},
    {30, 140, 148, 164, 32, 280, MotionEase::kLinear},
    {48, 116, 112, 160, 148, 220, MotionEase::kFastOut},
    {62, 42, 150, 154, 40, 420, MotionEase::kSmooth},
    {78, 138, 38, 150, 152, 460, MotionEase::kSmooth},
    {110, 112, 82, 138, 54, 520, MotionEase::kSmooth},
    {94, 96, 90, 122, 90, 700, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kStartledDuckFrames[] = {
    {kKeep, kKeep, kKeep, kKeep, kKeep, 70, MotionEase::kLinear},
    {180, 54, 8, 180, 12, 120, MotionEase::kFastOut},
    {180, 54, 8, 180, 12, 320, MotionEase::kLinear},
    {156, 18, 172, 172, 12, 220, MotionEase::kFastOut},
    {108, 168, 24, 162, 176, 460, MotionEase::kSmooth},
    {24, 132, 48, 176, 158, 520, MotionEase::kSmooth},
    {118, 28, 162, 152, 24, 620, MotionEase::kSmooth},
    {94, 96, 90, 124, 90, 760, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kSleepyFrames[] = {
    {108, 104, 94, 84, 104, 720, MotionEase::kSmooth},
    {146, 126, 72, 68, 64, 900, MotionEase::kSmooth},
    {158, 112, 82, 58, 116, 720, MotionEase::kSmooth},
    {142, 64, 128, 64, 72, 980, MotionEase::kSmooth},
    {162, 106, 86, 56, 108, 1050, MotionEase::kSmooth},
    {150, 102, 88, 60, 90, 820, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kSleepyNodFrames[] = {
    {112, 68, 132, 82, 64, 560, MotionEase::kSmooth},
    {156, 78, 118, 62, 54, 560, MotionEase::kSmooth},
    {118, 94, 88, 72, 112, 320, MotionEase::kFastOut},
    {172, 128, 58, 56, 136, 760, MotionEase::kSmooth},
    {132, 108, 82, 66, 68, 420, MotionEase::kSmooth},
    {164, 58, 136, 55, 132, 900, MotionEase::kSmooth},
    {152, 104, 88, 58, 104, 1100, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
constexpr Keyframe kSleepyWrapFrames[] = {
    {122, 158, 24, 108, 154, 760, MotionEase::kSmooth},
    {168, 174, 8, 88, 132, 980, MotionEase::kSmooth},
    {178, 132, 42, 72, 42, 620, MotionEase::kSmooth},
    {132, 96, 90, 76, 90, 360, MotionEase::kFastOut},
    {176, 18, 174, 62, 22, 1080, MotionEase::kSmooth},
    {162, 54, 146, 56, 154, 920, MotionEase::kSmooth},
    {154, 102, 90, 58, 90, 1180, MotionEase::kSmooth},
    {70, 90, 90, 90, 90, 800, MotionEase::kSmooth},
};
}  // namespace reference_motion
#endif  // WZH_DINOSAUR_REFERENCE_MOTION_H_
