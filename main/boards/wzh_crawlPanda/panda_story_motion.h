#ifndef WZH_CRAWLPANDA_STORY_MOTION_H_
#define WZH_CRAWLPANDA_STORY_MOTION_H_

// Direct head/left-paw/right-paw angles. Broad gestures alternate with a
// deliberate observation or offering beat; all stories finish sitting forward.
namespace story_motion {
using wzh_panda::Keyframe;

// Follow a butterfly high to one side, reach, then discover it on the other paw.
constexpr Keyframe kButterflyTrack[] = {
    {480, 145, 90, 90}, {620, 164, 30, 100}, {340, 164, 30, 100}, {780, 100, 65, 132},
    {720, 24, 85, 164}, {600, 52, 44, 140},  {780, 90, 90, 90},
};
// Gather bamboo, offer it to the user, bring it back for one last bite.
constexpr Keyframe kBambooOffer[] = {
    {620, 42, 26, 144},  {460, 74, 40, 158}, {560, 122, 60, 130}, {760, 106, 10, 170},
    {400, 106, 10, 170}, {580, 56, 52, 124}, {800, 90, 90, 90},
};
constexpr Keyframe kPeekaboo[] = {
    {620, 40, 150, 30},  {360, 40, 150, 30}, {740, 150, 28, 152},
    {440, 116, 60, 120}, {620, 26, 68, 170}, {800, 90, 90, 90},
};
constexpr Keyframe kWideSalute[] = {
    {650, 136, 40, 140}, {680, 164, 12, 168}, {380, 164, 12, 168},
    {880, 28, 22, 158},  {560, 64, 56, 124},  {850, 90, 90, 90},
};
// Offer one paw and wait; the other paw answers after eye contact.
constexpr Keyframe kPawHandshake[] = {
    {500, 138, 80, 100}, {720, 150, 12, 108}, {480, 150, 12, 108}, {560, 112, 44, 120},
    {760, 42, 76, 164},  {650, 70, 48, 140},  {900, 90, 90, 90},
};
constexpr Keyframe kPatACake[] = {
    {560, 130, 24, 110}, {620, 98, 58, 164},  {580, 32, 12, 128}, {660, 68, 66, 172},
    {720, 148, 18, 162}, {400, 148, 18, 162}, {850, 90, 90, 90},
};
constexpr Keyframe kScentTrail[] = {
    {820, 30, 56, 110},  {860, 52, 14, 132},  {380, 52, 14, 132}, {950, 108, 58, 154},
    {900, 158, 72, 170}, {750, 130, 42, 138}, {950, 90, 90, 90},
};
constexpr Keyframe kSleepyTuck[] = {
    {850, 144, 116, 64},  {1000, 158, 146, 32}, {600, 158, 146, 32},
    {1100, 112, 126, 54}, {900, 64, 108, 72},   {1100, 90, 90, 90},
};
// A glance leads the paw, then a deliberate held offer invites a response.
constexpr Keyframe kPawConversation[] = {
    {460, 142, 90, 90}, {660, 148, 14, 104}, {420, 100, 14, 104}, {600, 58, 52, 138},
    {720, 32, 82, 170}, {480, 112, 82, 170}, {900, 90, 90, 90},
};
constexpr Keyframe kBambooInspect[] = {
    {520, 42, 58, 116},  {580, 64, 18, 154}, {460, 116, 34, 136}, {380, 116, 34, 136},
    {680, 148, 72, 166}, {600, 78, 40, 126}, {880, 90, 90, 90},
};
constexpr Keyframe kShyInvitation[] = {
    {580, 32, 136, 46},  {320, 32, 136, 46}, {720, 122, 62, 142}, {620, 156, 20, 108},
    {400, 156, 20, 108}, {760, 58, 72, 158}, {850, 90, 90, 90},
};
constexpr Keyframe kPawFanfare[] = {
    {620, 128, 46, 118}, {680, 158, 12, 168}, {360, 158, 12, 168}, {680, 102, 52, 160},
    {760, 24, 18, 118},  {580, 62, 58, 140},  {900, 90, 90, 90},
};
constexpr Keyframe kFollowMyPaw[] = {
    {480, 138, 76, 104}, {700, 150, 12, 104}, {540, 62, 12, 104}, {620, 38, 58, 142},
    {740, 42, 82, 168},  {560, 134, 82, 168}, {920, 90, 90, 90},
};
constexpr Keyframe kPawExchange[] = {
    {540, 140, 14, 102}, {680, 108, 76, 170}, {600, 34, 12, 120}, {360, 68, 12, 120},
    {700, 44, 82, 172},  {660, 146, 22, 150}, {900, 90, 90, 90},
};
constexpr Keyframe kNearFarListen[] = {
    {620, 148, 80, 104}, {360, 148, 42, 104}, {700, 106, 24, 150}, {740, 32, 68, 122},
    {400, 32, 68, 166},  {680, 118, 48, 156}, {950, 90, 90, 90},
};
constexpr Keyframe kDrowsyStretch[] = {
    {850, 132, 124, 58}, {900, 156, 150, 28}, {520, 156, 150, 28}, {1000, 114, 60, 144},
    {620, 70, 36, 158},  {900, 52, 112, 72},  {1100, 90, 90, 90},
};
// Look toward an outstretched paw, pause, then follow the other paw back.
constexpr Keyframe kLookPastPaw[] = {
    {520, 104, 85, 96}, {690, 128, 45, 105}, {440, 128, 45, 105},
    {710, 82, 66, 136}, {580, 64, 89, 113},  {820, 90, 90, 90},
};
// Keep a supporting paw steady while the other brings the bamboo closer.
constexpr Keyframe kBambooTurn[] = {
    {600, 104, 55, 135}, {520, 112, 55, 111}, {550, 91, 68, 122},
    {520, 74, 78, 133},  {510, 79, 66, 136},  {780, 90, 90, 90},
};
// Reach with one paw, wait, then offer both without a fast alternating beat.
constexpr Keyframe kPawInvitation[] = {
    {500, 75, 89, 125},  {570, 79, 89, 125},  {640, 103, 51, 132},
    {470, 103, 51, 132}, {640, 108, 74, 111}, {790, 90, 90, 90},
};
// Two measured reaches with separate gaze targets, settling forward.
constexpr Keyframe kReachThenLookBack[] = {
    {460, 77, 84, 102},  {590, 67, 68, 151},  {560, 94, 48, 118},
    {650, 122, 40, 126}, {490, 104, 71, 114}, {800, 90, 90, 90},
};

// Short recordings get one readable gesture and a separate return to rest.
constexpr Keyframe kQuickPawLook[] = {
    {460, 114, 64, 101},
    {820, 90, 90, 90},
};
constexpr Keyframe kQuickLookBack[] = {
    {430, 71, 90, 112},
    {400, 104, 79, 103},
    {740, 90, 90, 90},
};
constexpr Keyframe kQuickBambooBite[] = {
    {470, 105, 64, 126},
    {420, 94, 73, 120},
    {750, 90, 90, 90},
};
constexpr Keyframe kQuickBambooOffer[] = {
    {490, 82, 57, 121},
    {490, 97, 68, 115},
    {770, 90, 90, 90},
};
constexpr Keyframe kQuickPawTap[] = {
    {450, 100, 70, 105},
    {430, 106, 81, 117},
    {720, 90, 90, 90},
};
constexpr Keyframe kQuickBrace[] = {
    {520, 96, 61, 130},
    {840, 90, 90, 90},
};
constexpr Keyframe kQuickTuck[] = {
    {560, 107, 106, 73},
    {990, 90, 90, 90},
};
}  // namespace story_motion
#endif  // WZH_CRAWLPANDA_STORY_MOTION_H_
