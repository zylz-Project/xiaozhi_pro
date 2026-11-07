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
// 数组顺序必须与三路舵机顺序一致：头、尾左右、尾上下。
constexpr gpio_num_t kServoPins[] = {SERVO_HEAD_GPIO, SERVO_TAIL_LR_GPIO, SERVO_TAIL_UD_GPIO};
constexpr ledc_channel_t kServoChannels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2};

int irnd(int count) {
    return count > 0 ? static_cast<int>(esp_random() % static_cast<uint32_t>(count)) : 0;
}
int irand(int low, int high) { return low + irnd(high - low + 1); }
float frand(float low, float high) {
    return low + (static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX)) * (high - low);
}
float smoothstep(float value) {
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
    Pose pose{static_cast<float>(key.head), static_cast<float>(key.tail_lr),
              static_cast<float>(key.tail_ud)};
    if (mirror && clip.mirrorable) {
        pose.head = 180.0f - pose.head;
        pose.tail_lr = 180.0f - pose.tail_lr;
    }
    // The tail panda reads happier with a higher carriage. Bias expressive
    // clips upward, widen the lateral sweep, and let the head follow the tail
    // so the two joints never look disconnected. The final return frame stays
    // at the true rest pose and explicit body commands remain exact.
    const bool expressive = std::strncmp(clip.name, "body_", 5) != 0;
    if (expressive && frame + 1 < clip.count) {
        pose.tail_ud = std::max(10.0f, pose.tail_ud - 22.0f);
        pose.tail_lr = std::clamp(90.0f + (pose.tail_lr - 90.0f) * 1.30f, 6.0f, 174.0f);
    }
    if (clip.authored_dynamics) {
        pose.head = wzh::PandaHeadTarget(wzh::StoryAngle(pose.head, 90, 8, 172, 1.45f));
        pose.tail_lr = wzh::StoryAngle(pose.tail_lr, 90, 6, 174, 1.45f);
        pose.tail_ud = wzh::StoryAngle(pose.tail_ud, 150, 0, 180, 1.45f);
    } else {
        pose.head =
            wzh::PandaHeadTarget(wzh::ActionAngle(pose.head, 90.0f, 8.0f, 172.0f, 2.10f));
        pose.tail_lr = wzh::ActionAngle(pose.tail_lr, 90.0f, 6.0f, 174.0f, 2.10f);
        pose.tail_ud = wzh::ActionAngle(pose.tail_ud, 150.0f, 0.0f, 180.0f, 2.10f);
    }
    if (expressive)
        pose.head = std::clamp(pose.head + (pose.tail_lr - 90.0f) * 0.30f, 30.0f, 150.0f);
    return pose;
}

// ============================================================================
// 关键帧动作库（移植自旧工程 tailRedPanda 的 living-motion 引擎）。
// Keyframe 顺序为 duration_ms / head / tail_lr / tail_ud。
// head: 0=右 90=中 180=左；tail_lr: 0=左 90=中 180=右；
// tail_ud: 0=上 90=中 180=下，默认坐姿 150。
// ============================================================================
constexpr Keyframe kIdleGlance[] = {
    {650, 132, 64, 162}, {420, 145, 150, 92}, {820, 112, 42, 130}, {950, 90, 90, 150}};
constexpr Keyframe kIdleLowSweep[] = {
    {850, 78, 32, 170}, {1150, 108, 154, 154}, {950, 96, 55, 174}, {900, 90, 90, 152}};
constexpr Keyframe kIdlePerk[] = {{700, 118, 95, 148},
                                  {850, 150, 142, 58},
                                  {520, 136, 156, 44},
                                  {1100, 96, 104, 126},
                                  {700, 90, 90, 150}};
constexpr Keyframe kIdleCircle[] = {{650, 62, 35, 126},   {650, 75, 76, 58},   {650, 105, 150, 78},
                                    {650, 125, 158, 144}, {650, 102, 78, 170}, {850, 90, 90, 148}};
constexpr Keyframe kCuriousPeek[] = {{380, 142, 90, 158}, {520, 154, 90, 158}, {700, 148, 138, 92},
                                     {420, 62, 146, 74},  {680, 128, 45, 120}, {950, 90, 90, 148}};
constexpr Keyframe kCuriousTrack[] = {{500, 38, 104, 158},
                                      {820, 54, 150, 132},
                                      {350, 145, 150, 132},
                                      {760, 132, 45, 72},
                                      {950, 90, 90, 146}};
constexpr Keyframe kCuriousCircle[] = {{450, 150, 92, 158}, {600, 138, 34, 106},
                                       {600, 92, 70, 42},   {600, 48, 148, 68},
                                       {600, 72, 158, 146}, {900, 90, 90, 150}};
constexpr Keyframe kEatSearch[] = {{700, 42, 70, 168},   {520, 128, 38, 174}, {420, 55, 118, 172},
                                   {420, 138, 150, 165}, {600, 72, 62, 174},  {850, 90, 90, 154}};
constexpr Keyframe kEatChew[] = {{620, 128, 74, 168}, {360, 56, 76, 168},   {470, 122, 108, 172},
                                 {330, 68, 110, 172}, {620, 145, 148, 158}, {900, 90, 90, 152}};
constexpr Keyframe kEatDelight[] = {{650, 45, 42, 174},
                                    {500, 132, 148, 166},
                                    {650, 55, 154, 138},
                                    {620, 138, 36, 102},
                                    {850, 90, 90, 150}};
constexpr Keyframe kBabySeek[] = {{420, 35, 92, 160}, {360, 145, 92, 160}, {650, 128, 42, 128},
                                  {520, 52, 148, 86}, {750, 135, 122, 52}, {950, 90, 90, 146}};
constexpr Keyframe kBabyWiggle[] = {{500, 138, 48, 145}, {440, 58, 144, 116}, {500, 126, 32, 82},
                                    {460, 65, 155, 58},  {750, 120, 72, 126}, {900, 90, 90, 150}};
constexpr Keyframe kBabyComfort[] = {{800, 138, 128, 142},
                                     {900, 150, 154, 94},
                                     {1000, 126, 122, 52},
                                     {1000, 104, 62, 96},
                                     {950, 90, 90, 148}};
constexpr Keyframe kProudDisplay[] = {{650, 45, 88, 158},   {750, 142, 40, 92},
                                      {900, 155, 84, 28},   {650, 62, 156, 48},
                                      {850, 132, 138, 108}, {1000, 90, 90, 150}};
constexpr Keyframe kProudPatrol[] = {{900, 32, 148, 130},
                                     {900, 148, 34, 98},
                                     {750, 122, 70, 44},
                                     {750, 52, 152, 66},
                                     {1000, 90, 90, 148}};
constexpr Keyframe kProudCall[] = {{500, 118, 90, 154}, {780, 155, 46, 92},  {900, 148, 90, 24},
                                   {700, 48, 158, 54},  {750, 128, 34, 118}, {1050, 90, 90, 150}};
constexpr Keyframe kNuzzle[] = {{800, 142, 118, 152},
                                {900, 158, 150, 118},
                                {850, 146, 128, 70},
                                {900, 120, 48, 102},
                                {1050, 90, 90, 150}};
constexpr Keyframe kShyApproach[] = {{650, 38, 76, 164},
                                     {900, 55, 42, 140},
                                     {620, 142, 138, 96},
                                     {850, 152, 154, 58},
                                     {1100, 90, 90, 150}};
constexpr Keyframe kSoftCurl[] = {{1000, 128, 36, 154},
                                  {1100, 145, 58, 92},
                                  {1200, 132, 122, 46},
                                  {1000, 108, 156, 108},
                                  {1050, 90, 90, 150}};
constexpr Keyframe kPlayWag[] = {{480, 138, 28, 132}, {480, 55, 154, 112}, {520, 128, 36, 78},
                                 {520, 62, 158, 54},  {620, 145, 42, 98},  {850, 90, 90, 148}};
constexpr Keyframe kPlayChase[] = {{550, 42, 150, 154}, {550, 138, 42, 126},  {600, 52, 30, 76},
                                   {600, 145, 148, 42}, {650, 118, 158, 126}, {900, 90, 90, 150}};
constexpr Keyframe kPlayEight[] = {{520, 145, 35, 112},  {520, 72, 78, 48},   {520, 42, 150, 106},
                                   {520, 128, 105, 164}, {520, 152, 38, 104}, {520, 58, 104, 52},
                                   {520, 36, 154, 116},  {850, 90, 90, 150}};
