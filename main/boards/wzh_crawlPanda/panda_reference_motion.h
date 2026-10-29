#ifndef WZH_CRAWLPANDA_REFERENCE_MOTION_H_
#define WZH_CRAWLPANDA_REFERENCE_MOTION_H_

// Imported from newWebServer_Project/crawlPanda/main/auto_run.cc.
// Pose order and timing are preserved. A board-neutral ending is appended
// when required; the tail panda must sit at 150 degrees, not legacy 85/105.
namespace reference_motion {
using wzh_panda::Keyframe;

constexpr Keyframe kIdleListen[] = {
    {650, 136, 90, 90},  {500, 148, 90, 90}, {720, 126, 72, 90},
    {900, 102, 88, 104}, {900, 90, 90, 90},
};
constexpr Keyframe kIdlePawAdjust[] = {
    {700, 54, 90, 90},   {700, 66, 58, 92}, {650, 118, 82, 128},
    {900, 104, 100, 84}, {850, 90, 90, 90},
};
constexpr Keyframe kIdleBreath[] = {
    {1100, 102, 82, 98},
    {1250, 78, 94, 86},
    {1150, 110, 76, 105},
    {1300, 90, 90, 90},
};
constexpr Keyframe kIdleStretch[] = {
    {900, 138, 132, 48}, {1000, 150, 162, 22}, {950, 76, 42, 142},
    {1100, 62, 22, 164}, {1200, 90, 90, 90},
};
constexpr Keyframe kCuriousPeek[] = {
    {380, 146, 90, 90}, {620, 156, 90, 90},  {720, 150, 42, 94},
    {430, 54, 28, 102}, {700, 128, 84, 152}, {1000, 90, 90, 90},
};
constexpr Keyframe kCuriousReach[] = {
    {600, 38, 90, 90},   {750, 52, 22, 92},  {620, 142, 48, 138},
    {800, 128, 86, 166}, {1000, 90, 90, 90},
};
constexpr Keyframe kCuriousDoubleCheck[] = {
    {440, 142, 78, 90}, {520, 154, 50, 92},  {420, 42, 82, 128},
    {620, 54, 90, 166}, {760, 124, 36, 116}, {1000, 90, 90, 90},
};
constexpr Keyframe kEatHoldBamboo[] = {
    {750, 44, 62, 118},  {850, 132, 30, 150}, {520, 58, 24, 146},
    {580, 128, 38, 158}, {700, 72, 48, 136},  {1000, 90, 90, 90},
};
constexpr Keyframe kEatChew[] = {
    {650, 132, 36, 144}, {360, 54, 30, 148},  {470, 124, 42, 156},
    {340, 66, 28, 146},  {520, 142, 54, 164}, {950, 90, 90, 90},
};
constexpr Keyframe kEatPawSwap[] = {
    {700, 46, 22, 132},  {620, 132, 68, 168}, {520, 58, 18, 154},
    {650, 142, 54, 140}, {760, 112, 34, 158}, {1000, 90, 90, 90},
};
constexpr Keyframe kBabyCall[] = {
    {420, 34, 86, 92},  {380, 148, 88, 94},  {600, 128, 36, 98},
    {500, 52, 74, 158}, {700, 136, 20, 168}, {950, 90, 90, 90},
};
constexpr Keyframe kBabyPaddle[] = {
    {480, 138, 22, 92}, {480, 58, 74, 164},  {520, 128, 16, 118},
    {520, 62, 64, 170}, {700, 142, 30, 150}, {900, 90, 90, 90},
};
constexpr Keyframe kBabyFindUser[] = {
    {650, 38, 90, 90},   {750, 52, 26, 92},   {620, 148, 72, 154},
    {780, 138, 18, 170}, {850, 112, 44, 140}, {1000, 90, 90, 90},
};
constexpr Keyframe kProudPushUp[] = {
    {650, 42, 78, 102}, {750, 138, 36, 146}, {850, 154, 14, 168},
    {650, 56, 22, 160}, {850, 132, 52, 136}, {1050, 90, 90, 90},
};
constexpr Keyframe kProudPatrol[] = {
    {780, 28, 20, 94},   {850, 46, 44, 154}, {780, 146, 86, 170},
    {850, 158, 24, 128}, {1050, 90, 90, 90},
};
constexpr Keyframe kProudCall[] = {
    {550, 118, 82, 98}, {760, 154, 42, 142}, {900, 146, 16, 166},
    {720, 44, 62, 158}, {800, 132, 24, 124}, {1050, 90, 90, 90},
};
constexpr Keyframe kAffectionHug[] = {
    {800, 142, 86, 94},  {900, 156, 48, 132}, {850, 146, 22, 160},
    {900, 116, 34, 150}, {1100, 90, 90, 90},
};
constexpr Keyframe kAffectionShy[] = {
    {650, 36, 94, 88},   {850, 52, 134, 52}, {620, 142, 72, 142},
    {850, 154, 30, 162}, {1100, 90, 90, 90},
};
constexpr Keyframe kAffectionAskUp[] = {
    {900, 132, 104, 76}, {950, 148, 142, 38}, {900, 126, 54, 142},
    {850, 62, 24, 168},  {1150, 90, 90, 90},
};
constexpr Keyframe kPlayfulCrawl[] = {
    {520, 138, 18, 92}, {520, 54, 70, 166},  {540, 126, 12, 122},
    {540, 62, 62, 172}, {650, 144, 26, 150}, {900, 90, 90, 90},
};
constexpr Keyframe kPlayfulPawDrum[] = {
    {480, 142, 28, 94}, {480, 52, 78, 158},  {500, 132, 18, 118},
    {500, 58, 66, 170}, {560, 122, 14, 166}, {900, 90, 90, 90},
};
constexpr Keyframe kPlayfulPounce[] = {
    {700, 44, 138, 44}, {620, 132, 86, 96},  {620, 148, 18, 168},
    {700, 54, 42, 148}, {760, 124, 24, 166}, {950, 90, 90, 90},
};
constexpr Keyframe kAmbientListen[] = {
    {1200, 42, 90, 90},   {1500, 56, 54, 94}, {1300, 146, 82, 142},
    {1700, 132, 28, 160}, {1600, 90, 90, 90},
};
constexpr Keyframe kAmbientRain[] = {
    {1200, 132, 120, 62}, {1400, 48, 72, 128},  {1300, 118, 30, 152},
    {1500, 62, 66, 168},  {1500, 104, 22, 136}, {1600, 90, 90, 90},
};
constexpr Keyframe kAmbientExplore[] = {
    {1300, 34, 40, 92},   {1450, 62, 78, 158}, {1300, 144, 86, 172},
    {1450, 118, 22, 126}, {1600, 90, 90, 90},
};
constexpr Keyframe kAmbientStretch[] = {
    {1500, 122, 90, 90}, {1800, 146, 152, 30}, {1800, 132, 48, 132},
    {1700, 54, 16, 168}, {1800, 90, 90, 90},
};
constexpr Keyframe kSettle[] = {
    {750, 102, 78, 102},
    {1050, 94, 88, 92},
    {900, 90, 90, 90},
};
}  // namespace reference_motion
#endif  // WZH_CRAWLPANDA_REFERENCE_MOTION_H_
