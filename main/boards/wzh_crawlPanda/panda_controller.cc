#include "panda_controller.h"
#include "wzh_body_commands.h"
#include "wzh_pet_feedback.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "application.h"
#include "config.h"
#include "mcp_server.h"
#include "panda_audio.h"
#include "wzh_sensors.h"

#define TAG "PandaMotion"

namespace {
constexpr uint32_t kTickMs = 20;
// 数组顺序必须与三路舵机顺序一致：头、左臂、右臂。
constexpr gpio_num_t kServoPins[] = {SERVO_HEAD_GPIO, SERVO_LEFT_ARM_GPIO, SERVO_RIGHT_ARM_GPIO};
constexpr ledc_channel_t kServoChannels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2};

int irnd(int count) {
    return count > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(count)) : 0;
}
int irand(int low, int high) { return low + irnd(high - low + 1); }
float frand(float low, float high) {
    return low + (static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX)) * (high - low);
}
float ease_motion(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

using wzh_panda::Behaviour;
using wzh_panda::Clip;
using wzh_panda::Keyframe;
using wzh_panda::Pose;
using wzh_panda::ShuffleBag;

constexpr size_t kBehaviourCount = static_cast<size_t>(Behaviour::kCount);

Pose TargetPose(const Clip& clip, uint8_t frame, bool mirror) {
    const Keyframe& key = clip.frames[frame];
    Pose pose{static_cast<float>(key.head), static_cast<float>(key.left_arm),
              static_cast<float>(key.right_arm)};
    if (mirror && clip.mirrorable) {
        pose.head = 180.0f - pose.head;
        const float old_left = pose.left_arm;
        pose.left_arm = 180.0f - pose.right_arm;
        pose.right_arm = 180.0f - old_left;
    }
    if (clip.authored_dynamics) {
        pose.head = wzh::PandaHeadTarget(wzh::StoryAngle(pose.head, 90, 8, 172, 1.45f));
        pose.left_arm = wzh::StoryAngle(pose.left_arm, 90, 8, 172, 1.45f);
        pose.right_arm = wzh::StoryAngle(pose.right_arm, 90, 8, 172, 1.45f);
        return pose;
    }
    pose.head = wzh::PandaHeadTarget(wzh::ActionAngle(pose.head, 90.0f, 8.0f, 172.0f, 2.30f));
    pose.left_arm = wzh::ActionAngle(pose.left_arm, 90.0f, 8.0f, 172.0f, 2.30f);
    pose.right_arm = wzh::ActionAngle(pose.right_arm, 90.0f, 8.0f, 172.0f, 2.30f);
    return pose;
}

// ============================================================================
// 产品编舞：每段有观察、接近、回应与收势，头与前爪错拍配合。
// Keyframe 顺序为 duration_ms / head / left_arm / right_arm。
// head: 0=右 90=前 180=左；left_arm: 15，爬行方向 180°→0°（角度减小）；
// right_arm: 16，爬行方向 0°→180°（角度增大）。默认坐姿 90/90/90。
// ============================================================================
constexpr Keyframe kIdleListen[] = {
    {550, 118, 82, 98}, {420, 138, 65, 110}, {650, 108, 75, 130},
    {460, 65, 85, 112}, {700, 100, 76, 104}, {620, 90, 90, 90},
};
constexpr Keyframe kIdlePawAdjust[] = {
    {550, 64, 80, 100},  {420, 76, 42, 112}, {650, 118, 68, 142},
    {460, 104, 112, 72}, {700, 90, 90, 90},
};
constexpr Keyframe kIdleBreath[] = {
    {800, 126, 58, 118}, {980, 56, 96, 152}, {760, 138, 44, 88},
    {1100, 68, 92, 144}, {850, 90, 90, 90},
};
constexpr Keyframe kIdleStretch[] = {
    {550, 118, 112, 68}, {420, 142, 142, 38}, {650, 108, 110, 70},
    {460, 68, 42, 138},  {700, 112, 28, 156}, {620, 90, 90, 90},
};
constexpr Keyframe kCuriousPeek[] = {
    {550, 116, 82, 98}, {420, 146, 62, 100}, {650, 132, 38, 118},
    {460, 66, 74, 138}, {700, 106, 65, 108}, {620, 90, 90, 90},
};
constexpr Keyframe kCuriousReach[] = {
    {550, 64, 82, 100},  {420, 48, 35, 114},  {650, 80, 58, 100},
    {460, 126, 75, 150}, {700, 110, 45, 128}, {620, 90, 90, 90},
};
constexpr Keyframe kCuriousDoubleCheck[] = {
    {550, 122, 70, 96}, {420, 144, 42, 108}, {650, 104, 68, 118}, {460, 58, 88, 152},
    {700, 74, 76, 122}, {620, 118, 48, 112}, {760, 90, 90, 90},
};
constexpr Keyframe kEatHoldBamboo[] = {
    {550, 116, 65, 112}, {420, 108, 38, 142}, {650, 72, 52, 128}, {460, 122, 46, 148},
    {700, 84, 62, 118},  {620, 105, 58, 130}, {760, 90, 90, 90},
};
constexpr Keyframe kEatChew[] = {
    {520, 112, 48, 132}, {360, 76, 62, 118}, {440, 116, 40, 142}, {380, 72, 56, 126},
    {550, 126, 52, 152}, {420, 96, 72, 116}, {760, 90, 90, 90},
};
constexpr Keyframe kEatPawSwap[] = {
    {550, 64, 38, 118}, {420, 90, 58, 142},  {650, 122, 78, 156}, {460, 110, 42, 124},
    {700, 76, 60, 148}, {620, 102, 68, 116}, {760, 90, 90, 90},
};
constexpr Keyframe kBabyCall[] = {
    {550, 62, 80, 108}, {420, 126, 62, 126}, {650, 106, 38, 106},
    {460, 68, 72, 148}, {700, 124, 52, 132}, {620, 90, 90, 90},
};
constexpr Keyframe kBabyPaddle[] = {
    {550, 122, 40, 104}, {420, 102, 70, 152}, {650, 65, 28, 124},
    {460, 82, 62, 156},  {700, 124, 42, 138}, {620, 90, 90, 90},
};
constexpr Keyframe kBabyFindUser[] = {
    {550, 64, 80, 104},  {420, 48, 54, 116},  {650, 90, 70, 138},
    {460, 138, 40, 156}, {700, 116, 62, 132}, {620, 90, 90, 90},
};
constexpr Keyframe kProudPushUp[] = {
    {550, 114, 72, 108}, {420, 126, 42, 138}, {650, 146, 26, 154}, {460, 116, 58, 124},
    {700, 64, 38, 142},  {620, 110, 62, 118}, {760, 90, 90, 90},
};
constexpr Keyframe kProudPatrol[] = {
    {550, 62, 42, 104},  {420, 46, 62, 138},  {650, 96, 76, 156}, {460, 138, 36, 126},
    {700, 150, 58, 148}, {620, 112, 70, 110}, {760, 90, 90, 90},
};
constexpr Keyframe kProudCall[] = {
    {550, 116, 74, 106}, {420, 142, 48, 136}, {650, 122, 28, 156},
    {460, 66, 52, 142},  {700, 114, 38, 122}, {620, 90, 90, 90},
};
constexpr Keyframe kAffectionHug[] = {
    {500, 116, 80, 100}, {460, 136, 58, 122}, {700, 108, 36, 144},
    {520, 72, 54, 130},  {620, 114, 46, 138}, {800, 90, 90, 90},
};
constexpr Keyframe kAffectionShy[] = {
    {550, 66, 108, 72},  {420, 52, 126, 54},  {650, 104, 92, 108},
    {460, 132, 48, 138}, {700, 110, 64, 120}, {620, 90, 90, 90},
};
constexpr Keyframe kAffectionAskUp[] = {
    {550, 116, 108, 72}, {420, 140, 132, 48}, {650, 112, 98, 90},
    {460, 76, 40, 140},  {700, 120, 30, 152}, {620, 90, 90, 90},
};
// Stroke landmarks for catalogue/target inspection. These three crawl clips
// are played by CrawlPlaybackPose's continuous reference oscillator, not by
// the generic keyframe timing/amplitude transforms.
constexpr Keyframe kPlayfulCrawl[] = {
    {400, 90, 90, 172},
    {400, 118, 8, 90},
    {400, 62, 90, 172},
    {400, 90, 90, 90},
};
constexpr Keyframe kCrawlTogether[] = {
    {600, 118, 8, 172},
    {600, 90, 90, 90},
    {600, 62, 8, 172},
    {600, 90, 90, 90},
};
constexpr Keyframe kPlayfulPawDrum[] = {
    {440, 116, 42, 100}, {360, 104, 76, 152}, {460, 68, 26, 120},
    {380, 80, 64, 156},  {520, 126, 36, 136}, {720, 90, 90, 90},
};
constexpr Keyframe kPlayfulPounce[] = {
    {550, 66, 122, 58},  {420, 80, 144, 36}, {650, 122, 72, 110}, {460, 142, 26, 154},
    {700, 106, 54, 136}, {620, 66, 40, 152}, {760, 90, 90, 90},
};
constexpr Keyframe kAmbientListen[] = {
    {760, 120, 76, 104}, {1000, 140, 54, 124}, {840, 108, 70, 144},
    {720, 64, 42, 120},  {880, 82, 65, 136},   {950, 90, 90, 90},
};
constexpr Keyframe kAmbientRain[] = {
    {900, 118, 110, 65}, {760, 68, 78, 128},   {1100, 104, 38, 146},
    {820, 78, 64, 156},  {1000, 118, 50, 122}, {1000, 90, 90, 90},
};
constexpr Keyframe kAmbientExplore[] = {
    {820, 66, 48, 104},  {900, 82, 78, 146}, {760, 132, 54, 160},
    {880, 110, 28, 122}, {900, 56, 62, 140}, {1000, 90, 90, 90},
};
constexpr Keyframe kAmbientStretch[] = {
    {800, 116, 106, 72}, {1000, 138, 140, 40}, {760, 106, 105, 82},
    {920, 68, 40, 140},  {780, 114, 26, 156},  {1000, 90, 90, 90},
};
constexpr Keyframe kSettle[] = {
    {800, 112, 66, 114},
    {1000, 78, 74, 126},
    {1100, 100, 82, 106},
    {1200, 90, 90, 90},
};

// Personality variations keep distinct spatial motifs, not just random tempo.
constexpr Keyframe kCuriousPawSearch[] = {
    {550, 118, 76, 104}, {420, 134, 40, 116}, {650, 108, 62, 96},
    {460, 64, 80, 140},  {700, 86, 54, 126},  {620, 90, 90, 90},
};
constexpr Keyframe kCuriousListenBack[] = {
    {550, 62, 82, 102},  {420, 48, 62, 124},  {650, 78, 78, 146},
    {460, 126, 48, 126}, {700, 110, 64, 112}, {620, 90, 90, 90},
};
constexpr Keyframe kAffectionPawCuddle[] = {
    {550, 116, 64, 114}, {420, 132, 40, 138}, {650, 104, 58, 122},
    {460, 74, 48, 144},  {700, 110, 66, 118}, {620, 90, 90, 90},
};
constexpr Keyframe kAffectionOpenHug[] = {
    {550, 66, 118, 62},  {420, 82, 98, 80},   {650, 118, 44, 136},
    {460, 136, 34, 150}, {700, 106, 62, 122}, {620, 90, 90, 90},
};
constexpr Keyframe kPlayfulInvite[] = {
    {550, 118, 40, 100}, {420, 138, 72, 116}, {650, 104, 52, 146}, {460, 64, 84, 158},
    {700, 80, 40, 128},  {620, 116, 62, 116}, {760, 90, 90, 90},
};
constexpr Keyframe kPlayfulWeightShift[] = {
    {520, 118, 38, 116}, {380, 100, 64, 152}, {560, 64, 86, 142},
    {400, 80, 50, 116},  {620, 128, 34, 140}, {760, 90, 90, 90},
};

#include "panda_reference_motion.h"
#include "panda_story_motion.h"

enum ClipId {
    C_IDLE_LISTEN,
    C_IDLE_PAW_ADJUST,
    C_IDLE_BREATH,
    C_IDLE_STRETCH,
    C_CURIOUS_PEEK,
    C_CURIOUS_REACH,
    C_CURIOUS_DOUBLE_CHECK,
    C_EAT_HOLD,
    C_EAT_CHEW,
    C_EAT_PAW_SWAP,
    C_BABY_CALL,
    C_BABY_PADDLE,
    C_BABY_FIND_USER,
    C_PROUD_PUSH_UP,
    C_PROUD_PATROL,
    C_PROUD_CALL,
    C_AFFECTION_HUG,
    C_AFFECTION_SHY,
    C_AFFECTION_ASK_UP,
    C_PLAYFUL_CRAWL,
    C_PLAYFUL_CRAWL_BOTH,
    C_PLAYFUL_PAW_DRUM,
    C_PLAYFUL_POUNCE,
    C_AMBIENT_LISTEN,
    C_AMBIENT_RAIN,
    C_AMBIENT_EXPLORE,
    C_AMBIENT_STRETCH,
    C_SETTLE,
    C_CURIOUS_PAW_SEARCH,
    C_CURIOUS_LISTEN_BACK,
    C_AFFECTION_PAW_CUDDLE,
    C_AFFECTION_OPEN_HUG,
    C_PLAYFUL_INVITE,
    C_PLAYFUL_WEIGHT_SHIFT,
    REF_IDLE_LISTEN,
    REF_IDLE_PAW_ADJUST,
    REF_IDLE_BREATH,
    REF_IDLE_STRETCH,
    REF_CURIOUS_PEEK,
    REF_CURIOUS_REACH,
    REF_CURIOUS_DOUBLE_CHECK,
    REF_EAT_HOLD_BAMBOO,
    REF_EAT_CHEW,
    REF_EAT_PAW_SWAP,
    REF_BABY_CALL,
    REF_BABY_PADDLE,
    REF_BABY_FIND_USER,
    REF_PROUD_PUSH_UP,
    REF_PROUD_PATROL,
    REF_PROUD_CALL,
    REF_AFFECTION_HUG,
    REF_AFFECTION_SHY,
    REF_AFFECTION_ASK_UP,
    REF_PLAYFUL_CRAWL,
    REF_PLAYFUL_PAW_DRUM,
    REF_PLAYFUL_POUNCE,
    REF_AMBIENT_LISTEN,
    REF_AMBIENT_RAIN,
    REF_AMBIENT_EXPLORE,
    REF_AMBIENT_STRETCH,
    REF_SETTLE,
    STORY_BUTTERFLY_TRACK,
    STORY_BAMBOO_OFFER,
    STORY_PEEKABOO,
    STORY_WIDE_SALUTE,
    STORY_PAW_HANDSHAKE,
    STORY_PAT_A_CAKE,
    STORY_SCENT_TRAIL,
    STORY_SLEEPY_TUCK,
    STORY_PAW_CONVERSATION,
    STORY_BAMBOO_INSPECT,
    STORY_SHY_INVITATION,
    STORY_PAW_FANFARE,
    STORY_FOLLOW_MY_PAW,
    STORY_PAW_EXCHANGE,
    STORY_NEAR_FAR_LISTEN,
    STORY_DROWSY_STRETCH,
    C_COUNT,
};

constexpr Clip kClips[C_COUNT] = {
    {"idle_listen", kIdleListen, std::size(kIdleListen), true},
    {"idle_paw_adjust", kIdlePawAdjust, std::size(kIdlePawAdjust), true},
    {"idle_breath", kIdleBreath, std::size(kIdleBreath), true},
    {"idle_stretch", kIdleStretch, std::size(kIdleStretch), true},
    {"curious_peek", kCuriousPeek, std::size(kCuriousPeek), true},
    {"curious_reach", kCuriousReach, std::size(kCuriousReach), true},
    {"curious_double_check", kCuriousDoubleCheck, std::size(kCuriousDoubleCheck), true},
    {"eat_hold_bamboo", kEatHoldBamboo, std::size(kEatHoldBamboo), true},
    {"eat_chew", kEatChew, std::size(kEatChew), true},
    {"eat_paw_swap", kEatPawSwap, std::size(kEatPawSwap), true},
    {"baby_call", kBabyCall, std::size(kBabyCall), true},
    {"baby_paddle", kBabyPaddle, std::size(kBabyPaddle), true},
    {"baby_find_user", kBabyFindUser, std::size(kBabyFindUser), true},
    {"proud_push_up", kProudPushUp, std::size(kProudPushUp), true},
    {"proud_patrol", kProudPatrol, std::size(kProudPatrol), true},
    {"proud_call", kProudCall, std::size(kProudCall), true},
    {"affection_hug", kAffectionHug, std::size(kAffectionHug), true},
    {"affection_shy", kAffectionShy, std::size(kAffectionShy), true},
    {"affection_ask_up", kAffectionAskUp, std::size(kAffectionAskUp), true},
    {"playful_crawl", kPlayfulCrawl, std::size(kPlayfulCrawl), false},
    {"playful_crawl_both", kCrawlTogether, std::size(kCrawlTogether), false},
    {"playful_paw_drum", kPlayfulPawDrum, std::size(kPlayfulPawDrum), true},
    {"playful_pounce", kPlayfulPounce, std::size(kPlayfulPounce), true},
    {"ambient_listen", kAmbientListen, std::size(kAmbientListen), true},
    {"ambient_rain", kAmbientRain, std::size(kAmbientRain), true},
    {"ambient_explore", kAmbientExplore, std::size(kAmbientExplore), true},
    {"ambient_stretch", kAmbientStretch, std::size(kAmbientStretch), true},
    {"settle", kSettle, std::size(kSettle), false},
    {"curious_paw_search", kCuriousPawSearch, std::size(kCuriousPawSearch), true},
    {"curious_listen_back", kCuriousListenBack, std::size(kCuriousListenBack), true},
    {"affection_paw_cuddle", kAffectionPawCuddle, std::size(kAffectionPawCuddle), true},
    {"affection_open_hug", kAffectionOpenHug, std::size(kAffectionOpenHug), true},
    {"playful_invite", kPlayfulInvite, std::size(kPlayfulInvite), true},
    {"playful_weight_shift", kPlayfulWeightShift, std::size(kPlayfulWeightShift), true},
    {"ref_idle_listen", reference_motion::kIdleListen, std::size(reference_motion::kIdleListen),
     true, true},
    {"ref_idle_paw_adjust", reference_motion::kIdlePawAdjust,
     std::size(reference_motion::kIdlePawAdjust), true, true},
    {"ref_idle_breath", reference_motion::kIdleBreath, std::size(reference_motion::kIdleBreath),
     true, true},
    {"ref_idle_stretch", reference_motion::kIdleStretch, std::size(reference_motion::kIdleStretch),
     true, true},
    {"ref_curious_peek", reference_motion::kCuriousPeek, std::size(reference_motion::kCuriousPeek),
     true, true},
    {"ref_curious_reach", reference_motion::kCuriousReach,
     std::size(reference_motion::kCuriousReach), true, true},
    {"ref_curious_double_check", reference_motion::kCuriousDoubleCheck,
     std::size(reference_motion::kCuriousDoubleCheck), true, true},
    {"ref_eat_hold_bamboo", reference_motion::kEatHoldBamboo,
     std::size(reference_motion::kEatHoldBamboo), true, true},
    {"ref_eat_chew", reference_motion::kEatChew, std::size(reference_motion::kEatChew), true, true},
    {"ref_eat_paw_swap", reference_motion::kEatPawSwap, std::size(reference_motion::kEatPawSwap),
     true, true},
    {"ref_baby_call", reference_motion::kBabyCall, std::size(reference_motion::kBabyCall), true,
     true},
    {"ref_baby_paddle", reference_motion::kBabyPaddle, std::size(reference_motion::kBabyPaddle),
     true, true},
    {"ref_baby_find_user", reference_motion::kBabyFindUser,
     std::size(reference_motion::kBabyFindUser), true, true},
    {"ref_proud_push_up", reference_motion::kProudPushUp, std::size(reference_motion::kProudPushUp),
     true, true},
    {"ref_proud_patrol", reference_motion::kProudPatrol, std::size(reference_motion::kProudPatrol),
     true, true},
    {"ref_proud_call", reference_motion::kProudCall, std::size(reference_motion::kProudCall), true,
     true},
    {"ref_affection_hug", reference_motion::kAffectionHug,
     std::size(reference_motion::kAffectionHug), true, true},
    {"ref_affection_shy", reference_motion::kAffectionShy,
     std::size(reference_motion::kAffectionShy), true, true},
    {"ref_affection_ask_up", reference_motion::kAffectionAskUp,
     std::size(reference_motion::kAffectionAskUp), true, true},
    {"ref_playful_crawl", reference_motion::kPlayfulCrawl,
     std::size(reference_motion::kPlayfulCrawl), true, true},
    {"ref_playful_paw_drum", reference_motion::kPlayfulPawDrum,
     std::size(reference_motion::kPlayfulPawDrum), true, true},
    {"ref_playful_pounce", reference_motion::kPlayfulPounce,
     std::size(reference_motion::kPlayfulPounce), true, true},
    {"ref_ambient_listen", reference_motion::kAmbientListen,
     std::size(reference_motion::kAmbientListen), true, true},
    {"ref_ambient_rain", reference_motion::kAmbientRain, std::size(reference_motion::kAmbientRain),
     true, true},
    {"ref_ambient_explore", reference_motion::kAmbientExplore,
     std::size(reference_motion::kAmbientExplore), true, true},
    {"ref_ambient_stretch", reference_motion::kAmbientStretch,
     std::size(reference_motion::kAmbientStretch), true, true},
    {"ref_settle", reference_motion::kSettle, std::size(reference_motion::kSettle), false, true},
    {"story_butterfly_track", story_motion::kButterflyTrack,
     std::size(story_motion::kButterflyTrack), true, true},
    {"story_bamboo_offer", story_motion::kBambooOffer, std::size(story_motion::kBambooOffer), true,
     true},
    {"story_peekaboo", story_motion::kPeekaboo, std::size(story_motion::kPeekaboo), true, true},
    {"story_wide_salute", story_motion::kWideSalute, std::size(story_motion::kWideSalute), true,
     true},
    {"story_paw_handshake", story_motion::kPawHandshake, std::size(story_motion::kPawHandshake),
     true, true},
    {"story_pat_a_cake", story_motion::kPatACake, std::size(story_motion::kPatACake), true, true},
    {"story_scent_trail", story_motion::kScentTrail, std::size(story_motion::kScentTrail), true,
     true},
    {"story_sleepy_tuck", story_motion::kSleepyTuck, std::size(story_motion::kSleepyTuck), true,
     true},
    {"story_pawconversation", story_motion::kPawConversation,
     std::size(story_motion::kPawConversation), true, true},
    {"story_bambooinspect", story_motion::kBambooInspect, std::size(story_motion::kBambooInspect),
     true, true},
    {"story_shyinvitation", story_motion::kShyInvitation, std::size(story_motion::kShyInvitation),
     true, true},
    {"story_pawfanfare", story_motion::kPawFanfare, std::size(story_motion::kPawFanfare), true,
     true},
    {"story_followmypaw", story_motion::kFollowMyPaw, std::size(story_motion::kFollowMyPaw), true,
     true},
    {"story_pawexchange", story_motion::kPawExchange, std::size(story_motion::kPawExchange), true,
     true},
    {"story_nearfarlisten", story_motion::kNearFarListen, std::size(story_motion::kNearFarListen),
     true, true},
    {"story_drowsystretch", story_motion::kDrowsyStretch, std::size(story_motion::kDrowsyStretch),
     true, true},
};

constexpr int8_t kFamily[kBehaviourCount][wzh_panda::kFamilyWidth] = {
    {C_IDLE_LISTEN, C_IDLE_PAW_ADJUST, C_IDLE_BREATH, C_IDLE_STRETCH, REF_IDLE_LISTEN,
     REF_IDLE_PAW_ADJUST, REF_IDLE_BREATH, REF_IDLE_STRETCH, -1, -1, -1, -1},
    {C_CURIOUS_PEEK, C_CURIOUS_REACH, C_CURIOUS_DOUBLE_CHECK, C_CURIOUS_PAW_SEARCH,
     C_CURIOUS_LISTEN_BACK, REF_CURIOUS_PEEK, REF_CURIOUS_REACH, REF_CURIOUS_DOUBLE_CHECK,
     STORY_BUTTERFLY_TRACK, STORY_PAW_CONVERSATION, -1, -1},
    {C_EAT_HOLD, C_EAT_CHEW, C_EAT_PAW_SWAP, REF_EAT_HOLD_BAMBOO, REF_EAT_CHEW, REF_EAT_PAW_SWAP,
     STORY_BAMBOO_OFFER, STORY_BAMBOO_INSPECT, -1, -1, -1, -1},
    {C_BABY_CALL, C_BABY_PADDLE, C_BABY_FIND_USER, REF_BABY_CALL, REF_BABY_PADDLE,
     REF_BABY_FIND_USER, STORY_PEEKABOO, STORY_SHY_INVITATION, -1, -1, -1, -1},
    {C_PROUD_PUSH_UP, C_PROUD_PATROL, C_PROUD_CALL, REF_PROUD_PUSH_UP, REF_PROUD_PATROL,
     REF_PROUD_CALL, STORY_WIDE_SALUTE, STORY_PAW_FANFARE, -1, -1, -1, -1},
    {C_AFFECTION_HUG, C_AFFECTION_SHY, C_AFFECTION_ASK_UP, C_AFFECTION_PAW_CUDDLE,
     C_AFFECTION_OPEN_HUG, REF_AFFECTION_HUG, REF_AFFECTION_SHY, REF_AFFECTION_ASK_UP,
     STORY_PAW_HANDSHAKE, STORY_FOLLOW_MY_PAW, -1, -1},
    {C_PLAYFUL_CRAWL, C_PLAYFUL_PAW_DRUM, C_PLAYFUL_POUNCE, C_PLAYFUL_INVITE,
     C_PLAYFUL_WEIGHT_SHIFT, REF_PLAYFUL_CRAWL, REF_PLAYFUL_PAW_DRUM, REF_PLAYFUL_POUNCE,
     STORY_PAT_A_CAKE, STORY_PAW_EXCHANGE, C_PLAYFUL_CRAWL_BOTH, -1},
    {C_AMBIENT_LISTEN, C_AMBIENT_RAIN, C_AMBIENT_EXPLORE, C_AMBIENT_STRETCH, REF_AMBIENT_LISTEN,
     REF_AMBIENT_RAIN, REF_AMBIENT_EXPLORE, REF_AMBIENT_STRETCH, STORY_SCENT_TRAIL,
     STORY_NEAR_FAR_LISTEN, -1, -1},
    {C_SETTLE, REF_SETTLE, STORY_SLEEPY_TUCK, STORY_DROWSY_STRETCH, -1, -1, -1, -1, -1, -1, -1, -1},
};
static_assert(C_COUNT < 128, "Clip IDs must fit the family shuffle bag");

// ============================================================================
// 身体部位级动作剪辑：从当前姿态起手，结束时回到默认姿态 90/90/90。
// 方向性动作（看左/看右）不可镜像，避免反向。
// ============================================================================
constexpr Keyframe kBodyShakeHead[] = {
    {450, 120, 90, 90}, {320, 160, 90, 90}, {450, 110, 90, 90},
    {450, 20, 90, 90},  {320, 60, 90, 90},  {600, 90, 90, 90},
};
constexpr Keyframe kBodyLookLeft[] = {
    {450, 120, 90, 90}, {480, 160, 90, 90}, {280, 160, 90, 90},
    {600, 110, 90, 90}, {700, 90, 90, 90},
};
constexpr Keyframe kBodyLookRight[] = {
    {450, 60, 90, 90}, {480, 20, 90, 90}, {280, 20, 90, 90}, {600, 70, 90, 90}, {700, 90, 90, 90},
};
constexpr Keyframe kBodyWaveLeft[] = {
    {750, 90, 30, 90}, {650, 90, 70, 90}, {750, 90, 24, 90}, {650, 90, 72, 90}, {1200, 90, 90, 90},
};
constexpr Keyframe kBodyWaveRight[] = {
    {750, 90, 90, 150}, {650, 90, 90, 110}, {750, 90, 90, 156},
    {650, 90, 90, 108}, {1200, 90, 90, 90},
};
constexpr Keyframe kBodyRaiseArms[] = {
    {950, 90, 30, 150},
    {650, 90, 30, 150},
    {900, 90, 46, 134},
    {1200, 90, 90, 90},
};

// Explicit joint commands: finite, directional, with a quiet centered finish.
constexpr Keyframe kJointHeadCenterFrames[] = {
    {1200, 90, 90, 90},
};
constexpr Keyframe kJointHeadSwayLeftFrames[] = {
    {650, 132, 90, 90}, {520, 108, 90, 90}, {650, 146, 90, 90},
    {500, 124, 90, 90}, {1000, 90, 90, 90},
};
constexpr Keyframe kJointHeadSwayRightFrames[] = {
    {650, 48, 90, 90}, {520, 72, 90, 90}, {650, 34, 90, 90}, {500, 56, 90, 90}, {1000, 90, 90, 90},
};
constexpr Keyframe kJointHeadScanFrames[] = {
    {900, 144, 90, 90}, {550, 144, 90, 90}, {1500, 36, 90, 90},
    {550, 36, 90, 90},  {1200, 90, 90, 90},
};
constexpr Keyframe kJointCenterFrames[] = {
    {1200, 90, 90, 90},
};
constexpr Keyframe kJointRaiseLeftFrames[] = {
    {900, 90, 24, 90},
    {700, 90, 24, 90},
    {1100, 90, 90, 90},
};
constexpr Keyframe kJointRaiseRightFrames[] = {
    {900, 90, 90, 156},
    {700, 90, 90, 156},
    {1100, 90, 90, 90},
};
constexpr Keyframe kJointLowerLeftFrames[] = {
    {1200, 90, 90, 90},
};
constexpr Keyframe kJointLowerRightFrames[] = {
    {1200, 90, 90, 90},
};
constexpr Keyframe kJointLowerArmsFrames[] = {
    {1200, 90, 90, 90},
};
constexpr Keyframe kJointWaveBothFrames[] = {
    {800, 90, 34, 146}, {650, 90, 70, 110}, {800, 90, 26, 154},
    {650, 90, 66, 114}, {1200, 90, 90, 90},
};
constexpr Keyframe kJointPawAlternateFrames[] = {
    {800, 90, 24, 90},  {950, 90, 90, 156}, {950, 90, 30, 90},
    {950, 90, 90, 150}, {1200, 90, 90, 90},
};
constexpr Keyframe kJointPawStretchFrames[] = {
    {1000, 90, 24, 156},
    {850, 90, 40, 140},
    {1000, 90, 20, 160},
    {1300, 90, 90, 90},
};
constexpr Keyframe kJointHugFrames[] = {
    {1000, 90, 26, 154},
    {1000, 90, 50, 130},
    {650, 90, 50, 130},
    {1300, 90, 90, 90},
};

enum BodyClipId {
    BODY_SHAKE_HEAD,
    BODY_LOOK_LEFT,
    BODY_LOOK_RIGHT,
    BODY_WAVE_LEFT,
    BODY_WAVE_RIGHT,
    BODY_RAISE_ARMS,
    BODY_CRAWL,
    BODY_CRAWL_BOTH,
    BODY_HEAD_CENTER,
    BODY_HEAD_SWAY_LEFT,
    BODY_HEAD_SWAY_RIGHT,
    BODY_HEAD_SCAN,
    BODY_CENTER,
    BODY_RAISE_LEFT,
    BODY_RAISE_RIGHT,
    BODY_LOWER_LEFT,
    BODY_LOWER_RIGHT,
    BODY_LOWER_ARMS,
    BODY_WAVE_BOTH,
    BODY_PAW_ALTERNATE,
    BODY_PAW_STRETCH,
    BODY_HUG,
    BODY_COUNT,
};
constexpr Clip kBodyClips[BODY_COUNT] = {
    {"body_shake_head", kBodyShakeHead, std::size(kBodyShakeHead), true},
    {"body_look_left", kBodyLookLeft, std::size(kBodyLookLeft), false},
    {"body_look_right", kBodyLookRight, std::size(kBodyLookRight), false},
    {"body_wave_left", kBodyWaveLeft, std::size(kBodyWaveLeft), false},
    {"body_wave_right", kBodyWaveRight, std::size(kBodyWaveRight), false},
    {"body_raise_arms", kBodyRaiseArms, std::size(kBodyRaiseArms), false},
    {"body_crawl", kPlayfulCrawl, std::size(kPlayfulCrawl), false},
    {"body_crawl_both", kCrawlTogether, std::size(kCrawlTogether), false},
    {"body_head_center", kJointHeadCenterFrames, std::size(kJointHeadCenterFrames), false, true},
    {"body_head_sway_left", kJointHeadSwayLeftFrames, std::size(kJointHeadSwayLeftFrames), false,
     true},
    {"body_head_sway_right", kJointHeadSwayRightFrames, std::size(kJointHeadSwayRightFrames), false,
     true},
    {"body_head_scan", kJointHeadScanFrames, std::size(kJointHeadScanFrames), false, true},
    {"body_center", kJointCenterFrames, std::size(kJointCenterFrames), false, true},
    {"body_raise_left", kJointRaiseLeftFrames, std::size(kJointRaiseLeftFrames), false, true},
    {"body_raise_right", kJointRaiseRightFrames, std::size(kJointRaiseRightFrames), false, true},
    {"body_lower_left", kJointLowerLeftFrames, std::size(kJointLowerLeftFrames), false, true},
    {"body_lower_right", kJointLowerRightFrames, std::size(kJointLowerRightFrames), false, true},
    {"body_lower_arms", kJointLowerArmsFrames, std::size(kJointLowerArmsFrames), false, true},
    {"body_wave_both", kJointWaveBothFrames, std::size(kJointWaveBothFrames), false, true},
    {"body_paw_alternate", kJointPawAlternateFrames, std::size(kJointPawAlternateFrames), false,
     true},
    {"body_paw_stretch", kJointPawStretchFrames, std::size(kJointPawStretchFrames), false, true},
    {"body_hug", kJointHugFrames, std::size(kJointHugFrames), false, true},
};

// Dedicated short acknowledgements: fixed pad identity, no random mirroring.
constexpr Keyframe kAck_touch1[] = {
    {550, 120, 82, 98}, {420, 140, 58, 120}, {650, 100, 72, 138},
    {460, 74, 64, 112}, {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch2[] = {
    {550, 116, 48, 100}, {420, 132, 68, 120}, {650, 108, 30, 112},
    {460, 80, 68, 132},  {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch3[] = {
    {550, 64, 80, 132},  {420, 48, 60, 112}, {650, 72, 68, 150},
    {460, 100, 48, 112}, {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch4[] = {
    {550, 116, 60, 120}, {420, 138, 36, 144}, {650, 78, 58, 126},
    {460, 104, 44, 138}, {700, 90, 90, 90},
};
constexpr Keyframe kAck_playful[] = {
    {400, 90, 90, 172},
    {400, 118, 8, 90},
    {400, 62, 90, 172},
    {400, 90, 90, 90},
};
constexpr Keyframe kAck_held[] = {
    {620, 118, 72, 108}, {650, 104, 54, 126}, {720, 76, 66, 116},
    {600, 108, 62, 122}, {850, 90, 90, 90},
};
constexpr Keyframe kAck_released[] = {
    {550, 116, 52, 110},
    {420, 132, 72, 138},
    {650, 86, 62, 122},
    {460, 90, 90, 90},
};
constexpr Keyframe kAck_pickup[] = {
    {550, 120, 72, 108}, {420, 140, 50, 130}, {650, 108, 62, 144},
    {460, 76, 78, 118},  {700, 90, 90, 90},
};
constexpr Keyframe kAck_shake[] = {
    {420, 62, 68, 112},  {400, 122, 52, 138}, {520, 78, 76, 148},
    {440, 106, 44, 122}, {760, 90, 90, 90},
};
constexpr Keyframe kAck_flip[] = {
    {550, 128, 112, 68}, {420, 142, 72, 118}, {650, 100, 40, 142},
    {460, 64, 62, 126},  {700, 90, 90, 90},
};
constexpr Keyframe kAck_settled[] = {
    {550, 116, 60, 112}, {420, 132, 72, 140}, {650, 104, 50, 124},
    {460, 76, 78, 108},  {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch1Alt[] = {
    {550, 62, 82, 100},  {420, 44, 62, 128}, {650, 82, 42, 116},
    {460, 116, 72, 140}, {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch2Alt[] = {
    {550, 130, 40, 108}, {420, 106, 80, 124}, {650, 142, 32, 132},
    {460, 112, 62, 104}, {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch3Alt[] = {
    {550, 50, 72, 140}, {420, 74, 56, 100}, {650, 38, 48, 148},
    {460, 68, 76, 118}, {700, 90, 90, 90},
};
constexpr Keyframe kAck_touch4Alt[] = {
    {550, 66, 56, 124}, {420, 44, 34, 148}, {650, 104, 68, 116},
    {460, 76, 44, 136}, {700, 90, 90, 90},
};
// Additional variants keep event identity while changing the spatial story.
constexpr Keyframe kRich_playful_alt[] = {
    {550, 120, 36, 100}, {420, 108, 72, 148}, {650, 64, 26, 118},
    {460, 82, 58, 156},  {700, 128, 42, 132}, {620, 90, 90, 90},
};
constexpr Keyframe kRich_held_alt[] = {
    {650, 62, 76, 104}, {700, 78, 52, 128}, {620, 112, 68, 116},
    {760, 82, 62, 122}, {850, 90, 90, 90},
};
constexpr Keyframe kRich_released_alt[] = {
    {550, 64, 78, 130},
    {420, 48, 52, 112},
    {650, 98, 66, 144},
    {460, 90, 90, 90},
};
constexpr Keyframe kRich_pickup_alt[] = {
    {550, 62, 78, 106},  {420, 44, 60, 130}, {650, 84, 38, 116},
    {460, 118, 66, 138}, {700, 90, 90, 90},
};
constexpr Keyframe kRich_shake_alt[] = {
    {420, 128, 70, 110}, {440, 68, 40, 130}, {500, 112, 76, 152},
    {420, 76, 58, 122},  {760, 90, 90, 90},
};
constexpr Keyframe kRich_flip_alt[] = {
    {550, 52, 114, 66},  {420, 38, 68, 122}, {650, 80, 46, 148},
    {460, 116, 72, 132}, {700, 90, 90, 90},
};
constexpr Keyframe kRich_settled_alt[] = {
    {550, 62, 76, 114},  {420, 46, 52, 132}, {650, 86, 68, 122},
    {460, 112, 58, 108}, {700, 90, 90, 90},
};
constexpr Keyframe kRich_touch1_story[] = {
    {550, 118, 80, 100}, {420, 144, 66, 114}, {650, 104, 38, 142},
    {460, 68, 72, 128},  {700, 112, 56, 114}, {620, 90, 90, 90},
};
constexpr Keyframe kRich_touch2_story[] = {
    {550, 118, 42, 100}, {420, 138, 72, 124}, {650, 108, 30, 142},
    {460, 132, 60, 112}, {700, 82, 72, 130},  {620, 90, 90, 90},
};
constexpr Keyframe kRich_touch3_story[] = {
    {550, 62, 80, 138}, {420, 42, 56, 108}, {650, 72, 38, 150},
    {460, 48, 68, 120}, {700, 98, 50, 108}, {620, 90, 90, 90},
};
constexpr Keyframe kRich_touch4_story[] = {
    {550, 66, 54, 126},  {420, 100, 36, 144}, {650, 136, 64, 112},
    {460, 106, 44, 152}, {700, 74, 62, 122},  {620, 90, 90, 90},
};
constexpr Keyframe kRich_playful_story[] = {
    {550, 120, 34, 100}, {420, 100, 72, 150}, {650, 62, 50, 128}, {460, 78, 84, 160},
    {700, 132, 28, 142}, {620, 106, 62, 112}, {760, 90, 90, 90},
};
constexpr Keyframe kRich_held_story[] = {
    {650, 116, 64, 116}, {760, 102, 48, 136}, {680, 76, 70, 122},
    {720, 108, 60, 110}, {900, 90, 90, 90},
};
constexpr Keyframe kRich_released_story[] = {
    {550, 118, 42, 100}, {420, 98, 68, 140}, {650, 64, 52, 120},
    {460, 112, 76, 128}, {700, 90, 90, 90},
};
constexpr Keyframe kRich_pickup_story[] = {
    {550, 118, 76, 104}, {420, 136, 54, 132}, {650, 100, 68, 146},
    {460, 64, 42, 118},  {700, 114, 72, 126}, {620, 90, 90, 90},
};
constexpr Keyframe kRich_shake_story[] = {
    {550, 64, 72, 108},  {420, 118, 44, 132}, {650, 76, 66, 150},
    {460, 126, 56, 118}, {700, 90, 90, 90},
};
constexpr Keyframe kRich_flip_story[] = {
    {550, 126, 108, 72}, {420, 100, 134, 46}, {650, 64, 82, 116},
    {460, 114, 38, 144}, {700, 78, 62, 128},  {620, 90, 90, 90},
};
constexpr Keyframe kRich_settled_story[] = {
    {550, 118, 62, 112}, {420, 100, 76, 140}, {650, 66, 44, 126},
    {460, 112, 68, 116}, {700, 90, 90, 90},
};

constexpr Clip kInteractionClips[] = {
    {"ack_touch1", kAck_touch1, std::size(kAck_touch1), false},
    {"ack_touch2", kAck_touch2, std::size(kAck_touch2), false},
    {"ack_touch3", kAck_touch3, std::size(kAck_touch3), false},
    {"ack_touch4", kAck_touch4, std::size(kAck_touch4), false},
    {"ack_playful", kAck_playful, std::size(kAck_playful), false},
    {"ack_held", kAck_held, std::size(kAck_held), false},
    {"ack_released", kAck_released, std::size(kAck_released), false},
    {"ack_pickup", kAck_pickup, std::size(kAck_pickup), false},
    {"ack_shake", kAck_shake, std::size(kAck_shake), false},
    {"ack_flip", kAck_flip, std::size(kAck_flip), false},
    {"ack_settled", kAck_settled, std::size(kAck_settled), false},
    {"ack_touch1_alt", kAck_touch1Alt, std::size(kAck_touch1Alt), false},
    {"ack_touch2_alt", kAck_touch2Alt, std::size(kAck_touch2Alt), false},
    {"ack_touch3_alt", kAck_touch3Alt, std::size(kAck_touch3Alt), false},
    {"ack_touch4_alt", kAck_touch4Alt, std::size(kAck_touch4Alt), false},
    {"ack_playful_alt", kRich_playful_alt, std::size(kRich_playful_alt), false},
    {"ack_held_alt", kRich_held_alt, std::size(kRich_held_alt), false},
    {"ack_released_alt", kRich_released_alt, std::size(kRich_released_alt), false},
    {"ack_pickup_alt", kRich_pickup_alt, std::size(kRich_pickup_alt), false},
    {"ack_shake_alt", kRich_shake_alt, std::size(kRich_shake_alt), false},
    {"ack_flip_alt", kRich_flip_alt, std::size(kRich_flip_alt), false},
    {"ack_settled_alt", kRich_settled_alt, std::size(kRich_settled_alt), false},
    {"ack_touch1_story", kRich_touch1_story, std::size(kRich_touch1_story), false},
    {"ack_touch2_story", kRich_touch2_story, std::size(kRich_touch2_story), false},
    {"ack_touch3_story", kRich_touch3_story, std::size(kRich_touch3_story), false},
    {"ack_touch4_story", kRich_touch4_story, std::size(kRich_touch4_story), false},
    {"ack_playful_story", kRich_playful_story, std::size(kRich_playful_story), false},
    {"ack_held_story", kRich_held_story, std::size(kRich_held_story), false},
    {"ack_released_story", kRich_released_story, std::size(kRich_released_story), false},
    {"ack_pickup_story", kRich_pickup_story, std::size(kRich_pickup_story), false},
    {"ack_shake_story", kRich_shake_story, std::size(kRich_shake_story), false},
    {"ack_flip_story", kRich_flip_story, std::size(kRich_flip_story), false},
    {"ack_settled_story", kRich_settled_story, std::size(kRich_settled_story), false},
    {"ack-touch1-expressive", story_motion::kPawHandshake, std::size(story_motion::kPawHandshake),
     false, true},
    {"ack-touch2-expressive", story_motion::kBambooOffer, std::size(story_motion::kBambooOffer),
     false, true},
    {"ack-touch3-expressive", story_motion::kButterflyTrack,
     std::size(story_motion::kButterflyTrack), false, true},
    {"ack-touch4-expressive", story_motion::kWideSalute, std::size(story_motion::kWideSalute),
     false, true},
    {"ack-playful-expressive", story_motion::kPatACake, std::size(story_motion::kPatACake), false,
     true},
    {"ack-held-expressive", story_motion::kSleepyTuck, std::size(story_motion::kSleepyTuck), false,
     true},
    {"ack-released-expressive", story_motion::kPeekaboo, std::size(story_motion::kPeekaboo), false,
     true},
    {"ack-pickup-expressive", story_motion::kPawHandshake, std::size(story_motion::kPawHandshake),
     false, true},
    {"ack-shake-expressive", story_motion::kButterflyTrack,
     std::size(story_motion::kButterflyTrack), false, true},
    {"ack-flip-expressive", story_motion::kScentTrail, std::size(story_motion::kScentTrail), false,
     true},
    {"ack-settled-expressive", story_motion::kSleepyTuck, std::size(story_motion::kSleepyTuck),
     false, true},
    {"ack_touch1_lively", story_motion::kFollowMyPaw, std::size(story_motion::kFollowMyPaw), false,
     true},
    {"ack_touch2_lively", story_motion::kPawConversation, std::size(story_motion::kPawConversation),
     false, true},
    {"ack_touch3_lively", story_motion::kBambooInspect, std::size(story_motion::kBambooInspect),
     false, true},
    {"ack_touch4_lively", story_motion::kPawFanfare, std::size(story_motion::kPawFanfare), false,
     true},
    {"ack_playful_lively", story_motion::kPawExchange, std::size(story_motion::kPawExchange), false,
     true},
    {"ack_held_lively", story_motion::kDrowsyStretch, std::size(story_motion::kDrowsyStretch),
     false, true},
    {"ack_released_lively", story_motion::kNearFarListen, std::size(story_motion::kNearFarListen),
     false, true},
    {"ack_pickup_lively", story_motion::kShyInvitation, std::size(story_motion::kShyInvitation),
     false, true},
    {"ack_shake_lively", story_motion::kPawExchange, std::size(story_motion::kPawExchange), false,
     true},
    {"ack_flip_lively", story_motion::kPawConversation, std::size(story_motion::kPawConversation),
     false, true},
    {"ack_settled_lively", story_motion::kDrowsyStretch, std::size(story_motion::kDrowsyStretch),
     false, true},
};
static_assert(std::size(kInteractionClips) == wzh::InteractionMotionSelector::kClipCount);

// Arms match the enabled AUTO_RUN_CRAWL_TEST branch in test_crawl/main.
// The product adds coordinated gaze, an entry glance and a finite finish.
constexpr uint32_t kCrawlAltPeriodMs = 800;
constexpr uint32_t kCrawlBothPeriodMs = 1200;
constexpr uint32_t kCrawlTransitionMs = 400;
constexpr float kCrawlStrokeDeg = 82.0f;

bool IsCrawlClip(const Clip& clip) {
    return clip.frames == kPlayfulCrawl || clip.frames == kCrawlTogether ||
           clip.frames == kAck_playful;
}

// 0 = alternating, 1 = both arms together, 2 = diagonal follow, 3 = one paw at a time.
Pose CrawlStroke(int mode, uint32_t elapsed_ms) {
    const uint32_t period = mode == 1 ? kCrawlBothPeriodMs : kCrawlAltPeriodMs;
    const float phase = static_cast<float>(elapsed_ms % period) / period;
    const float s = std::sin(6.28318530718f * phase);
    const float stroke = .5f - .5f * std::cos(6.28318530718f * phase);
    switch (mode) {
        case 1:
            return {90, 90 - kCrawlStrokeDeg * stroke, 90 + kCrawlStrokeDeg * stroke};
        case 2:
            return {90, 90 - kCrawlStrokeDeg * s,
                    90 + kCrawlStrokeDeg * std::sin(6.28318530718f * phase + 1.5707963f)};
        case 3: {
            const bool left_half = phase < .5f;
            const float sweep = .5f - .5f * std::cos(12.5663706f * phase);
            return {90, left_half ? 90 - kCrawlStrokeDeg * sweep : 90,
                    left_half ? 90 : 90 + kCrawlStrokeDeg * sweep};
        }
        default:
            return {90, 90 - kCrawlStrokeDeg * stroke, 90 + kCrawlStrokeDeg * (1 - stroke)};
    }
}

uint32_t CrawlEntryDuration(const Pose& from, const Pose& to) {
    const float travel =
        std::max({std::abs(to.head - from.head), std::abs(to.left_arm - from.left_arm),
                  std::abs(to.right_arm - from.right_arm)});
    // A new request can interrupt a different extreme pose. Lengthen only the
    // entry in that case; the steady gait always keeps its reference period.
    return std::max(
        kCrawlTransitionMs,
        static_cast<uint32_t>(std::ceil(travel * 1875 / wzh::kPetServoSpeed / kTickMs)) * kTickMs);
}

Pose CrawlPlaybackPose(const Clip& clip, const Pose& origin, uint32_t elapsed, int mode,
                       bool& finished) {
    const bool both = mode == 1;
    const uint32_t cycles = clip.frames == kAck_playful ? 2 : (both ? 4 : 6);
    const uint32_t period = both ? kCrawlBothPeriodMs : kCrawlAltPeriodMs;
    const Pose start = CrawlStroke(mode, 0);
    const uint32_t entry = CrawlEntryDuration(origin, start);
    const uint32_t running_ms = period * cycles;
    const Pose rest{90, 90, 90};
    const float exit_travel =
        std::max({std::abs(start.head - rest.head), std::abs(start.left_arm - rest.left_arm),
                  std::abs(start.right_arm - rest.right_arm)});
    const uint32_t exit_ms = wzh::SettlingDuration(exit_travel, 100.f);
    finished = elapsed >= entry + running_ms + exit_ms;
    if (finished)
        return rest;
    if (elapsed >= entry && elapsed < entry + running_ms) {
        Pose pose = CrawlStroke(mode, elapsed - entry);
        pose.head += wzh_crawl::CrawlGaze(elapsed - entry, running_ms, period);
        return pose;
    }
    const bool entering = elapsed < entry;
    const Pose& from = entering ? origin : start;
    const Pose& to = entering ? start : rest;
    const float mix =
        ease_motion(entering ? static_cast<float>(elapsed) / entry
                             : static_cast<float>(elapsed - entry - running_ms) / exit_ms);
    const float entry_progress = std::min(1.0f, static_cast<float>(elapsed) / entry);
    const float glance = entering ? std::sin(3.14159265359f * entry_progress) : 0;
    const float anticipation = (origin.head > 90 ? -24 : 24) * glance * glance;
    return {from.head + (to.head - from.head) * mix + anticipation,
            from.left_arm + (to.left_arm - from.left_arm) * mix,
            from.right_arm + (to.right_arm - from.right_arm) * mix};
}

bool Contains(const char* text, const char* token) {
    return text != nullptr && std::strstr(text, token) != nullptr;
}
}  // namespace

bool PandaController::Initialize(PandaAudio* audio) {
    audio_ = audio;
    InitializeServos();
    action_queue_ = xQueueCreate(1, sizeof(ActionRequest));
    if (action_queue_ == nullptr ||
        xTaskCreate(TaskEntry, "panda_motion", 5120, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start panda motion task");
        servo_output_.Release();
        return false;
    }
    // The board singleton is still being constructed here, so defer local
    // audio until the application and AudioService are fully initialized.
    return true;
}

void PandaController::InitializeServos() {
    // 与旧工程 servo.cc 一致：先给舵机电源，再初始化 50Hz/13bit PWM 通道。
    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << SERVO_POWER_GPIO;
    power_config.mode = GPIO_MODE_OUTPUT;
    power_config.pull_up_en = GPIO_PULLUP_DISABLE;
    power_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&power_config));
    ESP_ERROR_CHECK(gpio_set_level(SERVO_POWER_GPIO, 0));

    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = SERVO_DUTY_RES;
    timer_config.timer_num = SERVO_TIMER;
    timer_config.freq_hz = SERVO_FREQ_HZ;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    for (size_t i = 0; i < std::size(kServoPins); ++i) {
        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = kServoPins[i];
        channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
        channel_config.channel = kServoChannels[i];
        channel_config.timer_sel = SERVO_TIMER;
        channel_config.duty = 0;
        channel_config.hpoint = 0;
        channel_config.flags = {.output_invert = 0};
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
    servo_output_.Initialize(SERVO_POWER_GPIO, kServoChannels, std::size(kServoChannels));
    SetServoAngles(SERVO_HEAD_DEFAULT, SERVO_LEFT_ARM_DEFAULT, SERVO_RIGHT_ARM_DEFAULT);
    ESP_LOGI(TAG, "Servos initialized");
}

void PandaController::SetServoAngles(int head, int left_arm, int right_arm) {
    const int angles[] = {head, left_arm, right_arm};
    servo_output_.Write(angles, std::size(angles));
}

void PandaController::OutputPose(const wzh_panda::Pose& requested) {
    const uint32_t now = esp_timer_get_time() / 1000;
    const auto state = pet_.Get(now);
    const float requested_gain = state.gentle || state.mode == wzh::PetMode::kQuiet ? .60f : 1.0f;
    const float gain = quiet_center_.active() ? 1.f
                       : clip_active_         ? clip_gain_ * requested_gain
                                              : requested_gain;
    // Voice accents must not alter traction or flatten the reference strokes.
    Pose target = clip_active_ && clip_ != nullptr && IsCrawlClip(*clip_) ? pose_ : requested;
    target.head =
        wzh::ExpressiveAngle(target.head, 90.0f, wzh::kPandaHeadMin, wzh::kPandaHeadMax, gain);
    target.left_arm = wzh::ExpressiveAngle(target.left_arm, 90.0f, 8.0f, 172.0f, gain);
    target.right_arm = wzh::ExpressiveAngle(target.right_arm, 90.0f, 8.0f, 172.0f, gain);
    const uint32_t elapsed =
        last_output_ms_ == 0 ? kTickMs : std::min<uint32_t>(now - last_output_ms_, 50);
    const float allowed = wzh::kPetServoSpeed * elapsed / 1000.0f;
    const float travel = std::max({std::abs(target.head - last_output_pose_.head),
                                   std::abs(target.left_arm - last_output_pose_.left_arm),
                                   std::abs(target.right_arm - last_output_pose_.right_arm)});
    const float mix = travel > allowed && travel > 0 ? allowed / travel : 1.0f;
    target.head = last_output_pose_.head + (target.head - last_output_pose_.head) * mix;
    target.left_arm =
        last_output_pose_.left_arm + (target.left_arm - last_output_pose_.left_arm) * mix;
    target.right_arm =
        last_output_pose_.right_arm + (target.right_arm - last_output_pose_.right_arm) * mix;
    const float angles[] = {target.head, target.left_arm, target.right_arm};
    if (servo_output_.WriteDegrees(angles, std::size(angles))) {
        last_output_pose_ = target;
        last_output_ms_ = now;
        if (now - last_pose_log_ms_ >= 500) {
            ESP_LOGD(TAG, "Servo command %s=%.1f/%.1f/%.1f gentle=%d faults=0x%lx",
                     "head/left/right", angles[0], angles[1], angles[2], state.gentle,
                     static_cast<unsigned long>(servo_output_.faults()));
            last_pose_log_ms_ = now;
        }
    }
}

uint32_t PandaController::FrameDuration(uint8_t frame) const {
    const Pose from = frame == 0 ? clip_origin_ : TargetPose(*clip_, frame - 1, mirror_);
    const Pose to = TargetPose(*clip_, frame, mirror_);
    const float travel =
        std::max({std::abs(to.head - from.head), std::abs(to.left_arm - from.left_arm),
                  std::abs(to.right_arm - from.right_arm)});
    const uint32_t requested = static_cast<uint32_t>(clip_->frames[frame].duration_ms * tempo_);
    if (travel > .1f && std::abs(to.head - 90) <= 2.f && std::abs(to.left_arm - 90) <= 2.f &&
        std::abs(to.right_arm - 90) <= 2.f)
        return std::max(requested, wzh::SettlingDuration(travel, 100.f));
    return clip_->authored_dynamics
               ? wzh::StoryFrameDuration(travel, requested, motion_seed_, frame)
               : wzh::PetFrameDuration(travel, requested);
}

int PandaController::ChooseClip(Behaviour behaviour) {
    const size_t behaviour_index = static_cast<size_t>(behaviour);
    ShuffleBag& bag = bags_[behaviour_index];
    if (bag.position >= bag.count) {
        bag.count = 0;
        bag.position = 0;
        for (int i = 0; i < wzh_panda::kFamilyWidth && kFamily[behaviour_index][i] >= 0; ++i) {
            bag.items[bag.count++] = kFamily[behaviour_index][i];
        }
        for (int i = bag.count - 1; i > 0; --i) {
            std::swap(bag.items[i], bag.items[irnd(i + 1)]);
        }
        if (bag.count > 1 && bag.items[0] == bag.previous) {
            std::swap(bag.items[0], bag.items[1]);
        }
    }
    const int selected = bag.items[bag.position++];
    bag.previous = static_cast<int8_t>(selected);
    return selected;
}

void PandaController::StartClip(int clip_id, uint32_t now_ms) {
    // 常规行为剪辑在 kClips；身体部位剪辑从 C_COUNT 起编号，索引 kBodyClips。
    const Clip* clip = nullptr;
    if (clip_id >= 0 && clip_id < static_cast<int>(C_COUNT)) {
        clip = &kClips[clip_id];
    } else if (clip_id >= static_cast<int>(C_COUNT) &&
               clip_id < static_cast<int>(C_COUNT) + BODY_COUNT) {
        clip = &kBodyClips[clip_id - C_COUNT];
    } else if (clip_id >= static_cast<int>(C_COUNT) + BODY_COUNT &&
               clip_id < static_cast<int>(C_COUNT) + BODY_COUNT +
                             static_cast<int>(std::size(kInteractionClips))) {
        clip = &kInteractionClips[clip_id - C_COUNT - BODY_COUNT];
    }
    if (clip == nullptr) {
        ESP_LOGW(TAG, "Ignoring invalid clip id %d", clip_id);
        return;
    }
    quiet_center_.Reset();
    pose_ = last_output_pose_;
    from_ = last_output_pose_;
    clip_origin_ = last_output_pose_;
    clip_gain_ = 1.0f;
    // Gentle/quiet gain is applied once in OutputPose. Expressive clips get a
    // small per-run amplitude variation; body commands stay exact.
    if (std::strncmp(clip->name, "body_", 5) != 0)
        clip_gain_ *= frand(1.0f, 1.08f);
    // Crawl gaits rotate through alternating, diagonal follow and single-paw
    // paddles; only the explicit "both paws" command uses synchronized arms.
    if (clip->frames == kCrawlTogether) {
        crawl_mode_ = 1;
    } else if (clip->frames == kAck_playful || clip_id >= C_COUNT) {
        crawl_mode_ = 0;
    } else if (clip->frames == kPlayfulCrawl) {
        const int pick = irnd(10);
        crawl_mode_ = pick == 0 ? 1 : pick < 6 ? 0 : pick < 8 ? 2 : 3;
    } else {
        crawl_mode_ = 0;
    }
    // Expressive clips get a small per-run amplitude variation; explicit body
    // commands keep their exact requested range.
    if (std::strncmp(clip->name, "body_", 5) != 0 && clip_gain_ >= 1.0f)
        clip_gain_ *= frand(1.0f, 1.08f);
    clip_ = clip;
    frame_ = 0;
    frame_started_ms_ = now_ms;
    // 一般动作保留小范围节奏变化；爬行固定参考周期，保证持续牵引。
    tempo_ = IsCrawlClip(*clip_) ? 1.0f : frand(0.85f, 1.15f);
    if (!clip->mirrorable) {
        mirror_ = false;
    } else {
        // 避免随机镜像造成长期单侧偏置。
        if (mirror_balance_ >= 2) {
            mirror_ = false;
        } else if (mirror_balance_ <= -2) {
            mirror_ = true;
        } else {
            mirror_ = irnd(2) != 0;
        }
        mirror_balance_ += mirror_ ? 1 : -1;
    }
    motion_seed_ = esp_random();
    frame_duration_ms_ = FrameDuration(0);
    if (!request_active_.load()) {
        running_generation_ = intent_generation_.load();
        running_speech_bound_ = false;
    }
    clip_active_ = true;
    ESP_LOGI(TAG, "clip=%s mirror=%d tempo=%.2f", clip->name, mirror_, static_cast<double>(tempo_));
}

void PandaController::UpdatePlayer(uint32_t now_ms) {
    if (!clip_active_ || clip_ == nullptr) {
        return;
    }
    if (IsCrawlClip(*clip_)) {
        bool finished = false;
        pose_ = CrawlPlaybackPose(*clip_, clip_origin_, now_ms - frame_started_ms_, crawl_mode_,
                                  finished);
        clip_active_ = !finished;
        return;
    }
    while (clip_active_ && now_ms - frame_started_ms_ >= frame_duration_ms_) {
        pose_ = TargetPose(*clip_, frame_, mirror_);
        from_ = pose_;
        frame_started_ms_ += frame_duration_ms_;
        if (++frame_ >= clip_->count) {
            clip_active_ = false;
            return;
        }
        frame_duration_ms_ = FrameDuration(frame_);
    }
    const Pose target = TargetPose(*clip_, frame_, mirror_);
    const Pose previous = frame_ == 0   ? from_
                          : frame_ == 1 ? clip_origin_
                                        : TargetPose(*clip_, frame_ - 2, mirror_);
    const Pose next = frame_ + 1 < clip_->count ? TargetPose(*clip_, frame_ + 1, mirror_) : target;
    const uint32_t before = frame_ == 0 ? frame_duration_ms_ : FrameDuration(frame_ - 1);
    const uint32_t after =
        frame_ + 1 < clip_->count ? FrameDuration(frame_ + 1) : frame_duration_ms_;
    const float t = static_cast<float>(now_ms - frame_started_ms_) / frame_duration_ms_;
    const float head_t = wzh_crawl::GestureProgress(t, 0, frame_, mirror_);
    const float left_t = wzh_crawl::GestureProgress(t, 1, frame_, mirror_);
    const float right_t = wzh_crawl::GestureProgress(t, 2, frame_, mirror_);
    pose_.head = wzh::MotionHermite(previous.head, from_.head, target.head, next.head, before,
                                    frame_duration_ms_, after, head_t);
    pose_.left_arm = wzh::MotionHermite(previous.left_arm, from_.left_arm, target.left_arm,
                                        next.left_arm, before, frame_duration_ms_, after, left_t);
    pose_.right_arm =
        wzh::MotionHermite(previous.right_arm, from_.right_arm, target.right_arm, next.right_arm,
                           before, frame_duration_ms_, after, right_t);
}

bool PandaController::QueueRandomSound(const char* category) {
    if (audio_ == nullptr || !audio_->PlayRandomSound(category)) {
        return false;
    }
    ESP_LOGI(TAG, "queued random [%s] sound", category);
    return true;
}

wzh_panda::Behaviour PandaController::ClassifySound(int index) const {
    if (audio_ == nullptr) {
        return Behaviour::kCurious;
    }
    const char* name = audio_->SoundName(index);
    const int duration = audio_->SoundDurationMs(index);
    switch (wzh::ClassifySoundKind(name, "animal")) {
        case wzh::SoundKind::kSleep:
            return Behaviour::kSettle;
        case wzh::SoundKind::kChew:
            return Behaviour::kEat;
        case wzh::SoundKind::kAffection:
            return Behaviour::kAffection;
        case wzh::SoundKind::kSearch:
            return Behaviour::kCurious;
        case wzh::SoundKind::kLaugh:
        case wzh::SoundKind::kStretch:
            return Behaviour::kPlayful;
        default:
            break;
    }
    if (Contains(name, "吃竹") || Contains(name, "咀嚼") || Contains(name, "eat") ||
        Contains(name, "chew")) {
        return Behaviour::kEat;
    }
    if (Contains(name, "宝宝") || Contains(name, "嘤") || Contains(name, "baby")) {
        return Behaviour::kBaby;
    }
    if (Contains(name, "撒娇") || Contains(name, "亲近") || Contains(name, "Cat") ||
        Contains(name, "cat")) {
        return Behaviour::kAffection;
    }
    if (Contains(name, "成年") || Contains(name, "威风") || Contains(name, "adult")) {
        return Behaviour::kProud;
    }
    if (duration > 6500) {
        return Behaviour::kBaby;
    }
    if (duration > 0 && duration < 1700) {
        return Behaviour::kCurious;
    }
    return irnd(100) < 50 ? Behaviour::kCurious : Behaviour::kPlayful;
}

void PandaController::AddMicroMotion(wzh_panda::Pose& pose, uint32_t now_ms, float life) {
    // 呼吸带动头部与前爪轻微起伏；两条不同频率的正弦避免节拍器感。
    breath_phase_ += 20.0f;
    if (breath_phase_ > breath_period_) {
        breath_phase_ = 0.0f;
        breath_period_ = static_cast<uint32_t>(irand(3000, 4600));
    }
    const float breath = std::sin(6.2831853f * breath_phase_ / static_cast<float>(breath_period_));
    pose.head += breath * 2.0f * life;
    pose.left_arm += breath * 2.0f * life;
    pose.right_arm -= breath * 1.4f * life;

    // 前爪的重量转移和呼吸错拍，动作落点之间仍有生命感。
    const float t = static_cast<float>(now_ms) * 0.001f;
    pose.left_arm += std::sin(t * 0.72f + 0.2f) * 3.0f * life;
    pose.right_arm += std::sin(t * 0.68f + 2.2f) * 3.0f * life;

    // 头部漫无目的地漂移凝视。
    if (now_ms >= gaze_next_ms_) {
        gaze_target_ = frand(-15.0f, 15.0f);
        gaze_next_ms_ = now_ms + static_cast<uint32_t>(irand(1300, 3900));
    }
    gaze_pos_ += (gaze_target_ - gaze_pos_) * 0.035f;
    pose.head += gaze_pos_ * life;
}

wzh_panda::Pose PandaController::ApplyAudioMotion(const wzh_panda::Pose& pose,
                                                  const PandaMotionData& audio, uint32_t now_ms) {
    const bool body_control = clip_active_ && clip_ && std::strncmp(clip_->name, "body_", 5) == 0;
    const auto offset = voice_gesture_.Update(audio.playing && !body_control, audio.level,
                                              audio.attack, now_ms, speech_intent_.load());
    return {pose.head + offset.head, pose.left_arm + offset.left, pose.right_arm + offset.right};
}

void PandaController::TaskEntry(void* arg) { static_cast<PandaController*>(arg)->Task(); }

void PandaController::ParkForShutdown() {
    request_active_.store(false);
    speaking_.store(false);
    speech_tail_.store(false);
    clip_active_ = false;
    const auto origin = last_output_pose_;
    const Pose rest = {SERVO_HEAD_DEFAULT, SERVO_LEFT_ARM_DEFAULT, SERVO_RIGHT_ARM_DEFAULT};
    const float travel =
        std::max({std::abs(origin.head - rest.head), std::abs(origin.left_arm - rest.left_arm),
                  std::abs(origin.right_arm - rest.right_arm)});
    const uint32_t duration = std::min<uint32_t>(2200, wzh::SettlingDuration(travel, 90.f));
    const uint32_t started = esp_timer_get_time() / 1000;
    quiet_center_.Reset();
    quiet_center_.Sample({origin.head, origin.left_arm, origin.right_arm},
                         {rest.head, rest.left_arm, rest.right_arm}, started);
    while (servo_output_.powered() && !servo_output_.blocked()) {
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() / 1000 - started);
        const float mix = wzh::SettlingProgress(float(elapsed) / duration);
        OutputPose(Pose{origin.head + (rest.head - origin.head) * mix,
                        origin.left_arm + (rest.left_arm - origin.left_arm) * mix,
                        origin.right_arm + (rest.right_arm - origin.right_arm) * mix});
        if (elapsed >= duration)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    servo_output_.SetFault(wzh::kShutdown, true);
}

cJSON* PandaController::RequestResult(bool accepted) const {
    return wzh::PetStatus(pet_mode(), servo_output_.faults(), servo_output_.powered(),
                          request_active_.load(), conversation_active_.load(), accepted);
}

void PandaController::CancelSoundMotion() {
    sound_motion_.Cancel(audio_ ? audio_->GetMotionData().sequence : 0);
}

int PandaController::ChooseOfflineSoundClip(int index, wzh::SoundKind kind, uint32_t duration_ms) {
    // Footsteps deserve real traction, not an arbitrary sitting gesture. Both
    // gaits retain the reference oscillator and the finite smooth entry/exit.
    if (kind == wzh::SoundKind::kSteps)
        return irnd(4) == 0 ? C_PLAYFUL_CRAWL_BOTH : C_PLAYFUL_CRAWL;
    Behaviour family = ClassifySound(index);
    if (duration_ms > 8000 && (kind == wzh::SoundKind::kCall || kind == wzh::SoundKind::kLaugh) &&
        irnd(100) < 35)
        family = irnd(2) ? Behaviour::kPlayful : Behaviour::kCurious;
    return ChooseClip(family);
}

bool PandaController::UpdateSoundMotion(uint32_t now) {
    if (audio_ == nullptr || conversation_active_.load()) {
        CancelSoundMotion();
        return false;
    }
    const auto sound = audio_->GetMotionData();
    // Offline animal recordings use the authored action library: large, rich
    // gestures instead of envelope-driven speech following. Long recordings
    // keep drawing new clips until the sound ends. Paired recordings keep
    // their own timeline; ambient scenes remain gentle background motion.
    if (sound.playing && sound.audible && sound.kind != wzh::SoundKind::kAmbient &&
        !wzh::UserPairAtIndex(sound.sound_index) && !request_active_.load()) {
        const uint32_t remaining =
            sound.duration_ms > sound.elapsed_ms ? sound.duration_ms - sound.elapsed_ms : 0;
        if (!clip_active_ && remaining > 700) {
            sound_motion_.Cancel(sound.sequence);
            StartClip(ChooseOfflineSoundClip(sound.sound_index, sound.kind, sound.duration_ms),
                      now);
        }
        return false;
    }
    const uint32_t random = sound_motion_.NeedsBegin(sound) ? esp_random() : 0;
    wzh::SoundPose out{};
    bool powered = false;
    if (!sound_motion_.Update(
            sound, wzh::PetMechanism::kCrawl,
            {last_output_pose_.head, last_output_pose_.left_arm, last_output_pose_.right_arm, 0, 0},
            now, out, powered, random))
        return false;

    clip_active_ = false;
    if (powered) {
        quiet_center_.Reset();
        pose_ = {out[0], out[1], out[2]};
        OutputPose(pose_);
    } else {
        servo_output_.Release();
    }
    return true;
}

void PandaController::Task() {
    const int animal_count = audio_ != nullptr ? audio_->SoundCountByCategory("animal") : 0;
    const int ambient_count = audio_ != nullptr ? audio_->SoundCountByCategory("ambient") : 0;
    ESP_LOGI(TAG, "living motion: animal=%d ambient=%d", animal_count, ambient_count);
    uint32_t now_ms = esp_log_timestamp();
    TickType_t motion_tick = xTaskGetTickCount();
    // Audio-first autonomous scores yield immediately to touch, voice and protection.
    while (true) {
        if (shutdown_requested_.load()) {
            xQueueReset(action_queue_);
            ParkForShutdown();
        }
        now_ms = esp_log_timestamp();
        if (stop_audio_requested_.exchange(false) && audio_ != nullptr) {
            audio_->StopPlayback();
            CancelSoundMotion();
            autonomous_playback_ = false;
        }
        if (center_requested_.exchange(false)) {
            CancelSoundMotion();
            quiet_center_.Reset();
            autonomous_playback_ = false;
            clip_active_ = false;
            request_active_.store(false);
            running_speech_bound_ = false;
        }
        PollInteraction(now_ms);
        const auto pet_state = pet_.Get(now_ms);
        if (MotionInterrupted()) {
            clip_active_ = false;
            request_active_.store(false);
            running_speech_bound_ = false;
        }
        const bool offline = !conversation_active_.load();
        if (servo_output_.blocked() || !pet_state.motion) {
            CancelSoundMotion();
            autoplay_.Busy(now_ms, esp_random());
            autonomous_playback_ = false;
            clip_active_ = false;
            request_active_.store(false);
            xQueueReset(action_queue_);
            if (audio_ != nullptr && audio_->GetMotionData().playing) {
                audio_->StopPlayback();
            }
            if (servo_output_.blocked()) {
                quiet_center_.Reset();
                servo_output_.Release();
            } else {
                // Quiet/rest/stop finish at neutral; protection still releases immediately.
                SettleToCenter(now_ms);
            }
            motion_tick = xTaskGetTickCount();
            vTaskDelayUntil(&motion_tick, pdMS_TO_TICKS(kTickMs));
            continue;
        }

        // 明确请求（情绪/MCP/按键/身体部位）优先级最高，在线时不播本地声音以免与 TTS 重叠。
        ActionRequest request;
        if (xQueueReceive(action_queue_, &request, 0) == pdTRUE &&
            now_ms - request.created_ms < 2500 && request.generation == intent_generation_.load() &&
            (!request.speech_bound || speaking_.load())) {
            if (autonomous_playback_ && audio_ != nullptr && audio_->Playing())
                audio_->StopPlayback();
            autonomous_playback_ = false;
            CancelSoundMotion();
            autoplay_.Busy(now_ms, esp_random());
            bool sound_started = false;
            running_generation_ = request.generation;
            running_priority_.store(request.priority);
            running_speech_bound_ = request.speech_bound;
            request_active_.store(true);
            // Keep an existing Flash call intact when touch replaces motion.
            // Stop/rest/conversation cancel audio through their explicit path.
            if (request.interaction != wzh::PetInteraction::kNone && !conversation_active_.load() &&
                audio_ != nullptr && pet_.CanPlayInteractionSound(now_ms)) {
                if (audio_->PlayInteractionSound(request.interaction)) {
                    pet_.InteractionSoundStarted(now_ms);
                    sound_started = true;
                }
            }
            if (sound_started) {
                clip_active_ = false;
            } else if (request.is_body) {
                auto interaction = request.interaction;
                if (interaction == wzh::PetInteraction::kPlayful &&
                    (pet_state.gentle || pet_state.mode != wzh::PetMode::kNormal)) {
                    interaction = wzh::PetInteraction::kTouch4;
                }
                const int clip_id =
                    interaction != wzh::PetInteraction::kNone
                        ? static_cast<int>(C_COUNT) + BODY_COUNT +
                              interaction_motion_.Select(interaction, esp_random())
                        : static_cast<int>(C_COUNT) +
                              BodyClipId(static_cast<PandaBodyAction>(request.action));
                StartClip(clip_id, now_ms);
            } else {
                if (request.with_local_sound && offline && pet_state.local_sound &&
                    audio_ != nullptr && pet_.CanPlayLocalSound(now_ms)) {
                    if (audio_->PlayForAction(request.action)) {
                        pet_.LocalSoundStarted(now_ms);
                        sound_started = true;
                    }
                }
                if (!sound_started)
                    StartClip(ChooseClip(ActionToBehaviour(request.action)), now_ms);
                else
                    clip_active_ = false;
            }
        }

        const bool local_motion = UpdateSoundMotion(now_ms);
        if (!local_motion && (audio_ == nullptr || !audio_->Playing()))
            autonomous_playback_ = false;
        if (autoplay_.Due(now_ms,
                          offline && pet_state.local_sound && !servo_output_.powered() &&
                              !clip_active_ && !request_active_.load() && !local_motion &&
                              (audio_ == nullptr || !audio_->Playing()),
                          esp_random())) {
            // No queued sound is accumulated while AudioService or Audio Hub is busy.
            if (audio_ != nullptr && audio_->PlayAutonomousSound(false)) {
                autonomous_playback_ = true;
                running_generation_ = intent_generation_.load();
                running_speech_bound_ = false;
            }
            autoplay_.Busy(now_ms, esp_random());
        }
        if (local_motion) {
            quiet_center_.Reset();
            request_active_.store(false);  // A touch can immediately take over an autonomous show.
            last_motion_ms_ = now_ms;
            if (xTaskGetTickCount() - motion_tick > pdMS_TO_TICKS(60))
                motion_tick = xTaskGetTickCount();
            vTaskDelayUntil(&motion_tick, pdMS_TO_TICKS(kTickMs));
            continue;
        }

        PandaMotionData motion = audio_ != nullptr ? audio_->GetMotionData() : PandaMotionData{};

        // 在线用真实 TTS 包络驱动带头、跟爪和换边；无声时不启动。
        if (!offline && (speaking_.load() || speech_tail_.load())) {
            PandaMotionData tts =
                audio_ != nullptr ? audio_->GetTtsMotionData() : PandaMotionData{};
            if (tts.playing) {
                motion = tts;
            } else if (!speaking_.load() && !tts.audible) {
                speech_tail_.store(false);
            }
        }

        if (offline) {
            motion = {};  // Offline gestures own their finite choreography.
        }
        // Idle life: when nothing else owns the output, start a small idle clip
        // every few seconds so the pet never freezes at neutral between sounds.
        const bool idle_eligible = !conversation_active_.load() && pet_state.motion &&
                                   !request_active_.load() && !clip_active_ && !motion.playing &&
                                   !local_motion && !servo_output_.powered();
        if (idle_life_.Due(now_ms, idle_eligible, esp_random())) {
            static constexpr Behaviour kIdleFamilies[] = {Behaviour::kIdle, Behaviour::kCurious,
                                                          Behaviour::kAmbient};
            Behaviour family = kIdleFamilies[irnd(std::size(kIdleFamilies))];
            if (irnd(100) < 20)
                family = Behaviour::kPlayful;
            StartClip(ChooseClip(family), now_ms);
            idle_life_.Busy(now_ms, esp_random());
        }
        UpdatePlayer(now_ms);
        if (!clip_active_) {
            request_active_.store(false);
            running_speech_bound_ = false;
        }
        Pose expressive = pose_;
        if (motion.playing &&
            (!request_active_.load() || running_priority_.load() != wzh::PetPriority::kExplicit)) {
            // Keep an emotion's bias, without letting an old two-paw listening
            // clip hide the independent conversational paw choreography.
            expressive = {90 + (pose_.head - 90) * .25f, 90 + (pose_.left_arm - 90) * .12f,
                          90 + (pose_.right_arm - 90) * .12f};
        }
        if (clip_active_ || motion.playing)
            AddMicroMotion(expressive, now_ms, motion.playing ? 0.8f : 1.0f);
        const wzh_panda::Pose out = ApplyAudioMotion(expressive, motion, now_ms);
        if (clip_active_ || motion.playing) {
            quiet_center_.Reset();
            last_motion_ms_ = now_ms;
            OutputPose(out);
        } else {
            SettleToCenter(now_ms);
        }
        if (xTaskGetTickCount() - motion_tick > pdMS_TO_TICKS(60))
            motion_tick = xTaskGetTickCount();
        vTaskDelayUntil(&motion_tick, pdMS_TO_TICKS(kTickMs));
    }
}

wzh_panda::Behaviour PandaController::ActionToBehaviour(PandaAction action) {
    switch (action) {
        case PandaAction::kAffection:
            return Behaviour::kAffection;
        case PandaAction::kHappy:
            return Behaviour::kPlayful;
        case PandaAction::kProud:
            return Behaviour::kProud;
        case PandaAction::kEat:
            return Behaviour::kEat;
        case PandaAction::kListen:
            return Behaviour::kAmbient;
        case PandaAction::kStartled:
            return Behaviour::kCurious;
        case PandaAction::kSleepy:
            return Behaviour::kSettle;
        case PandaAction::kDiscover:
        default:
            return Behaviour::kCurious;
    }
}

bool PandaController::QueueRequest(ActionRequest request, wzh::PetPriority priority) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (action_queue_ == nullptr || servo_output_.blocked() || shutdown_requested_.load() ||
        (request_active_.load() && priority != wzh::PetPriority::kExplicit &&
         priority <= running_priority_.load()) ||
        !pet_.Offer(priority, esp_timer_get_time() / 1000)) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    last_request_us_.store(now);
    if (!request.is_body && request.action == last_action_ && last_action_time_us_ > 0 &&
        last_action_generation_ == intent_generation_.load() &&
        now - last_action_time_us_ < 1200000) {
        return true;
    }
    request.created_ms = now / 1000;
    request.priority = priority;
    request.generation = intent_generation_.fetch_add(1) + 1;
    if (xQueueOverwrite(action_queue_, &request) != pdTRUE) {
        return false;
    }
    if (!request.is_body) {
        last_action_ = request.action;
        last_action_time_us_ = now;
        last_action_generation_ = request.generation;
    }
    return true;
}

bool PandaController::TriggerAction(PandaAction action, bool with_local_sound,
                                    wzh::PetPriority priority, bool speech_bound) {
    ActionRequest request{action, with_local_sound, false};
    request.speech_bound = speech_bound;
    return QueueRequest(request, priority);
}

bool PandaController::HandleBodyCommand(const std::string& name) {
    PandaBodyAction action;
    if (!ParseBodyAction(name, action))
        return false;
    const bool accepted = TriggerBodyAction(action);
    if (accepted) {
        ESP_LOGI(TAG, "Local joint command=%s accepted", BodyActionName(action));
    } else {
        ESP_LOGW(TAG, "Local joint command=%s rejected", BodyActionName(action));
    }
    return true;
}

bool PandaController::TriggerBodyAction(PandaBodyAction action) {
    if (static_cast<unsigned>(action) >= static_cast<unsigned>(PandaBodyAction::kCount)) {
        return false;
    }
    return QueueRequest(ActionRequest{static_cast<PandaAction>(action), false, true},
                        wzh::PetPriority::kExplicit);
}

int PandaController::BodyClipId(PandaBodyAction action) {
    static_assert(BODY_COUNT == static_cast<int>(PandaBodyAction::kCount));
    switch (action) {
        case PandaBodyAction::kShakeHead:
            return BODY_SHAKE_HEAD;
        case PandaBodyAction::kLookLeft:
            return BODY_LOOK_LEFT;
        case PandaBodyAction::kLookRight:
            return BODY_LOOK_RIGHT;
        case PandaBodyAction::kWaveLeft:
            return BODY_WAVE_LEFT;
        case PandaBodyAction::kWaveRight:
            return BODY_WAVE_RIGHT;
        case PandaBodyAction::kRaiseArms:
            return BODY_RAISE_ARMS;
        case PandaBodyAction::kCrawl:
            return BODY_CRAWL;
        case PandaBodyAction::kCrawlBoth:
            return BODY_CRAWL_BOTH;
        case PandaBodyAction::kHeadCenter:
            return BODY_HEAD_CENTER;
        case PandaBodyAction::kHeadSwayLeft:
            return BODY_HEAD_SWAY_LEFT;
        case PandaBodyAction::kHeadSwayRight:
            return BODY_HEAD_SWAY_RIGHT;
        case PandaBodyAction::kHeadScan:
            return BODY_HEAD_SCAN;
        case PandaBodyAction::kCenter:
            return BODY_CENTER;
        case PandaBodyAction::kRaiseLeft:
            return BODY_RAISE_LEFT;
        case PandaBodyAction::kRaiseRight:
            return BODY_RAISE_RIGHT;
        case PandaBodyAction::kLowerLeft:
            return BODY_LOWER_LEFT;
        case PandaBodyAction::kLowerRight:
            return BODY_LOWER_RIGHT;
        case PandaBodyAction::kLowerArms:
            return BODY_LOWER_ARMS;
        case PandaBodyAction::kWaveBoth:
            return BODY_WAVE_BOTH;
        case PandaBodyAction::kPawAlternate:
            return BODY_PAW_ALTERNATE;
        case PandaBodyAction::kPawStretch:
            return BODY_PAW_STRETCH;
        case PandaBodyAction::kHug:
            return BODY_HUG;
        default:
            return -1;
    }
}

bool PandaController::ParseBodyAction(const std::string& name, PandaBodyAction& action) {
    const auto token = wzh::CanonicalBodyCommand(name, wzh::kBodyCrawl);
    if (token.empty())
        return false;
    static const struct {
        const char* name;
        PandaBodyAction action;
    } actions[] = {
        {"shake_head", PandaBodyAction::kShakeHead},
        {"look_left", PandaBodyAction::kLookLeft},
        {"look_right", PandaBodyAction::kLookRight},
        {"wave_left", PandaBodyAction::kWaveLeft},
        {"wave_right", PandaBodyAction::kWaveRight},
        {"raise_arms", PandaBodyAction::kRaiseArms},
        {"crawl", PandaBodyAction::kCrawl},
        {"crawl_both", PandaBodyAction::kCrawlBoth},
        {"head_center", PandaBodyAction::kHeadCenter},
        {"head_sway_left", PandaBodyAction::kHeadSwayLeft},
        {"head_sway_right", PandaBodyAction::kHeadSwayRight},
        {"head_scan", PandaBodyAction::kHeadScan},
        {"center", PandaBodyAction::kCenter},
        {"raise_left", PandaBodyAction::kRaiseLeft},
        {"raise_right", PandaBodyAction::kRaiseRight},
        {"lower_left", PandaBodyAction::kLowerLeft},
        {"lower_right", PandaBodyAction::kLowerRight},
        {"lower_arms", PandaBodyAction::kLowerArms},
        {"wave_both", PandaBodyAction::kWaveBoth},
        {"paw_alternate", PandaBodyAction::kPawAlternate},
        {"paw_stretch", PandaBodyAction::kPawStretch},
        {"hug", PandaBodyAction::kHug},
    };
    for (const auto& item : actions) {
        if (token == item.name) {
            action = item.action;
            return true;
        }
    }
    return false;
}

const char* PandaController::BodyActionName(PandaBodyAction action) {
    switch (action) {
        case PandaBodyAction::kShakeHead:
            return "shake_head";
        case PandaBodyAction::kLookLeft:
            return "look_left";
        case PandaBodyAction::kLookRight:
            return "look_right";
        case PandaBodyAction::kWaveLeft:
            return "wave_left";
        case PandaBodyAction::kWaveRight:
            return "wave_right";
        case PandaBodyAction::kRaiseArms:
            return "raise_arms";
        case PandaBodyAction::kCrawl:
            return "crawl";
        case PandaBodyAction::kCrawlBoth:
            return "crawl_both";
        case PandaBodyAction::kHeadCenter:
            return "head_center";
        case PandaBodyAction::kHeadSwayLeft:
            return "head_sway_left";
        case PandaBodyAction::kHeadSwayRight:
            return "head_sway_right";
        case PandaBodyAction::kHeadScan:
            return "head_scan";
        case PandaBodyAction::kCenter:
            return "center";
        case PandaBodyAction::kRaiseLeft:
            return "raise_left";
        case PandaBodyAction::kRaiseRight:
            return "raise_right";
        case PandaBodyAction::kLowerLeft:
            return "lower_left";
        case PandaBodyAction::kLowerRight:
            return "lower_right";
        case PandaBodyAction::kLowerArms:
            return "lower_arms";
        case PandaBodyAction::kWaveBoth:
            return "wave_both";
        case PandaBodyAction::kPawAlternate:
            return "paw_alternate";
        case PandaBodyAction::kPawStretch:
            return "paw_stretch";
        case PandaBodyAction::kHug:
            return "hug";
        default:
            return "unknown";
    }
}

bool PandaController::IsMaintenanceBusy() const {
    const int64_t now = esp_timer_get_time();
    const int64_t requested = last_request_us_.load();
    return maintenance_.load() || now < 30000000 || request_active_.load() ||
           servo_output_.powered() || (audio_ != nullptr && audio_->Playing()) ||
           (requested > 0 && now - requested < 30000000);
}

void PandaController::SetConversationActive(bool active) {
    const bool changed = conversation_active_.exchange(active) != active;
    if (!changed) {
        return;
    }
    ESP_LOGI(TAG, "Mode changed to %s",
             active ? "online/conversation" : "offline/interaction-wait");
    if (!active) {
        speaking_.store(false);
        speech_tail_.store(false);
        speech_intent_.store(wzh::SpeechIntent::kNeutral);
        pending_expression_.store(-1);
        // Closing a conversation consumes its pending/current expressions.
        // A reconnect or an idle transition must not resume stale choreography.
        intent_generation_.fetch_add(1);
    }
    if (active && audio_ != nullptr) {
        audio_->StopPlayback();
    }
    // Connecting/listening stay centered; only actual speech starts voice motion.
}

void PandaController::SetSpeaking(bool speaking) {
    const bool was_speaking = speaking_.exchange(speaking);
    if (speaking)
        speech_tail_.store(true);
    if (was_speaking && !speaking) {
        pending_expression_.store(-1);
    }
}

bool PandaController::RequestCenter() {
    if (shutdown_requested_.load() || servo_output_.blocked())
        return false;
    intent_generation_.fetch_add(1);
    pending_expression_.store(-1);
    center_requested_.store(true);
    return true;
}

void PandaController::SettleToCenter(uint32_t now) {
    ApplyAudioMotion(pose_, {}, now);
    const auto sample = quiet_center_.Sample(
        {last_output_pose_.head, last_output_pose_.left_arm, last_output_pose_.right_arm},
        {90, 90, 90}, now, 100.f, 250);
    if (quiet_center_.powered(now)) {
        pose_ = {sample[0], sample[1], sample[2]};
        OutputPose(pose_);
    } else {
        servo_output_.Release();
    }
}

void PandaController::SetDefaultPose() { shutdown_requested_.store(true); }

PandaAction PandaController::EmotionToAction(const std::string& emotion) const {
    if (emotion == "sad" || emotion == "crying" || emotion == "depressed" || emotion == "lonely" ||
        emotion == "angry" || emotion == "frustrated" || emotion == "fearful" ||
        emotion == "scared" || emotion == "worried") {
        return PandaAction::kListen;
    }
    if (emotion == "happy" || emotion == "laughing" || emotion == "winking" || emotion == "funny" ||
        emotion == "excited" || emotion == "delighted" || emotion == "joyful" ||
        emotion == "proud") {
        return PandaAction::kHappy;
    }
    if (emotion == "loving" || emotion == "embarrassed" || emotion == "relaxed" ||
        emotion == "grateful" || emotion == "gratitude" || emotion == "love") {
        return PandaAction::kAffection;
    }
    if (emotion == "sad" || emotion == "crying" || emotion == "sleepy" || emotion == "tired" ||
        emotion == "bored" || emotion == "depressed" || emotion == "lonely") {
        return PandaAction::kSleepy;
    }
    if (emotion == "angry" || emotion == "confident" || emotion == "cool" || emotion == "annoyed" ||
        emotion == "frustrated") {
        return PandaAction::kProud;
    }
    if (emotion == "surprised" || emotion == "shocked" || emotion == "fearful" ||
        emotion == "scared" || emotion == "worried") {
        return PandaAction::kStartled;
    }
    if (emotion == "thinking" || emotion == "confused" || emotion == "curious") {
        return PandaAction::kDiscover;
    }
    return PandaAction::kDiscover;
}

void PandaController::HandleLlmEmotion(const char* emotion) {
    // 离线时 llm.emotion 已失去时效性，应继续由自主逻辑控制动作。
    if (!conversation_active_.load() || emotion == nullptr || emotion[0] == '\0') {
        return;
    }
    // neutral 是状态机/界面的复位信号，不是可表演的情绪，忽略它。
    if (std::strcmp(emotion, "neutral") == 0 || std::strcmp(emotion, "normal") == 0) {
        return;
    }
    if (wzh::ContainsAny(emotion, {"sad", "crying", "angry", "fear", "worried", "lonely"})) {
        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
    }
    expression_since_.store(esp_timer_get_time() / 1000);
    pending_expression_.store(static_cast<int>(EmotionToAction(emotion)));
}

void PandaController::HandleUserSpeechEmotion(const char* text) {
    if (!conversation_active_.load() || text == nullptr || text[0] == '\0')
        return;
    if (HandlePetControl(wzh::ParsePetControl(text)))
        return;
    // Explicit cloud body intent is executed by MCP, once.
    if (!wzh::MatchBodyCommand(text, wzh::kBodyCrawl).empty())
        return;
    React(wzh::ReactionFromText(text), wzh::PetPriority::kEmotion);
}

bool PandaController::ParseAction(const std::string& name, PandaAction& action) {
    static const struct {
        const char* name;
        PandaAction action;
    } actions[] = {{"discover", PandaAction::kDiscover}, {"affection", PandaAction::kAffection},
                   {"happy", PandaAction::kHappy},       {"proud", PandaAction::kProud},
                   {"eat", PandaAction::kEat},           {"listen", PandaAction::kListen},
                   {"startled", PandaAction::kStartled}, {"sleepy", PandaAction::kSleepy}};
    for (const auto& item : actions) {
        if (name == item.name) {
            action = item.action;
            return true;
        }
    }
    return false;
}

const char* PandaController::ActionName(PandaAction action) {
    switch (action) {
        case PandaAction::kDiscover:
            return "discover";
        case PandaAction::kAffection:
            return "affection";
        case PandaAction::kHappy:
            return "happy";
        case PandaAction::kProud:
            return "proud";
        case PandaAction::kEat:
            return "eat";
        case PandaAction::kListen:
            return "listen";
        case PandaAction::kStartled:
            return "startled";
        case PandaAction::kSleepy:
            return "sleepy";
    }
    return "unknown";
}

void PandaController::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.panda.get_pet_status",
        "查询宠物模式、运动保护、是否忙碌与对话状态。动作结果的accepted只代表请求接收，"
        "不代表实物执行完成；拒绝时解释reason，不要宣称做完。动作工具可传details=true返回详情。",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            return wzh::PetStatus(pet_mode(), servo_output_.faults(), servo_output_.powered(),
                                  request_active_.load(), conversation_active_.load());
        });
    mcp.AddTool("self.panda.pet_control",
                "宠物持续状态控制：stop立即停止，quiet安静陪伴，rest休息，wake恢复活动。"
                "普通动作和情绪不会自动解除stop/rest。",
                PropertyList({Property("command", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    const bool accepted = HandlePetControl(
                        wzh::ParsePetControl(properties["command"].value<std::string>()));
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    mcp.AddTool("self.panda.react_to_user_emotion",
                "让爬爬熊猫根据用户此刻的情绪做出肢体动作。"
                "设备是一只爬爬熊猫机器人，通过转头、摆弄前肢来回应情绪。"
                "用户情绪明显且需要肢体回应时使用；每轮适量调用，避免重复表演。"
                "判断依据包括用户措辞、语义、上下文以及语音情绪线索；例如"
                "「我考了满分/太棒了/好开心」→ happy，「吓死我了/天啊」→ surprised，"
                "「好难过/想哭」→ sad（温和倾听），「气死我了」→ angry，「好喜欢你」→ loving，"
                "「好困/想睡觉」→ sleepy。"
                "emotion 取值：happy/loving/sad/angry/surprised/thinking/neutral。"
                "情绪不明显或不确定时传 neutral。",
                PropertyList({Property("emotion", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    if (!conversation_active_.load()) {
                        return "offline mode: emotion control is paused";
                    }
                    const auto emotion = properties["emotion"].value<std::string>();
                    if (emotion == "neutral" || emotion == "normal") {
                        return false;
                    }
                    if (wzh::ContainsAny(emotion,
                                         {"sad", "crying", "angry", "fear", "worried", "lonely"})) {
                        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
                    }
                    const bool accepted =
                        TriggerAction(EmotionToAction(emotion), false, wzh::PetPriority::kEmotion);
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    mcp.AddTool(
        "self.panda.action",
        "执行一个明确的爬爬熊猫动作。仅在用户要求动作或当前语境强烈需要肢体表达时使用。action: "
        "discover, affection, happy, proud, eat, listen, startled, sleepy。",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("details", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& properties) -> ReturnValue {
            PandaAction action;
            if (!ParseAction(properties["action"].value<std::string>(), action)) {
                return "invalid action";
            }
            const bool accepted = TriggerAction(action, !conversation_active_.load());
            if (properties["details"].value<bool>())
                return RequestResult(accepted);
            return accepted;
        });
    mcp.AddTool("self.panda.body", wzh::BodyCommandHelp(wzh::kBodyCrawl),
                PropertyList({Property("action", kPropertyTypeString),
                              Property("details", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    PandaBodyAction action;
                    if (!ParseBodyAction(properties["action"].value<std::string>(), action)) {
                        return "invalid body action";
                    }
                    const bool accepted = TriggerBodyAction(action);
                    if (properties["details"].value<bool>())
                        return RequestResult(accepted);
                    return accepted;
                });
    mcp.AddTool("self.panda.body_capabilities", "查询实际支持的关节动作与动作名称；不执行动作。",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    return wzh::BodyCommandHelp(wzh::kBodyCrawl);
                });
    mcp.AddTool("self.panda.get_mode", "返回爬爬熊猫当前 online 或 offline 模式。", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    return std::string(conversation_active_.load() ? "online" : "offline");
                });
}

wzh::PetMode PandaController::pet_mode() const {
    return pet_.Get(esp_timer_get_time() / 1000).mode;
}

bool PandaController::HandlePetControl(wzh::PetControl control, bool respond) {
    if (control == wzh::PetControl::kNone) {
        return false;
    }
    pet_.Control(control, esp_timer_get_time() / 1000);
    intent_generation_.fetch_add(1);
    pending_expression_.store(-1);
    stop_audio_requested_.store(control != wzh::PetControl::kWake || respond);
    const bool paused = control == wzh::PetControl::kStop || control == wzh::PetControl::kRest;
    servo_output_.SetFault(wzh::kPetPaused, paused);
    if (control == wzh::PetControl::kWake) {
        servo_output_.SetFault(wzh::kMotionTimeout, false);
        if (respond) {
            TriggerAction(PandaAction::kDiscover, !conversation_active_.load(),
                          wzh::PetPriority::kSensor);
        }
    } else {
        Application::GetInstance().StopInteraction();
    }
    ESP_LOGI(TAG, "Pet control=%d", static_cast<int>(control));
    return true;
}

void PandaController::SetMaintenance(bool blocked) {
    pet_.SetMaintenance(blocked);
    if (maintenance_.exchange(blocked) != blocked) {
        intent_generation_.fetch_add(1);
        stop_audio_requested_.store(true);
        pending_expression_.store(-1);
    }
    servo_output_.SetFault(wzh::kMaintenance, blocked);
}

bool PandaController::MotionInterrupted() const {
    return servo_output_.blocked() || shutdown_requested_.load() ||
           intent_generation_.load() != running_generation_ ||
           (running_speech_bound_ && !speaking_.load() &&
            (audio_ == nullptr || !audio_->GetTtsMotionData().playing));
}

bool PandaController::QueueInteraction(wzh::PetInteraction interaction) {
    PandaBodyAction body = PandaBodyAction::kShakeHead;
    switch (interaction) {
        case wzh::PetInteraction::kTouch1:
            body = PandaBodyAction::kShakeHead;
            break;
        case wzh::PetInteraction::kTouch2:
            body = PandaBodyAction::kWaveLeft;
            break;
        case wzh::PetInteraction::kTouch3:
            body = PandaBodyAction::kWaveRight;
            break;
        case wzh::PetInteraction::kTouch4:
        case wzh::PetInteraction::kPlayful:
            body = PandaBodyAction::kRaiseArms;
            break;
        case wzh::PetInteraction::kHeld:
        case wzh::PetInteraction::kPickedUp:
            body = PandaBodyAction::kLookLeft;
            break;
        case wzh::PetInteraction::kReleased:
        case wzh::PetInteraction::kPutDown:
            body = PandaBodyAction::kLookRight;
            break;
        case wzh::PetInteraction::kShaken:
        case wzh::PetInteraction::kFlipped:
            body = PandaBodyAction::kShakeHead;
            break;
        default:
            return false;
    }
    ActionRequest request{static_cast<PandaAction>(body), false, true};
    request.interaction = interaction;
    const bool accepted = QueueRequest(request, wzh::PetPriority::kExplicit);
    if (accepted) {
        ESP_LOGD(TAG, "Interaction=%s accepted", wzh::InteractionName(interaction));
    } else {
        ESP_LOGD(TAG, "Interaction=%s rejected faults=0x%lx mode=%d",
                 wzh::InteractionName(interaction),
                 static_cast<unsigned long>(servo_output_.faults()), int(pet_mode()));
    }
    return accepted;
}

void PandaController::HandleTouch(uint8_t code) {
    const uint32_t now = esp_timer_get_time() / 1000;
    pet_.Touch(code, now);
    PollInteraction(now);
}

bool PandaController::HandleSensorEvent(int event) {
    const auto kind = static_cast<wzh::Sensors::Event>(event);
    const uint32_t now = esp_timer_get_time() / 1000;
    if (kind == wzh::Sensors::Event::kHuman) {
        if (!pet_.SensorReady(now) || pet_.Get(now).mode != wzh::PetMode::kNormal)
            return false;
        const bool accepted =
            TriggerAction(PandaAction::kDiscover, false, wzh::PetPriority::kSensor);
        if (accepted)
            pet_.SensorAccepted(now);
        return accepted;
    }
    if (!pet_.Get(now).motion)
        return false;
    // Acceleration from our own performance can resemble pickup/settling.
    // Suppress pet gestures during commanded motion; fall protection is independent.
    if (request_active_.load() || servo_output_.powered()) {
        return false;
    }
    const auto interaction = kind == wzh::Sensors::Event::kPickedUp ? wzh::PetInteraction::kPickedUp
                             : kind == wzh::Sensors::Event::kShaken ? wzh::PetInteraction::kShaken
                             : kind == wzh::Sensors::Event::kFlipped
                                 ? wzh::PetInteraction::kFlipped
                                 : wzh::PetInteraction::kPutDown;
    const bool accepted = QueueInteraction(interaction);
    if (accepted) {
        pet_.Gentle(now, kind == wzh::Sensors::Event::kPutDown ? 1800 : 3500);
    }
    return accepted;
}

void PandaController::HandleAssistantSpeech(const char* text) {
    if (text == nullptr || !conversation_active_.load()) {
        return;
    }
    speech_intent_.store(wzh::SpeechIntentFromText(text));
    const auto reaction = wzh::ReactionFromText(text);
    if (reaction == wzh::PetReaction::kNone) {
        return;
    }
    if (reaction == wzh::PetReaction::kComfort) {
        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
    }
    const PandaAction action = reaction == wzh::PetReaction::kHappy       ? PandaAction::kHappy
                               : reaction == wzh::PetReaction::kAffection ? PandaAction::kAffection
                               : reaction == wzh::PetReaction::kComfort   ? PandaAction::kListen
                               : reaction == wzh::PetReaction::kProud     ? PandaAction::kProud
                               : reaction == wzh::PetReaction::kEat       ? PandaAction::kEat
                               : reaction == wzh::PetReaction::kStartled  ? PandaAction::kStartled
                               : reaction == wzh::PetReaction::kSleepy    ? PandaAction::kSleepy
                                                                          : PandaAction::kDiscover;
    expression_since_.store(esp_timer_get_time() / 1000);
    pending_expression_.store(static_cast<int>(action));
}

bool PandaController::React(wzh::PetReaction reaction, wzh::PetPriority priority,
                            bool speech_bound) {
    if (reaction == wzh::PetReaction::kNone) {
        return false;
    }
    if (reaction == wzh::PetReaction::kComfort) {
        pet_.Gentle(esp_timer_get_time() / 1000, 5000);
    }
    const PandaAction action = reaction == wzh::PetReaction::kHappy       ? PandaAction::kHappy
                               : reaction == wzh::PetReaction::kAffection ? PandaAction::kAffection
                               : reaction == wzh::PetReaction::kComfort   ? PandaAction::kListen
                               : reaction == wzh::PetReaction::kProud     ? PandaAction::kProud
                               : reaction == wzh::PetReaction::kEat       ? PandaAction::kEat
                               : reaction == wzh::PetReaction::kStartled  ? PandaAction::kStartled
                               : reaction == wzh::PetReaction::kSleepy    ? PandaAction::kSleepy
                                                                          : PandaAction::kDiscover;
    const auto state = pet_.Get(esp_timer_get_time() / 1000);
    return TriggerAction(action, !conversation_active_.load() && state.local_sound, priority,
                         speech_bound);
}

void PandaController::PollInteraction(uint32_t now) {
    const auto touch = pet_.PollTouchEvent(now);
    if (touch.response != wzh::TouchResponse::kNone) {
        QueueInteraction(touch.interaction());
    }
    int expression = pending_expression_.load();
    if (expression >= 0 && now - expression_since_.load() >= 2500) {
        pending_expression_.compare_exchange_strong(expression, -1);
    } else if (expression >= 0 && speaking_.load() && audio_ != nullptr &&
               audio_->GetTtsMotionData().playing) {
        int expected = expression;
        if (pending_expression_.compare_exchange_strong(expected, -1)) {
            TriggerAction(static_cast<PandaAction>(expression), false, wzh::PetPriority::kEmotion,
                          true);
        }
    }
}