constexpr Keyframe kAmbientListen[] = {{1100, 42, 90, 166},
                                       {1500, 55, 132, 138},
                                       {1200, 145, 142, 84},
                                       {1800, 132, 62, 56},
                                       {1600, 90, 90, 146}};
constexpr Keyframe kAmbientRain[] = {{1000, 132, 42, 168},  {800, 48, 148, 154},
                                     {1300, 120, 154, 106}, {1400, 62, 84, 48},
                                     {1500, 104, 32, 112},  {1500, 90, 90, 152}};
constexpr Keyframe kAmbientScent[] = {{1300, 35, 54, 170},
                                      {1500, 62, 148, 158},
                                      {1300, 142, 128, 116},
                                      {1500, 118, 38, 72},
                                      {1600, 90, 90, 150}};
constexpr Keyframe kAmbientStretch[] = {{1500, 122, 90, 174},
                                        {1800, 145, 42, 116},
                                        {1800, 132, 82, 34},
                                        {1700, 55, 154, 68},
                                        {1800, 90, 90, 150}};
constexpr Keyframe kSettle[] = {{900, 90, 58, 158}, {1100, 102, 142, 108}, {900, 90, 98, 150}};
// Playful fast wag: small, quick side-to-side beats instead of the slow broad
// sweeps used by the story clips.
constexpr Keyframe kTailWagFast[] = {
    {80, 90, 78, 118}, {80, 90, 102, 108}, {80, 90, 78, 118}, {80, 90, 102, 108},
    {80, 90, 78, 118}, {80, 90, 102, 108}, {420, 90, 90, 136}, {500, 90, 90, 150}};
// Tick-tock: holds the two sides for a beat, giving a cheeky pause.
constexpr Keyframe kTailWagTickTock[] = {
    {130, 90, 72, 122}, {180, 90, 72, 122}, {130, 90, 108, 102},
    {180, 90, 108, 102}, {130, 90, 72, 122}, {180, 90, 72, 122},
    {420, 90, 90, 138}, {500, 90, 90, 150}};
// Burst: eight short beats with a slight rise in energy toward the end.
constexpr Keyframe kTailWagBurst[] = {
    {60, 90, 76, 118}, {60, 90, 104, 112}, {60, 90, 74, 116}, {60, 90, 106, 108},
    {60, 90, 72, 114}, {60, 90, 108, 106}, {60, 90, 70, 112}, {60, 90, 110, 104},
    {360, 90, 90, 136}, {500, 90, 90, 150}};
constexpr Keyframe kStartledFrames[] = {
    {80, 88, 92, 148},     // 定格瞬间
    {130, 42, 148, 132},   // 快速缩头、尾巴甩向一侧
    {220, 150, 42, 118},   // 惊疑望向另一侧
    {300, 132, 138, 96},   // 身体压低戒备
    {320, 52, 58, 120},    // 再确认另一侧
    {380, 118, 104, 132},  // 缓一口气
    {600, 90, 90, 146},    // 收势
    {700, 90, 90, 150},
};

// New multi-beat interaction stories: glance/reach, reply, and return.
constexpr Keyframe kCuriousLookBack[] = {
    {650, 130, 58, 136},
    {750, 145, 156, 68},
    {600, 115, 46, 128},
    {800, 90, 90, 150},
};
constexpr Keyframe kCuriousTailRadar[] = {
    {650, 55, 65, 110},
    {600, 75, 50, 100},
    {750, 125, 135, 90},
    {800, 90, 90, 150},
};
constexpr Keyframe kAffectionTailWrap[] = {
    {650, 115, 62, 148},
    {700, 135, 158, 86},
    {650, 100, 48, 122},
    {800, 90, 90, 150},
};
constexpr Keyframe kAffectionPeek[] = {
    {650, 55, 152, 126},
    {600, 75, 54, 82},
    {650, 115, 146, 54},
    {800, 90, 90, 150},
};
constexpr Keyframe kPlayfulRibbon[] = {
    {650, 120, 140, 90}, {700, 110, 105, 65}, {850, 60, 40, 95},
    {650, 75, 75, 120},  {800, 90, 90, 150},
};
constexpr Keyframe kPlayfulWelcome[] = {
    {650, 125, 130, 100}, {600, 100, 90, 70}, {750, 55, 50, 100},
    {650, 80, 85, 125},   {800, 90, 90, 150},
};

#include "panda_reference_motion.h"
#include "panda_story_motion.h"

enum ClipId {
    C_IDLE_GLANCE,
    C_IDLE_LOW_SWEEP,
    C_IDLE_PERK,
    C_IDLE_CIRCLE,
    C_CURIOUS_PEEK,
    C_CURIOUS_TRACK,
    C_CURIOUS_CIRCLE,
    C_EAT_SEARCH,
    C_EAT_CHEW,
    C_EAT_DELIGHT,
    C_BABY_SEEK,
    C_BABY_WIGGLE,
    C_BABY_COMFORT,
    C_PROUD_DISPLAY,
    C_PROUD_PATROL,
    C_PROUD_CALL,
    C_NUZZLE,
    C_SHY_APPROACH,
    C_SOFT_CURL,
    C_PLAY_WAG,
    C_PLAY_CHASE,
    C_PLAY_EIGHT,
    C_AMBIENT_LISTEN,
    C_AMBIENT_RAIN,
    C_AMBIENT_SCENT,
    C_AMBIENT_STRETCH,
    C_SETTLE,
    C_STARTLED,
    C_CURIOUS_LOOK_BACK,
    C_CURIOUS_TAIL_RADAR,
    C_AFFECTION_TAIL_WRAP,
    C_AFFECTION_PEEK,
    C_PLAYFUL_RIBBON,
    C_PLAYFUL_WELCOME,
    REF_IDLE_GLANCE,
    REF_IDLE_LOW_SWEEP,
    REF_IDLE_PERK,
    REF_IDLE_CIRCLE,
    REF_IDLE_HIGH_WAG,
    REF_CURIOUS_PEEK,
    REF_CURIOUS_TRACK,
    REF_CURIOUS_CIRCLE,
    REF_CURIOUS_TAIL_LIFT,
    REF_EAT_SEARCH,
    REF_EAT_CHEW,
    REF_EAT_LOOK_UP,
    REF_BABY_SEEK,
    REF_BABY_WIGGLE,
    REF_BABY_COMFORT,
    REF_PROUD_DISPLAY,
    REF_PROUD_PATROL,
    REF_PROUD_CALL,
    REF_AFFECTION_NUZZLE,
    REF_AFFECTION_SHY,
    REF_AFFECTION_CURL,
    REF_PLAYFUL_WAG,
    REF_PLAYFUL_CHASE,
    REF_PLAYFUL_FIGURE8,
    REF_PLAYFUL_HIGH_SWEEP,
    REF_AMBIENT_LISTEN,
    REF_AMBIENT_RAIN,
    REF_AMBIENT_SCENT,
    REF_AMBIENT_STRETCH,
    REF_SETTLE,
    STORY_TAIL_QUESTION,
    STORY_TASTE_AND_SHARE,
    STORY_HIDE_AND_SEEK,
    STORY_TAIL_FAN,
    STORY_CHEEK_TAIL_HUG,
    STORY_RIBBON_EIGHT,
    STORY_BRAVE_RETURN,
    STORY_CURL_GOODNIGHT,
    STORY_TAIL_PERISCOPE,
    STORY_BAMBOO_BALANCE,
    STORY_TAIL_PEEK,
    STORY_HIGH_TAIL_SALUTE,
    STORY_TAIL_GIFT,
    STORY_TAIL_RAINBOW,
    STORY_TAIL_REASSURE,
    STORY_TAIL_NEST,
    STORY_HEAD_FIRST_GREETING,
    STORY_PEEK_THEN_FLAG,
    STORY_TAIL_COMPASS,
    STORY_RIBBON_BOW,
    STORY_CURLED_LISTENING,
    STORY_LEFT_RIGHT_INVITATION,
    STORY_FOOD_PEEK,
    STORY_QUESTION_ANSWER,
    C_TAIL_WAG_FAST,
    C_TAIL_WAG_TICK_TOCK,
    C_TAIL_WAG_BURST,
    C_COUNT,
};

