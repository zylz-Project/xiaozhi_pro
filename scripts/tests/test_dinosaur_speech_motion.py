"""Replay live speech envelopes against the dinosaur's five commanded axes."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as fluent_motion


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'main/boards/wzh_dinosaur/dinosaur_speech_motion.h'


class DinosaurSpeechMotionTest(unittest.TestCase):
    def run_motion(self, program):
        fluent_motion.WzhFluentMotionTest.run_cpp(self, f'#include "{HEADER}"\n' + program)

    def test_short_utterance_moves_immediately_and_releases_without_a_step(self):
        self.run_motion(r'''
#include <cassert>
using wzh::SpeechIntent;
using wzh_dino::SpeechMotion;
int main() {
    SpeechMotion motion;
    using Pose = std::array<float, 5>;
    assert(motion.Update(980, 0.f, 0.f, SpeechIntent::kJoy) == Pose{});
    assert(!motion.active());

    // First real 60 ms PCM callback for a .12 mean level through AudioService.
    const auto first = motion.Update(1000, .0576f, .1698f, SpeechIntent::kNeutral);
    auto pose = motion.Update(1020, .0576f, .1698f, SpeechIntent::kNeutral);
    assert(motion.active() && first[0] < -.3f && first[3] > .3f);
    assert(pose[0] < -1.2f && pose[3] > 1.2f);
    pose = motion.Update(1040, .0576f, .1698f, SpeechIntent::kNeutral);

    for (uint32_t now = 1060; now <= 1760; now += 20) {
        const auto previous = pose;
        pose = motion.Update(now, 0.f, 0.f, SpeechIntent::kNeutral);
        for (unsigned axis = 0; axis < pose.size(); ++axis)
            assert(std::abs(pose[axis] - previous[axis]) <= 2.55f);
        if (now == 1080)
            assert(motion.active() && pose[3] > .5f);
    }
    assert(!motion.active() && pose == Pose{});
}
''')

    def test_sentence_intent_changes_posture_without_an_independent_cycle(self):
        self.run_motion(r'''
#include <cassert>
using wzh::SpeechIntent;
using wzh_dino::SpeechMotion;
using Pose = std::array<float, 5>;

Pose Hold(SpeechIntent intent, float level = .24f) {
    SpeechMotion motion;
    Pose pose{};
    for (uint32_t now = 1000; now < 1500; now += 20)
        pose = motion.Update(now, level, 0.f, intent);
    for (uint32_t now = 1500; now < 6000; now += 20) {
        const auto steady = motion.Update(now, level, 0.f, intent);
        for (unsigned axis = 0; axis < pose.size(); ++axis)
            assert(std::abs(steady[axis] - pose[axis]) < .005f);
    }
    return pose;
}

int main() {
    const auto neutral = Hold(SpeechIntent::kNeutral);
    const auto comfort = Hold(SpeechIntent::kComfort);
    const auto joy = Hold(SpeechIntent::kJoy);
    const auto question = Hold(SpeechIntent::kQuestion);
    assert(neutral[0] < 0 && std::abs(neutral[0]) < 5 && std::abs(neutral[2]) < .01f);
    assert(comfort[0] > 4 && comfort[3] < 0 && std::abs(comfort[2]) < 2);
    assert(question[0] < -8 && question[1] > 4 && question[2] > 6);
    assert(joy[3] > neutral[3] + 3 && std::abs(joy[4]) > 6);
    assert(Hold(SpeechIntent::kJoy, .45f)[3] > joy[3] + .45f);
    assert(Hold(SpeechIntent::kJoy, .12f)[3] < joy[3] - .8f);
}
''')

    def test_onsets_are_latched_and_a_long_pause_never_jumps_to_the_target(self):
        self.run_motion(r'''
#include <cassert>
#include <limits>
using wzh::SpeechIntent;
using wzh_dino::SpeechMotion;
int main() {
    SpeechMotion one, held, rapid;
    constexpr auto joy = SpeechIntent::kJoy;
    for (uint32_t now = 1000; now <= 1280; now += 20) {
        const auto expected = one.Update(now, .2f, now == 1000 ? .7f : 0.f, joy);
        // The same callback value persists across several 20 ms motion ticks.
        assert(held.Update(now, .2f, .7f, joy) == expected);
        // Rearming inside the cooldown may not create a delayed extra accent.
        assert(rapid.Update(now, .2f, now == 1020 ? 0.f : .7f, joy) == expected);
    }
    const auto original = one.Update(1300, .2f, 0.f, joy);
    const auto accented = one.Update(1320, .2f, .7f, joy);
    assert(accented[3] > original[3] + .35f);

    const auto moved = one.Update(70000, 100.f, 20.f, SpeechIntent::kComfort);
    constexpr std::array<float, 5> limits{15, 9, 12, 14, 13};
    for (unsigned axis = 0; axis < moved.size(); ++axis) {
        assert(std::isfinite(moved[axis]) && std::abs(moved[axis]) <= limits[axis]);
        assert(std::abs(moved[axis] - accented[axis]) <= 5.05f);
    }
    one.Reset();
    assert(!one.active());
    const auto invalid = one.Update(70020, std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN(), joy);
    assert(!one.active() && invalid == decltype(invalid){});

    SpeechMotion wrapped, normal;
    assert(wrapped.Update(UINT32_MAX - 9, .2f, .3f, joy) ==
           normal.Update(1000, .2f, .3f, joy));
    assert(wrapped.Update(10, .2f, .3f, joy) == normal.Update(1020, .2f, .3f, joy));
}
''')


if __name__ == '__main__':
    unittest.main()
