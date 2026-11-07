#ifndef WZH_TAILREDPANDA_STORY_MOTION_H_
#define WZH_TAILREDPANDA_STORY_MOTION_H_

// Direct head/tail-LR/tail-UD angles; sitting neutral is 90/90/150.
namespace story_motion {
using wzh_panda::Keyframe;

// Look, lift a question-mark tail, wait, then look back beneath the tail arc.
constexpr Keyframe kTailQuestion[] = {
    {520, 148, 92, 146}, {680, 160, 38, 72},  {720, 136, 18, 14}, {420, 136, 18, 14},
    {920, 34, 150, 44},  {660, 72, 120, 106}, {900, 90, 90, 150},
};
constexpr Keyframe kTasteAndShare[] = {
    {650, 38, 54, 172},  {520, 66, 32, 174}, {680, 144, 142, 160}, {720, 124, 166, 94},
    {400, 124, 166, 94}, {850, 58, 52, 136}, {850, 90, 90, 150},
};
constexpr Keyframe kHideAndSeek[] = {
    {720, 30, 28, 174},  {420, 30, 28, 174}, {900, 152, 72, 60},
    {650, 130, 156, 18}, {700, 48, 134, 68}, {950, 90, 90, 150},
};
// Unfurl a tall tail fan, cross the horizon, then fold it back down.
constexpr Keyframe kTailFan[] = {
    {650, 138, 90, 92},  {850, 158, 24, 14}, {450, 158, 24, 14},
    {1000, 28, 164, 28}, {900, 64, 40, 72},  {1000, 90, 90, 150},
};
constexpr Keyframe kCheekTailHug[] = {
    {650, 146, 118, 144}, {850, 168, 158, 86}, {520, 168, 158, 86}, {1000, 130, 52, 18},
    {900, 50, 24, 62},    {750, 72, 66, 118},  {1000, 90, 90, 150},
};
// A complete lifted figure eight: four different heights, no repeated wag loop.
constexpr Keyframe kRibbonEight[] = {
    {650, 150, 26, 92},  {700, 116, 82, 14}, {760, 34, 166, 82},  {700, 62, 102, 166},
    {760, 156, 18, 106}, {700, 118, 90, 26}, {800, 38, 158, 116}, {950, 90, 90, 150},
};
constexpr Keyframe kBraveReturn[] = {
    {500, 32, 28, 170},  {380, 32, 28, 170}, {760, 56, 90, 116},
    {900, 152, 148, 30}, {700, 126, 46, 66}, {950, 90, 90, 150},
};
constexpr Keyframe kCurlGoodnight[] = {
    {850, 148, 154, 118},  {1100, 164, 168, 74}, {620, 164, 168, 74},
    {1200, 118, 108, 132}, {1000, 66, 62, 172},  {1200, 90, 90, 150},
};
constexpr Keyframe kTailPeriscope[] = {
    {760, 90, 124, 30}, {420, 90, 124, 30},  {680, 156, 124, 18}, {850, 124, 26, 54},
    {940, 30, 156, 34}, {720, 64, 104, 104}, {960, 90, 90, 150},
};
constexpr Keyframe kBambooBalance[] = {
    {650, 38, 38, 162},  {600, 68, 62, 170}, {560, 136, 148, 118}, {440, 136, 148, 118},
    {840, 156, 114, 40}, {820, 58, 30, 86},  {980, 90, 90, 150},
};
constexpr Keyframe kTailPeek[] = {
    {620, 28, 32, 170},  {380, 28, 32, 170}, {820, 118, 74, 74},  {760, 160, 152, 18},
    {440, 160, 152, 18}, {900, 48, 122, 62}, {1000, 90, 90, 150},
};
constexpr Keyframe kHighTailSalute[] = {
    {620, 136, 90, 102}, {850, 160, 26, 14},  {440, 160, 26, 14},  {1040, 30, 162, 26},
    {820, 72, 112, 58},  {780, 126, 42, 102}, {1000, 90, 90, 150},
};
constexpr Keyframe kTailGift[] = {
    {760, 90, 156, 54}, {640, 158, 156, 54}, {480, 158, 156, 54}, {900, 118, 70, 14},
    {940, 32, 24, 76},  {700, 64, 108, 124}, {1000, 90, 90, 150},
};
constexpr Keyframe kTailRainbow[] = {
    {720, 90, 26, 144}, {760, 36, 144, 126}, {820, 152, 164, 56}, {740, 152, 90, 14},
    {420, 152, 90, 14}, {940, 32, 18, 36},   {850, 70, 132, 106}, {1000, 90, 90, 150},
};
constexpr Keyframe kTailReassure[] = {
    {600, 32, 28, 168},  {400, 32, 28, 168}, {800, 68, 82, 118},  {920, 152, 154, 28},
    {460, 152, 154, 28}, {860, 120, 44, 70}, {1020, 90, 90, 150},
};
constexpr Keyframe kTailNest[] = {
    {850, 142, 148, 114},  {1000, 162, 168, 62}, {600, 162, 168, 62},
    {1100, 110, 106, 128}, {1000, 58, 54, 170},  {1200, 90, 90, 150},
};
// Head-first greeting: acknowledge the person, then open the tail flag.
constexpr Keyframe kHeadFirstGreeting[] = {
    {480, 148, 90, 150}, {260, 148, 90, 140}, {650, 104, 24, 26},
    {780, 32, 160, 38},  {560, 76, 78, 102},  {950, 90, 90, 150},
};
// A low shy peek has a clear silhouette change when the tail opens overhead.
constexpr Keyframe kPeekThenFlag[] = {
    {520, 34, 30, 168},  {240, 34, 30, 168}, {720, 116, 44, 18},
    {680, 154, 158, 28}, {620, 54, 126, 76}, {950, 90, 90, 150},
};
// High flag: repeated broad sweeps carry the performance; head only accents.
constexpr Keyframe kTailCompass[] = {
    {700, 90, 18, 24},  {760, 110, 164, 20}, {760, 98, 18, 38},
    {760, 68, 164, 18}, {760, 84, 24, 42},   {1000, 90, 90, 150},
};
constexpr Keyframe kRibbonBow[] = {
    {700, 112, 18, 20}, {820, 76, 162, 72}, {840, 104, 24, 156},
    {820, 68, 158, 24}, {760, 108, 32, 88}, {1000, 90, 90, 150},
};
// Hold a curled tail while looking across the person, then gently unfurl.
constexpr Keyframe kCurledListening[] = {
    {500, 148, 90, 150}, {680, 136, 150, 64}, {300, 136, 150, 64},
    {820, 38, 150, 64},  {700, 74, 30, 24},   {1000, 90, 90, 150},
};
constexpr Keyframe kLeftRightInvitation[] = {
    {700, 112, 20, 38}, {800, 92, 164, 18}, {800, 68, 18, 48},
    {800, 88, 164, 22}, {800, 116, 24, 40}, {1000, 90, 90, 150},
};
constexpr Keyframe kFoodPeek[] = {
    {560, 56, 44, 168},  {480, 34, 44, 168}, {620, 136, 142, 134},
    {720, 146, 114, 34}, {650, 58, 32, 76},  {980, 90, 90, 150},
};
constexpr Keyframe kQuestionAnswer[] = {
    {650, 116, 24, 112}, {800, 102, 160, 18}, {800, 72, 22, 24},
    {800, 88, 162, 112}, {800, 108, 26, 18},  {1000, 90, 90, 150},
};
}  // namespace story_motion
#endif  // WZH_TAILREDPANDA_STORY_MOTION_H_