constexpr Clip kClips[C_COUNT] = {
    {"idle_glance", kIdleGlance, std::size(kIdleGlance), true},
    {"idle_low_sweep", kIdleLowSweep, std::size(kIdleLowSweep), true},
    {"idle_perk", kIdlePerk, std::size(kIdlePerk), true},
    {"idle_circle", kIdleCircle, std::size(kIdleCircle), true},
    {"curious_peek", kCuriousPeek, std::size(kCuriousPeek), true},
    {"curious_track", kCuriousTrack, std::size(kCuriousTrack), true},
    {"curious_circle", kCuriousCircle, std::size(kCuriousCircle), true},
    {"eat_search", kEatSearch, std::size(kEatSearch), true},
    {"eat_chew", kEatChew, std::size(kEatChew), true},
    {"eat_look_up", kEatDelight, std::size(kEatDelight), true},
    {"baby_seek", kBabySeek, std::size(kBabySeek), true},
    {"baby_wiggle", kBabyWiggle, std::size(kBabyWiggle), true},
    {"baby_comfort", kBabyComfort, std::size(kBabyComfort), true},
    {"proud_display", kProudDisplay, std::size(kProudDisplay), true},
    {"proud_patrol", kProudPatrol, std::size(kProudPatrol), true},
    {"proud_call", kProudCall, std::size(kProudCall), true},
    {"affection_nuzzle", kNuzzle, std::size(kNuzzle), true},
    {"affection_shy", kShyApproach, std::size(kShyApproach), true},
    {"affection_curl", kSoftCurl, std::size(kSoftCurl), true},
    {"playful_wag", kPlayWag, std::size(kPlayWag), true},
    {"playful_chase", kPlayChase, std::size(kPlayChase), true},
    {"playful_figure8", kPlayEight, std::size(kPlayEight), true},
    {"ambient_listen", kAmbientListen, std::size(kAmbientListen), true},
    {"ambient_rain", kAmbientRain, std::size(kAmbientRain), true},
    {"ambient_scent", kAmbientScent, std::size(kAmbientScent), true},
    {"ambient_stretch", kAmbientStretch, std::size(kAmbientStretch), true},
    {"settle", kSettle, std::size(kSettle), false},
    {"startled_flinch", kStartledFrames, std::size(kStartledFrames), true},
    {"curious_look_back", kCuriousLookBack, std::size(kCuriousLookBack), true},
    {"curious_tail_radar", kCuriousTailRadar, std::size(kCuriousTailRadar), true},
    {"affection_tail_wrap", kAffectionTailWrap, std::size(kAffectionTailWrap), true},
    {"affection_peek", kAffectionPeek, std::size(kAffectionPeek), true},
    {"playful_ribbon", kPlayfulRibbon, std::size(kPlayfulRibbon), true},
    {"playful_welcome", kPlayfulWelcome, std::size(kPlayfulWelcome), true},
    {"ref_idle_glance", reference_motion::kIdleGlance, std::size(reference_motion::kIdleGlance),
     true, true},
    {"ref_idle_low_sweep", reference_motion::kIdleLowSweep,
     std::size(reference_motion::kIdleLowSweep), true, true},
    {"ref_idle_perk", reference_motion::kIdlePerk, std::size(reference_motion::kIdlePerk), true,
     true},
    {"ref_idle_circle", reference_motion::kIdleCircle, std::size(reference_motion::kIdleCircle),
     true, true},
    {"ref_idle_high_wag", reference_motion::kIdleHighWag, std::size(reference_motion::kIdleHighWag),
     true, true},
    {"ref_curious_peek", reference_motion::kCuriousPeek, std::size(reference_motion::kCuriousPeek),
     true, true},
    {"ref_curious_track", reference_motion::kCuriousTrack,
     std::size(reference_motion::kCuriousTrack), true, true},
    {"ref_curious_circle", reference_motion::kCuriousCircle,
     std::size(reference_motion::kCuriousCircle), true, true},
    {"ref_curious_tail_lift", reference_motion::kCuriousTailLift,
     std::size(reference_motion::kCuriousTailLift), true, true},
    {"ref_eat_search", reference_motion::kEatSearch, std::size(reference_motion::kEatSearch), true,
     true},
    {"ref_eat_chew", reference_motion::kEatChew, std::size(reference_motion::kEatChew), true, true},
    {"ref_eat_look_up", reference_motion::kEatDelight, std::size(reference_motion::kEatDelight),
     true, true},
    {"ref_baby_seek", reference_motion::kBabySeek, std::size(reference_motion::kBabySeek), true,
     true},
    {"ref_baby_wiggle", reference_motion::kBabyWiggle, std::size(reference_motion::kBabyWiggle),
     true, true},
    {"ref_baby_comfort", reference_motion::kBabyComfort, std::size(reference_motion::kBabyComfort),
     true, true},
    {"ref_proud_display", reference_motion::kProudDisplay,
     std::size(reference_motion::kProudDisplay), true, true},
    {"ref_proud_patrol", reference_motion::kProudPatrol, std::size(reference_motion::kProudPatrol),
     true, true},
    {"ref_proud_call", reference_motion::kProudCall, std::size(reference_motion::kProudCall), true,
     true},
    {"ref_affection_nuzzle", reference_motion::kNuzzle, std::size(reference_motion::kNuzzle), true,
     true},
    {"ref_affection_shy", reference_motion::kShyApproach, std::size(reference_motion::kShyApproach),
     true, true},
    {"ref_affection_curl", reference_motion::kSoftCurl, std::size(reference_motion::kSoftCurl),
     true, true},
    {"ref_playful_wag", reference_motion::kPlayWag, std::size(reference_motion::kPlayWag), true,
     true},
    {"ref_playful_chase", reference_motion::kPlayChase, std::size(reference_motion::kPlayChase),
     true, true},
    {"ref_playful_figure8", reference_motion::kPlayEight, std::size(reference_motion::kPlayEight),
     true, true},
    {"ref_playful_high_sweep", reference_motion::kPlayHighSweep,
     std::size(reference_motion::kPlayHighSweep), true, true},
    {"ref_ambient_listen", reference_motion::kAmbientListen,
     std::size(reference_motion::kAmbientListen), true, true},
    {"ref_ambient_rain", reference_motion::kAmbientRain, std::size(reference_motion::kAmbientRain),
     true, true},
    {"ref_ambient_scent", reference_motion::kAmbientScent,
     std::size(reference_motion::kAmbientScent), true, true},
    {"ref_ambient_stretch", reference_motion::kAmbientStretch,
     std::size(reference_motion::kAmbientStretch), true, true},
    {"ref_settle", reference_motion::kSettle, std::size(reference_motion::kSettle), false, true},
    {"story_tail_question", story_motion::kTailQuestion, std::size(story_motion::kTailQuestion),
     true, true},
    {"story_taste_and_share", story_motion::kTasteAndShare, std::size(story_motion::kTasteAndShare),
     true, true},
    {"story_hide_and_seek", story_motion::kHideAndSeek, std::size(story_motion::kHideAndSeek), true,
     true},
    {"story_tail_fan", story_motion::kTailFan, std::size(story_motion::kTailFan), true, true},
    {"story_cheek_tail_hug", story_motion::kCheekTailHug, std::size(story_motion::kCheekTailHug),
     true, true},
    {"story_ribbon_eight", story_motion::kRibbonEight, std::size(story_motion::kRibbonEight), true,
     true},
    {"story_brave_return", story_motion::kBraveReturn, std::size(story_motion::kBraveReturn), true,
     true},
    {"story_curl_goodnight", story_motion::kCurlGoodnight, std::size(story_motion::kCurlGoodnight),
     true, true},
    {"story_tailperiscope", story_motion::kTailPeriscope, std::size(story_motion::kTailPeriscope),
     true, true},
    {"story_bamboobalance", story_motion::kBambooBalance, std::size(story_motion::kBambooBalance),
     true, true},
    {"story_tailpeek", story_motion::kTailPeek, std::size(story_motion::kTailPeek), true, true},
    {"story_hightailsalute", story_motion::kHighTailSalute,
     std::size(story_motion::kHighTailSalute), true, true},
    {"story_tailgift", story_motion::kTailGift, std::size(story_motion::kTailGift), true, true},
    {"story_tailrainbow", story_motion::kTailRainbow, std::size(story_motion::kTailRainbow), true,
     true},
    {"story_tailreassure", story_motion::kTailReassure, std::size(story_motion::kTailReassure),
     true, true},
    {"story_tailnest", story_motion::kTailNest, std::size(story_motion::kTailNest), true, true},
    {"story_head_first_greeting", story_motion::kHeadFirstGreeting,
     std::size(story_motion::kHeadFirstGreeting), true, true},
    {"story_peek_then_flag", story_motion::kPeekThenFlag, std::size(story_motion::kPeekThenFlag),
     true, true},
    {"story_tail_compass", story_motion::kTailCompass, std::size(story_motion::kTailCompass), true,
     true},
    {"story_ribbon_bow", story_motion::kRibbonBow, std::size(story_motion::kRibbonBow), true, true},
    {"story_curled_listening", story_motion::kCurledListening,
     std::size(story_motion::kCurledListening), true, true},
    {"story_left_right_invitation", story_motion::kLeftRightInvitation,
     std::size(story_motion::kLeftRightInvitation), true, true},
    {"story_food_peek", story_motion::kFoodPeek, std::size(story_motion::kFoodPeek), true, true},
    {"story_question_answer", story_motion::kQuestionAnswer,
     std::size(story_motion::kQuestionAnswer), true, true},
    {"tail_wag_fast", kTailWagFast, std::size(kTailWagFast), true},
    {"tail_wag_tick_tock", kTailWagTickTock, std::size(kTailWagTickTock), true},
    {"tail_wag_burst", kTailWagBurst, std::size(kTailWagBurst), true},
};

constexpr int8_t kFamily[kBehaviourCount][wzh_panda::kFamilyWidth] = {
    {C_IDLE_GLANCE, C_IDLE_LOW_SWEEP, C_IDLE_PERK, C_IDLE_CIRCLE, REF_IDLE_GLANCE,
     REF_IDLE_LOW_SWEEP, REF_IDLE_PERK, REF_IDLE_CIRCLE, REF_IDLE_HIGH_WAG,
     STORY_HEAD_FIRST_GREETING, STORY_CURLED_LISTENING, C_TAIL_WAG_FAST, C_TAIL_WAG_TICK_TOCK,
     C_TAIL_WAG_BURST, -1, -1},
    {C_CURIOUS_PEEK, C_CURIOUS_TRACK, C_CURIOUS_CIRCLE, C_CURIOUS_LOOK_BACK, C_CURIOUS_TAIL_RADAR,
     REF_CURIOUS_PEEK, REF_CURIOUS_TRACK, REF_CURIOUS_CIRCLE, REF_CURIOUS_TAIL_LIFT,
     STORY_TAIL_QUESTION, STORY_TAIL_PERISCOPE, STORY_PEEK_THEN_FLAG, STORY_TAIL_COMPASS,
     STORY_CURLED_LISTENING, STORY_QUESTION_ANSWER, -1},
    {C_EAT_SEARCH, C_EAT_CHEW, C_EAT_DELIGHT, REF_EAT_SEARCH, REF_EAT_CHEW, REF_EAT_LOOK_UP,
     STORY_TASTE_AND_SHARE, STORY_BAMBOO_BALANCE, STORY_FOOD_PEEK, -1, -1, -1, -1, -1, -1, -1},
    {C_BABY_SEEK, C_BABY_WIGGLE, C_BABY_COMFORT, REF_BABY_SEEK, REF_BABY_WIGGLE, REF_BABY_COMFORT,
     STORY_HIDE_AND_SEEK, STORY_TAIL_PEEK, STORY_PEEK_THEN_FLAG, STORY_HEAD_FIRST_GREETING,
     STORY_QUESTION_ANSWER, -1, -1, -1, -1, -1},
    {C_PROUD_DISPLAY, C_PROUD_PATROL, C_PROUD_CALL, REF_PROUD_DISPLAY, REF_PROUD_PATROL,
     REF_PROUD_CALL, STORY_TAIL_FAN, STORY_HIGH_TAIL_SALUTE, STORY_TAIL_COMPASS,
     STORY_LEFT_RIGHT_INVITATION, -1, -1, -1, -1, -1, -1},
    {C_NUZZLE, C_SHY_APPROACH, C_SOFT_CURL, C_AFFECTION_TAIL_WRAP, C_AFFECTION_PEEK,
     REF_AFFECTION_NUZZLE, REF_AFFECTION_SHY, REF_AFFECTION_CURL, STORY_CHEEK_TAIL_HUG,
     STORY_TAIL_GIFT, STORY_CURLED_LISTENING, STORY_HEAD_FIRST_GREETING, STORY_PEEK_THEN_FLAG, -1,
     -1, -1},
    {C_PLAY_WAG, C_PLAY_CHASE, C_PLAY_EIGHT, C_PLAYFUL_RIBBON, C_PLAYFUL_WELCOME, REF_PLAYFUL_WAG,
     REF_PLAYFUL_CHASE, REF_PLAYFUL_FIGURE8, REF_PLAYFUL_HIGH_SWEEP, STORY_RIBBON_EIGHT,
     STORY_TAIL_RAINBOW, STORY_RIBBON_BOW, STORY_LEFT_RIGHT_INVITATION, STORY_HEAD_FIRST_GREETING,
     STORY_QUESTION_ANSWER, -1},
    {C_AMBIENT_LISTEN, C_AMBIENT_RAIN, C_AMBIENT_SCENT, C_AMBIENT_STRETCH, REF_AMBIENT_LISTEN,
     REF_AMBIENT_RAIN, REF_AMBIENT_SCENT, REF_AMBIENT_STRETCH, -1, -1, -1, -1, -1, -1, -1, -1},
    {C_SETTLE, REF_SETTLE, STORY_CURL_GOODNIGHT, STORY_TAIL_NEST, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1},
    {C_STARTLED, STORY_BRAVE_RETURN, STORY_TAIL_REASSURE, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1},
};
static_assert(C_COUNT < 128, "Clip IDs must fit the family shuffle bag");

// ============================================================================
// 身体部位级动作剪辑：从当前姿态起手，结束时回到默认坐姿 90/90/150。
// 方向性动作（看左/看右）不可镜像，避免反向。
// ============================================================================
constexpr Keyframe kBodyShakeHead[] = {
    {900, 144, 90, 150},
    {1600, 36, 90, 150},
    {500, 36, 90, 150},
    {1200, 90, 90, 150},
};
constexpr Keyframe kBodyLookLeft[] = {
    {900, 144, 90, 150},
    {700, 144, 90, 150},
    {1200, 90, 90, 150},
};
constexpr Keyframe kBodyLookRight[] = {
    {900, 36, 90, 150},
    {700, 36, 90, 150},
    {1200, 90, 90, 150},
};
constexpr Keyframe kBodyWagTail[] = {
    {600, 90, 28, 118}, {650, 90, 164, 68},  {550, 90, 110, 20}, {350, 90, 110, 20},
    {750, 90, 18, 42},  {650, 90, 142, 104}, {800, 90, 90, 150},
};
constexpr Keyframe kBodyTwistNeck[] = {
    {850, 134, 90, 150},
    {1400, 46, 90, 150},
    {1100, 126, 90, 150},
    {1200, 90, 90, 150},
};

// Explicit joint commands: finite, directional, with a quiet centered finish.
constexpr Keyframe kJointHeadCenterFrames[] = {
    {1200, 90, 90, 150},
};
constexpr Keyframe kJointHeadSwayLeftFrames[] = {
    {650, 132, 90, 150}, {520, 108, 90, 150}, {650, 146, 90, 150},
    {500, 124, 90, 150}, {1000, 90, 90, 150},
};
constexpr Keyframe kJointHeadSwayRightFrames[] = {
    {650, 48, 90, 150}, {520, 72, 90, 150},  {650, 34, 90, 150},
    {500, 56, 90, 150}, {1000, 90, 90, 150},
};
constexpr Keyframe kJointHeadScanFrames[] = {
    {900, 144, 90, 150}, {550, 144, 90, 150}, {1500, 36, 90, 150},
    {550, 36, 90, 150},  {1200, 90, 90, 150},
};
constexpr Keyframe kJointCenterFrames[] = {
    {1200, 90, 90, 150},
};
constexpr Keyframe kJointTailUpFrames[] = {
    {1100, 90, 90, 38},
    {700, 90, 90, 38},
    {1400, 90, 90, 150},
};
constexpr Keyframe kJointTailDownFrames[] = {
    {1000, 90, 90, 170},
    {650, 90, 90, 170},
    {1200, 90, 90, 150},
};
constexpr Keyframe kJointTailLeftFrames[] = {
    {900, 90, 22, 150},
    {700, 90, 22, 150},
    {1100, 90, 90, 150},
};
constexpr Keyframe kJointTailRightFrames[] = {
    {900, 90, 158, 150},
    {700, 90, 158, 150},
    {1100, 90, 90, 150},
};
constexpr Keyframe kJointTailCenterFrames[] = {
    {1200, 90, 90, 150},
};
constexpr Keyframe kJointTailLevelFrames[] = {
    {1100, 90, 90, 90},
    {650, 90, 90, 90},
    {1300, 90, 90, 150},
};
constexpr Keyframe kJointTailSwayLeftFrames[] = {
    {800, 90, 26, 150},
    {700, 90, 72, 150},
    {850, 90, 18, 150},
    {1000, 90, 90, 150},
};
constexpr Keyframe kJointTailSwayRightFrames[] = {
    {800, 90, 154, 150},
    {700, 90, 108, 150},
    {850, 90, 162, 150},
    {1000, 90, 90, 150},
};
constexpr Keyframe kJointTailCircleFrames[] = {
    {900, 90, 130, 70}, {950, 90, 154, 105}, {950, 90, 90, 142},  {950, 90, 26, 105},
    {950, 90, 50, 40},  {950, 90, 128, 22},  {1300, 90, 90, 150},
};
constexpr Keyframe kJointTailEightFrames[] = {
    {850, 90, 140, 48},  {850, 90, 90, 100}, {850, 90, 35, 42},  {850, 90, 90, 80},
    {850, 90, 146, 128}, {850, 90, 90, 100}, {850, 90, 34, 126}, {1400, 90, 90, 150},
};
constexpr Keyframe kJointTailCurlFrames[] = {
    {1100, 90, 90, 44},
    {800, 90, 90, 18},
    {550, 90, 90, 18},
    {1400, 90, 90, 150},
};
constexpr Keyframe kJointWagTailSlowFrames[] = {
    {1500, 90, 26, 100}, {2000, 90, 154, 74}, {1600, 90, 48, 112},
    {1600, 90, 136, 68}, {1500, 90, 90, 150},
};
constexpr Keyframe kJointTailFanFrames[] = {
    {1100, 90, 90, 32}, {1000, 90, 30, 24},  {1400, 90, 150, 44},
    {1200, 90, 40, 20}, {1500, 90, 90, 150},
};
constexpr Keyframe kJointTailBounceFrames[] = {
    {900, 90, 90, 42},
    {1000, 90, 90, 130},
    {1000, 90, 90, 28},
    {1200, 90, 90, 150},
};

enum BodyClipId {
    BODY_SHAKE_HEAD,
    BODY_LOOK_LEFT,
    BODY_LOOK_RIGHT,
    BODY_WAG_TAIL,
    BODY_TWIST_NECK,
    BODY_HEAD_CENTER,
    BODY_HEAD_SWAY_LEFT,
    BODY_HEAD_SWAY_RIGHT,
    BODY_HEAD_SCAN,
    BODY_CENTER,
    BODY_TAIL_UP,
    BODY_TAIL_DOWN,
    BODY_TAIL_LEFT,
    BODY_TAIL_RIGHT,
    BODY_TAIL_CENTER,
    BODY_TAIL_LEVEL,
    BODY_TAIL_SWAY_LEFT,
    BODY_TAIL_SWAY_RIGHT,
    BODY_TAIL_CIRCLE,
    BODY_TAIL_EIGHT,
    BODY_TAIL_CURL,
    BODY_WAG_TAIL_SLOW,
    BODY_TAIL_FAN,
    BODY_TAIL_BOUNCE,
    BODY_COUNT,
};
constexpr Clip kBodyClips[BODY_COUNT] = {
    {"body_shake_head", kBodyShakeHead, std::size(kBodyShakeHead), true},
    {"body_look_left", kBodyLookLeft, std::size(kBodyLookLeft), false},
    {"body_look_right", kBodyLookRight, std::size(kBodyLookRight), false},
    {"body_wag_tail", kBodyWagTail, std::size(kBodyWagTail), true},
    {"body_twist_neck", kBodyTwistNeck, std::size(kBodyTwistNeck), true},
    {"body_head_center", kJointHeadCenterFrames, std::size(kJointHeadCenterFrames), false, true},
    {"body_head_sway_left", kJointHeadSwayLeftFrames, std::size(kJointHeadSwayLeftFrames), false,
     true},
    {"body_head_sway_right", kJointHeadSwayRightFrames, std::size(kJointHeadSwayRightFrames), false,
     true},
    {"body_head_scan", kJointHeadScanFrames, std::size(kJointHeadScanFrames), false, true},
    {"body_center", kJointCenterFrames, std::size(kJointCenterFrames), false, true},
    {"body_tail_up", kJointTailUpFrames, std::size(kJointTailUpFrames), false, true},
    {"body_tail_down", kJointTailDownFrames, std::size(kJointTailDownFrames), false, true},
    {"body_tail_left", kJointTailLeftFrames, std::size(kJointTailLeftFrames), false, true},
    {"body_tail_right", kJointTailRightFrames, std::size(kJointTailRightFrames), false, true},
    {"body_tail_center", kJointTailCenterFrames, std::size(kJointTailCenterFrames), false, true},
    {"body_tail_level", kJointTailLevelFrames, std::size(kJointTailLevelFrames), false, true},
    {"body_tail_sway_left", kJointTailSwayLeftFrames, std::size(kJointTailSwayLeftFrames), false,
     true},
    {"body_tail_sway_right", kJointTailSwayRightFrames, std::size(kJointTailSwayRightFrames), false,
     true},
    {"body_tail_circle", kJointTailCircleFrames, std::size(kJointTailCircleFrames), false, true},
    {"body_tail_eight", kJointTailEightFrames, std::size(kJointTailEightFrames), false, true},
    {"body_tail_curl", kJointTailCurlFrames, std::size(kJointTailCurlFrames), false, true},
    {"body_wag_tail_slow", kJointWagTailSlowFrames, std::size(kJointWagTailSlowFrames), false,
     true},
    {"body_tail_fan", kJointTailFanFrames, std::size(kJointTailFanFrames), false, true},
    {"body_tail_bounce", kJointTailBounceFrames, std::size(kJointTailBounceFrames), false, true},
};

// Dedicated short acknowledgements: fixed pad identity, no random mirroring.
constexpr Keyframe kAck_touch1[] = {
    {700, 158, 146, 96}, {420, 100, 64, 58}, {420, 42, 118, 128}, {900, 90, 90, 150}};
constexpr Keyframe kAck_touch2[] = {
    {700, 150, 162, 104}, {420, 74, 58, 56}, {420, 126, 126, 132}, {900, 90, 90, 150}};
constexpr Keyframe kAck_touch3[] = {
    {700, 30, 18, 110}, {420, 84, 126, 64}, {420, 144, 70, 136}, {900, 90, 90, 150}};
constexpr Keyframe kAck_touch4[] = {
    {700, 145, 150, 42}, {420, 62, 56, 98}, {420, 120, 120, 30}, {900, 90, 90, 150}};
constexpr Keyframe kAck_playful[] = {{650, 140, 156, 42},
                                     {420, 140, 62, 100},
                                     {650, 40, 44, 34},
                                     {420, 40, 148, 96},
                                     {1000, 90, 90, 150}};
constexpr Keyframe kAck_held[] = {
    {700, 160, 66, 118}, {420, 110, 148, 70}, {420, 72, 100, 42}, {900, 90, 90, 150}};
constexpr Keyframe kAck_released[] = {
    {700, 140, 58, 120}, {420, 90, 152, 68}, {420, 40, 104, 136}, {900, 90, 90, 150}};
constexpr Keyframe kAck_pickup[] = {
    {700, 160, 142, 86}, {420, 120, 58, 52}, {420, 64, 114, 128}, {900, 90, 90, 150}};
constexpr Keyframe kAck_shake[] = {
    {700, 20, 56, 126}, {420, 80, 150, 68}, {420, 148, 98, 44}, {900, 90, 90, 150}};
constexpr Keyframe kAck_flip[] = {
    {700, 155, 60, 118}, {420, 100, 150, 64}, {420, 44, 106, 136}, {900, 90, 90, 150}};
constexpr Keyframe kAck_settled[] = {
    {700, 138, 148, 78}, {420, 78, 66, 120}, {420, 116, 112, 58}, {900, 90, 90, 150}};
// Alternate responses retain each pad's identity, with a different head/tail pose.
constexpr Keyframe kAck_touch1Alt[] = {
    {600, 22, 52, 120}, {500, 90, 138, 68}, {500, 138, 84, 138}, {900, 90, 90, 150}};
constexpr Keyframe kAck_touch2Alt[] = {
    {650, 135, 54, 64}, {500, 84, 148, 118}, {500, 42, 96, 42}, {900, 90, 90, 150}};
constexpr Keyframe kAck_touch3Alt[] = {
    {650, 45, 132, 94}, {500, 96, 58, 58}, {500, 138, 104, 134}, {900, 90, 90, 150}};
constexpr Keyframe kAck_touch4Alt[] = {
    {650, 35, 60, 122}, {500, 92, 142, 56}, {500, 144, 88, 140}, {900, 90, 90, 150}};
// Additional variants keep event identity while changing the spatial story.
constexpr Keyframe kRich_playful_alt[] = {
    {650, 125, 52, 116}, {650, 105, 144, 62}, {850, 55, 68, 134}, {800, 90, 90, 150}};
constexpr Keyframe kRich_held_alt[] = {
    {650, 60, 62, 124}, {650, 80, 140, 76}, {700, 100, 92, 138}, {800, 90, 90, 150}};
constexpr Keyframe kRich_released_alt[] = {
    {600, 60, 142, 118}, {600, 110, 64, 72}, {800, 90, 90, 150}};
constexpr Keyframe kRich_pickup_alt[] = {
    {620, 55, 64, 122}, {650, 75, 146, 64}, {800, 90, 90, 150}};
constexpr Keyframe kRich_shake_alt[] = {
    {550, 130, 126, 138}, {650, 105, 52, 76}, {800, 90, 90, 150}};
constexpr Keyframe kRich_flip_alt[] = {{600, 55, 142, 136}, {650, 75, 58, 78}, {800, 90, 90, 150}};
constexpr Keyframe kRich_settled_alt[] = {
    {600, 65, 60, 122}, {600, 80, 146, 82}, {800, 90, 90, 150}};
constexpr Keyframe kRich_touch1_story[] = {
    {550, 120, 110, 136}, {650, 140, 150, 100}, {650, 110, 72, 74}, {800, 90, 90, 150}};
constexpr Keyframe kRich_touch2_story[] = {
    {600, 120, 138, 110}, {550, 110, 62, 72}, {700, 140, 154, 126}, {800, 90, 90, 150}};
constexpr Keyframe kRich_touch3_story[] = {
    {600, 60, 46, 116}, {550, 70, 136, 70}, {700, 45, 28, 128}, {800, 90, 90, 150}};
constexpr Keyframe kRich_touch4_story[] = {
    {600, 115, 132, 100}, {650, 95, 52, 60}, {800, 65, 146, 124}, {800, 90, 90, 150}};
constexpr Keyframe kRich_playful_story[] = {{600, 120, 136, 108},
                                            {650, 100, 58, 74},
                                            {850, 60, 148, 58},
                                            {650, 75, 100, 128},
                                            {800, 90, 90, 150}};
constexpr Keyframe kRich_held_story[] = {
    {650, 115, 124, 116}, {650, 100, 58, 72}, {700, 80, 142, 120}, {800, 90, 90, 150}};
constexpr Keyframe kRich_released_story[] = {
    {600, 120, 48, 124}, {600, 100, 148, 70}, {800, 90, 90, 150}};
constexpr Keyframe kRich_pickup_story[] = {
    {600, 120, 124, 116}, {650, 100, 52, 66}, {650, 80, 150, 120}, {800, 90, 90, 150}};
constexpr Keyframe kRich_shake_story[] = {
    {550, 65, 46, 142}, {650, 115, 144, 78}, {800, 90, 90, 150}};
constexpr Keyframe kRich_flip_story[] = {
    {600, 120, 54, 132}, {650, 100, 146, 68}, {800, 90, 90, 150}};
constexpr Keyframe kRich_settled_story[] = {
    {600, 115, 138, 112}, {600, 100, 54, 70}, {800, 90, 90, 150}};

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
    {"ack-touch1-expressive", story_motion::kCheekTailHug, std::size(story_motion::kCheekTailHug),
     false, true},
    {"ack-touch2-expressive", story_motion::kTailQuestion, std::size(story_motion::kTailQuestion),
     false, true},
    {"ack-touch3-expressive", story_motion::kTasteAndShare, std::size(story_motion::kTasteAndShare),
     false, true},
    {"ack-touch4-expressive", story_motion::kTailFan, std::size(story_motion::kTailFan), false,
     true},
    {"ack-playful-expressive", story_motion::kRibbonEight, std::size(story_motion::kRibbonEight),
     false, true},
    {"ack-held-expressive", story_motion::kCurlGoodnight, std::size(story_motion::kCurlGoodnight),
     false, true},
    {"ack-released-expressive", story_motion::kHideAndSeek, std::size(story_motion::kHideAndSeek),
     false, true},
    {"ack-pickup-expressive", story_motion::kCheekTailHug, std::size(story_motion::kCheekTailHug),
     false, true},
    {"ack-shake-expressive", story_motion::kBraveReturn, std::size(story_motion::kBraveReturn),
     false, true},
    {"ack-flip-expressive", story_motion::kTailQuestion, std::size(story_motion::kTailQuestion),
     false, true},
    {"ack-settled-expressive", story_motion::kCurlGoodnight,
     std::size(story_motion::kCurlGoodnight), false, true},
    {"ack_touch1_lively", story_motion::kTailGift, std::size(story_motion::kTailGift), false, true},
    {"ack_touch2_lively", story_motion::kTailPeriscope, std::size(story_motion::kTailPeriscope),
     false, true},
    {"ack_touch3_lively", story_motion::kBambooBalance, std::size(story_motion::kBambooBalance),
     false, true},
    {"ack_touch4_lively", story_motion::kHighTailSalute, std::size(story_motion::kHighTailSalute),
     false, true},
    {"ack_playful_lively", story_motion::kTailRainbow, std::size(story_motion::kTailRainbow), false,
     true},
    {"ack_held_lively", story_motion::kTailNest, std::size(story_motion::kTailNest), false, true},
    {"ack_released_lively", story_motion::kTailPeek, std::size(story_motion::kTailPeek), false,
     true},
    {"ack_pickup_lively", story_motion::kTailGift, std::size(story_motion::kTailGift), false, true},
    {"ack_shake_lively", story_motion::kTailReassure, std::size(story_motion::kTailReassure), false,
     true},
    {"ack_flip_lively", story_motion::kTailPeriscope, std::size(story_motion::kTailPeriscope),
     false, true},
    {"ack_settled_lively", story_motion::kTailNest, std::size(story_motion::kTailNest), false,
     true},
};
static_assert(std::size(kInteractionClips) == wzh::InteractionMotionSelector::kClipCount);

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
    // 舵机电源与 PWM 分开控制：先给电源，再把各通道设置到安全默认角度。
    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << SERVO_POWER_GPIO;
    power_config.mode = GPIO_MODE_OUTPUT;
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
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }
    servo_output_.Initialize(SERVO_POWER_GPIO, kServoChannels, std::size(kServoChannels));
    // Library poses still use the 150-degree sitting center. Re-indexed tails
    // map that whole library to a 90-degree center at the servo boundary.
    wzh::g_tail_ud_remap = SERVO_TAIL_UD_DEFAULT != 150;
    wzh::g_tail_servo_ud = SERVO_TAIL_UD_DEFAULT;
    SetServoAngles(SERVO_HEAD_DEFAULT, SERVO_TAIL_LR_DEFAULT, SERVO_TAIL_UD_DEFAULT);
}

void PandaController::SetServoAngles(int head, int tail_lr, int tail_ud) {
    const int angles[] = {head, tail_lr, tail_ud};
    servo_output_.Write(angles, std::size(angles));
}

void PandaController::OutputPose(const wzh_panda::Pose& requested) {
    const uint32_t now = esp_timer_get_time() / 1000;
    const auto state = pet_.Get(now);
    const float requested_gain = state.gentle || state.mode == wzh::PetMode::kQuiet ? .60f : 1.0f;
    const float gain = quiet_center_.active() ? 1.f
                       : clip_active_         ? std::min(clip_gain_, requested_gain)
                                              : requested_gain;
    Pose target = requested;
    target.head =
        wzh::ExpressiveAngle(target.head, 90.0f, wzh::kPandaHeadMin, wzh::kPandaHeadMax, gain);
    target.tail_lr = wzh::ExpressiveAngle(target.tail_lr, 90.0f, 6.0f, 174.0f, gain);
    target.tail_ud = wzh::ExpressiveAngle(target.tail_ud, 150.0f, 0.0f, 180.0f, gain);
    const float servo_ud = wzh::TailServoUd(target.tail_ud);
    const float last_servo_ud =
        wzh::g_tail_ud_remap ? wzh::g_tail_servo_ud : last_output_pose_.tail_ud;
    const uint32_t elapsed =
        last_output_ms_ == 0 ? kTickMs : std::min<uint32_t>(now - last_output_ms_, 50);
    const float allowed = wzh::kPetServoSpeed * elapsed / 1000.0f;
    const float travel = std::max({std::abs(target.head - last_output_pose_.head),
                                   std::abs(target.tail_lr - last_output_pose_.tail_lr),
                                   std::abs(servo_ud - last_servo_ud)});
    const float mix = travel > allowed && travel > 0 ? allowed / travel : 1.0f;
    target.head = last_output_pose_.head + (target.head - last_output_pose_.head) * mix;
    target.tail_lr = last_output_pose_.tail_lr + (target.tail_lr - last_output_pose_.tail_lr) * mix;
    const float write_ud = last_servo_ud + (servo_ud - last_servo_ud) * mix;
    const float angles[] = {target.head, target.tail_lr, write_ud};
    if (servo_output_.WriteDegrees(angles, std::size(angles))) {
        target.tail_ud = last_output_pose_.tail_ud + (target.tail_ud - last_output_pose_.tail_ud) * mix;
        last_output_pose_ = target;
        if (wzh::g_tail_ud_remap)
            wzh::g_tail_servo_ud = write_ud;
        last_output_ms_ = now;
        if (now - last_pose_log_ms_ >= 500) {
            ESP_LOGD(TAG, "Servo command %s=%.1f/%.1f/%.1f gentle=%d faults=0x%lx",
                     "head/tailLR/tailUD", angles[0], angles[1], angles[2], state.gentle,
                     static_cast<unsigned long>(servo_output_.faults()));
            last_pose_log_ms_ = now;
        }
    }
}

uint32_t PandaController::FrameDuration(uint8_t frame) const {
    const Pose from = frame == 0 ? clip_origin_ : TargetPose(*clip_, frame - 1, mirror_);
    const Pose to = TargetPose(*clip_, frame, mirror_);
    const float travel =
        std::max({std::abs(to.head - from.head), std::abs(to.tail_lr - from.tail_lr),
                  std::abs(to.tail_ud - from.tail_ud)});
    const uint32_t requested = static_cast<uint32_t>(clip_->frames[frame].duration_ms * tempo_);
    if (travel > .1f && std::abs(to.head - 90) <= 2.f && std::abs(to.tail_lr - 90) <= 2.f &&
        std::abs(to.tail_ud - 150) <= 2.f)
        return std::max(requested, wzh::SettlingDuration(travel, 75.f));
    return clip_->authored_dynamics
               ? wzh::StoryFrameDuration(travel, requested, motion_seed_, frame)
               : wzh::PetFrameDuration(travel, requested);
}

int PandaController::ChooseClip(Behaviour behaviour) {
    const size_t behaviour_index = static_cast<size_t>(behaviour);
    ShuffleBag& bag = bags_[behaviour_index];
    if (bag.position >= bag.count) {
        bag.count = 0;
        for (int i = 0; i < wzh_panda::kFamilyWidth && kFamily[behaviour_index][i] >= 0; ++i) {
            bag.items[bag.count++] = kFamily[behaviour_index][i];
        }
        for (int i = bag.count - 1; i > 0; --i) {
            std::swap(bag.items[i], bag.items[irnd(i + 1)]);
        }
        if (bag.count > 1 && bag.items[0] == bag.previous) {
            std::swap(bag.items[0], bag.items[1]);
        }
        bag.position = 0;
    }
    const int result = bag.items[bag.position++];
    bag.previous = static_cast<int8_t>(result);
    last_clip_ = result;
    return result;
}

void PandaController::StartClip(int clip_id, uint32_t now_ms) {
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
    const auto state = pet_.Get(now_ms);
    clip_gain_ = state.gentle || state.mode == wzh::PetMode::kQuiet ? .60f : 1.0f;
    clip_ = clip;
    frame_ = 0;
    frame_started_ms_ = now_ms;
    // 所有动作都带随机节奏；硬摆动只在真机校准阶段由板级配置决定，这里
    // 统一使用柔和节奏，保证“灵动”而非生硬。
    tempo_ = frand(0.92f, 1.08f);
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
    pose_.head = wzh::MotionHermite(previous.head, from_.head, target.head, next.head, before,
                                    frame_duration_ms_, after, t);
    pose_.tail_lr = wzh::MotionHermite(previous.tail_lr, from_.tail_lr, target.tail_lr,
                                       next.tail_lr, before, frame_duration_ms_, after, t);
    pose_.tail_ud = wzh::MotionHermite(previous.tail_ud, from_.tail_ud, target.tail_ud,
                                       next.tail_ud, before, frame_duration_ms_, after, t);
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
        case wzh::SoundKind::kSneeze:
            return Behaviour::kStartled;
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
    if (clip_active_ && clip_ && std::strncmp(clip_->name, "body_", 5) == 0)
        return;
    // 呼吸带动头部微幅起伏、尾巴轻轻浮沉。
    breath_phase_ += 20.0f;
    if (breath_phase_ > breath_period_) {
        breath_phase_ = 0.0f;
        breath_period_ = static_cast<uint32_t>(irand(2800, 4300));
    }
    const float b = std::sin(6.2831853f * breath_phase_ / static_cast<float>(breath_period_));
    pose.tail_ud -= b * 1.8f * life;

    // 头部漫无目的地漂移凝视。
    if (now_ms >= gaze_next_ms_) {
        gaze_target_ = frand(-13.0f, 13.0f);
        gaze_next_ms_ = now_ms + static_cast<uint32_t>(irand(1200, 3800));
    }
    gaze_pos_ += (gaze_target_ - gaze_pos_) * 0.03f;
    pose.head += gaze_pos_ * life;

    // One slow, phase-coherent ellipse keeps momentum between landmarks.
    // Avoid fast tremor on the yaw-only head and the tail mechanism.
    const float phase = static_cast<float>(now_ms) * .0022f;
    pose.tail_lr += std::sin(phase) * 6.5f * life;
    pose.tail_ud += std::cos(phase) * 4.2f * life;
}

wzh_panda::Pose PandaController::ApplyAudioMotion(const wzh_panda::Pose& pose,
                                                  const PandaMotionData& audio, uint32_t now_ms) {
    Pose base = pose;
    const bool body_control = clip_active_ && clip_ && std::strncmp(clip_->name, "body_", 5) == 0;
    const auto offset =
        voice_motion_.Update(wzh::VoiceMechanism::kTail, audio.playing && !body_control,
                             audio.level, audio.attack, now_ms, speech_intent_.load());
    const bool reserve = audio.playing && (!request_active_.load() ||
                                           running_priority_.load() != wzh::PetPriority::kExplicit);
    if (reserve) {
        // Keep most of the clip's own large pose during speech; the voice
        // offsets are layered on top instead of replacing the gesture.
        base = {90 + (pose.head - 90) * .45f * std::max(0.f, 1 - std::abs(offset[0]) / 82),
                90 + (pose.tail_lr - 90) * .35f * std::max(0.f, 1 - std::abs(offset[1]) / 84),
                150 + (pose.tail_ud - 150) * .20f * std::max(0.f, 1 - std::abs(offset[2]) / 136)};
    }
    if (reserve)
        return {std::clamp(base.head + offset[0], 9.f, 171.f),
                std::clamp(base.tail_lr + offset[1], 7.f, 173.f),
                std::clamp(base.tail_ud + offset[2], 11.f, 177.f)};
    return {base.head + offset[0], base.tail_lr + offset[1], base.tail_ud + offset[2]};
}

void PandaController::TaskEntry(void* arg) { static_cast<PandaController*>(arg)->Task(); }

void PandaController::ParkForShutdown() {
    request_active_.store(false);
    speaking_.store(false);
    speech_tail_.store(false);
    clip_active_ = false;
    const auto origin = last_output_pose_;
    const float tail_rest = wzh::g_tail_ud_remap ? 150.f : SERVO_TAIL_UD_DEFAULT;
    const Pose rest = {SERVO_HEAD_DEFAULT, SERVO_TAIL_LR_DEFAULT, tail_rest};
    const float travel =
        std::max({std::abs(origin.head - rest.head), std::abs(origin.tail_lr - rest.tail_lr),
                  std::abs(origin.tail_ud - rest.tail_ud)});
    const uint32_t duration = std::min<uint32_t>(2200, wzh::SettlingDuration(travel, 90.f));
    const uint32_t started = esp_timer_get_time() / 1000;
    quiet_center_.Reset();
    quiet_center_.Sample({origin.head, origin.tail_lr, origin.tail_ud},
                         {rest.head, rest.tail_lr, rest.tail_ud}, started);
    while (servo_output_.powered() && !servo_output_.blocked()) {
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() / 1000 - started);
        const float mix = wzh::SettlingProgress(float(elapsed) / duration);
        OutputPose(Pose{origin.head + (rest.head - origin.head) * mix,
                        origin.tail_lr + (rest.tail_lr - origin.tail_lr) * mix,
                        origin.tail_ud + (rest.tail_ud - origin.tail_ud) * mix});
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

bool PandaController::UpdateSoundMotion(uint32_t now) {
    if (audio_ == nullptr || conversation_active_.load()) {
        CancelSoundMotion();
        return false;
    }
    const auto sound = audio_->GetMotionData();
    // One continuous clock owns short/long Flash sounds and paired recordings.
    // Never resample a wide wag into sparse frames or truncate its other side.
    const uint32_t random = sound_motion_.NeedsBegin(sound) ? esp_random() : 0;
    wzh::SoundPose out{};
    bool powered = false;
    if (!sound_motion_.Update(
            sound, wzh::PetMechanism::kTail,
            {last_output_pose_.head, last_output_pose_.tail_lr, last_output_pose_.tail_ud, 0, 0},
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
            // A fresh gesture may replace motion while the current Flash call
            // finishes. Stop/rest/conversation still cancel playback explicitly.
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
                const int clip_id =
                    request.interaction != wzh::PetInteraction::kNone
                        ? static_cast<int>(C_COUNT) + BODY_COUNT +
                              interaction_motion_.Select(request.interaction, esp_random())
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

        // 在线说话使用真实 TTS 包络，动作助手负责平滑和有限语句编排，
        // 避免把每个音节的 attack 尖峰直接变成头尾抖动。
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
        // every few seconds so the tail never stays parked at neutral.
        const bool idle_eligible = !conversation_active_.load() && pet_state.motion &&
                                   !request_active_.load() && !clip_active_ && !motion.playing &&
                                   !local_motion && !servo_output_.powered();
        if (idle_life_.Due(now_ms, idle_eligible, esp_random())) {
            static constexpr Behaviour kIdleFamilies[] = {Behaviour::kIdle, Behaviour::kCurious,
                                                          Behaviour::kAmbient};
            StartClip(ChooseClip(kIdleFamilies[irnd(std::size(kIdleFamilies))]), now_ms);
            idle_life_.Busy(now_ms, esp_random());
        }
        UpdatePlayer(now_ms);
        if (!clip_active_) {
            request_active_.store(false);
            running_speech_bound_ = false;
        }
        Pose expressive = pose_;
        if (clip_active_ && !motion.playing) {
            AddMicroMotion(expressive, now_ms, 1.0f);
        }
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
            return Behaviour::kStartled;
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
        case PandaBodyAction::kWagTail:
            return BODY_WAG_TAIL;
        case PandaBodyAction::kTwistNeck:
            return BODY_TWIST_NECK;
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
        case PandaBodyAction::kTailUp:
            return BODY_TAIL_UP;
        case PandaBodyAction::kTailDown:
            return BODY_TAIL_DOWN;
        case PandaBodyAction::kTailLeft:
            return BODY_TAIL_LEFT;
        case PandaBodyAction::kTailRight:
            return BODY_TAIL_RIGHT;
        case PandaBodyAction::kTailCenter:
            return BODY_TAIL_CENTER;
        case PandaBodyAction::kTailLevel:
            return BODY_TAIL_LEVEL;
        case PandaBodyAction::kTailSwayLeft:
            return BODY_TAIL_SWAY_LEFT;
        case PandaBodyAction::kTailSwayRight:
            return BODY_TAIL_SWAY_RIGHT;
        case PandaBodyAction::kTailCircle:
            return BODY_TAIL_CIRCLE;
        case PandaBodyAction::kTailEight:
            return BODY_TAIL_EIGHT;
        case PandaBodyAction::kTailCurl:
            return BODY_TAIL_CURL;
        case PandaBodyAction::kWagTailSlow:
            return BODY_WAG_TAIL_SLOW;
        case PandaBodyAction::kTailFan:
            return BODY_TAIL_FAN;
        case PandaBodyAction::kTailBounce:
            return BODY_TAIL_BOUNCE;
        default:
            return -1;
    }
}

bool PandaController::ParseBodyAction(const std::string& name, PandaBodyAction& action) {
    const auto token = wzh::CanonicalBodyCommand(name, wzh::kBodyTail);
    if (token.empty())
        return false;
    static const struct {
        const char* name;
        PandaBodyAction action;
    } actions[] = {
        {"shake_head", PandaBodyAction::kShakeHead},
        {"look_left", PandaBodyAction::kLookLeft},
        {"look_right", PandaBodyAction::kLookRight},
        {"wag_tail", PandaBodyAction::kWagTail},
        {"twist_neck", PandaBodyAction::kTwistNeck},
        {"head_center", PandaBodyAction::kHeadCenter},
        {"head_sway_left", PandaBodyAction::kHeadSwayLeft},
        {"head_sway_right", PandaBodyAction::kHeadSwayRight},
        {"head_scan", PandaBodyAction::kHeadScan},
        {"center", PandaBodyAction::kCenter},
        {"tail_up", PandaBodyAction::kTailUp},
        {"tail_down", PandaBodyAction::kTailDown},
        {"tail_left", PandaBodyAction::kTailLeft},
        {"tail_right", PandaBodyAction::kTailRight},
        {"tail_center", PandaBodyAction::kTailCenter},
        {"tail_level", PandaBodyAction::kTailLevel},
        {"tail_sway_left", PandaBodyAction::kTailSwayLeft},
        {"tail_sway_right", PandaBodyAction::kTailSwayRight},
        {"tail_circle", PandaBodyAction::kTailCircle},
        {"tail_eight", PandaBodyAction::kTailEight},
        {"tail_curl", PandaBodyAction::kTailCurl},
        {"wag_tail_slow", PandaBodyAction::kWagTailSlow},
        {"tail_fan", PandaBodyAction::kTailFan},
        {"tail_bounce", PandaBodyAction::kTailBounce},
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
        case PandaBodyAction::kWagTail:
            return "wag_tail";
        case PandaBodyAction::kTwistNeck:
            return "twist_neck";
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
        case PandaBodyAction::kTailUp:
            return "tail_up";
        case PandaBodyAction::kTailDown:
            return "tail_down";
        case PandaBodyAction::kTailLeft:
            return "tail_left";
        case PandaBodyAction::kTailRight:
            return "tail_right";
        case PandaBodyAction::kTailCenter:
            return "tail_center";
        case PandaBodyAction::kTailLevel:
            return "tail_level";
        case PandaBodyAction::kTailSwayLeft:
            return "tail_sway_left";
        case PandaBodyAction::kTailSwayRight:
            return "tail_sway_right";
        case PandaBodyAction::kTailCircle:
            return "tail_circle";
        case PandaBodyAction::kTailEight:
            return "tail_eight";
        case PandaBodyAction::kTailCurl:
            return "tail_curl";
        case PandaBodyAction::kWagTailSlow:
            return "wag_tail_slow";
        case PandaBodyAction::kTailFan:
            return "tail_fan";
        case PandaBodyAction::kTailBounce:
            return "tail_bounce";
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
        {last_output_pose_.head, last_output_pose_.tail_lr, last_output_pose_.tail_ud},
        {90, 90, 150}, now, 75.f, 250);
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
    if (!wzh::MatchBodyCommand(text, wzh::kBodyTail).empty())
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
                "让小熊猫根据用户此刻的情绪做出肢体动作。"
                "设备是一个小熊猫机器人，通过转头、摆尾巴来回应情绪。"
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
        "执行一个明确的小熊猫动作。仅在用户要求动作或当前语境强烈需要肢体表达时使用。action: "
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
    mcp.AddTool("self.panda.body", wzh::BodyCommandHelp(wzh::kBodyTail),
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
                    return wzh::BodyCommandHelp(wzh::kBodyTail);
                });
    mcp.AddTool("self.panda.get_mode", "返回小熊猫当前 online 或 offline 模式。", PropertyList(),
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
            body = PandaBodyAction::kLookLeft;
            break;
        case wzh::PetInteraction::kTouch3:
            body = PandaBodyAction::kLookRight;
            break;
        case wzh::PetInteraction::kTouch4:
        case wzh::PetInteraction::kPlayful:
            body = PandaBodyAction::kWagTail;
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
