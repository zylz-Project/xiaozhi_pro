"""Exercise production pet policies on the host, without ESP-IDF or hardware."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WzhPetBehaviorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("g++ is required")
        cls.directory = tempfile.TemporaryDirectory()
        directory = Path(cls.directory.name)
        source = directory / "pet.cc"
        source.write_text(r'''
#include <cassert>
#include <cstdlib>
#include "wzh_pet_behavior.h"
using namespace wzh;
int main(int argc, char** argv) {
    assert(argc == 2);
    switch (std::atoi(argv[1])) {
    case 0:
        assert(ReactionFromText("我今天不开心") == PetReaction::kComfort);
        assert(ReactionFromText("我很开心") == PetReaction::kHappy);
        assert(ReactionFromText("我很难过") == PetReaction::kComfort);
        assert(ReactionFromText("不要吃饭") == PetReaction::kNone);
        assert(!PositiveMention("别摇头", "摇头"));
        assert(!PositiveMention("不要向左看", "向左看"));
        assert(PositiveMention("不要摇头，向左看", "向左看"));
        assert(ParsePetControl("别摇头") == PetControl::kStop);
        assert(ParsePetControl("不要睡觉") == PetControl::kNone);
        assert(ParsePetControl("晚安") == PetControl::kRest);
        assert(ParsePetControl("sleepy") == PetControl::kRest);
        assert(ParsePetControl("醒醒") == PetControl::kWake);
        break;
    case 1: {
        PetBehavior pet;
        pet.Control(PetControl::kRest, 100);
        assert(!pet.Get(100).motion && !pet.Get(900000).motion);
        pet.Touch(1, 200);
        assert(pet.PollTouch(1500) == TouchResponse::kNone);
        assert(!pet.Offer(PetPriority::kExplicit, 1600));
        pet.SetMaintenance(false);
        assert(!pet.Get(1700).motion); // Network changes never wake the pet.
        pet.Control(PetControl::kWake, 1800);
        assert(pet.Get(1800).motion && pet.Get(1800).local_sound);
        pet.SetMaintenance(true);
        pet.Control(PetControl::kWake, 1900);
        assert(!pet.Get(1900).motion); // Wake cannot bypass an upgrade lock.
        pet.SetMaintenance(false);
        pet.Control(PetControl::kQuiet, 2000);
        assert(pet.Get(2000).motion && !pet.Get(2000).local_sound);
        pet.Control(PetControl::kStop, 2100);
        assert(!pet.Get(2100).motion);
        break;
    }
    case 2: {
        PetBehavior pet;
        assert(pet.Offer(PetPriority::kExplicit, 100));
        assert(!pet.Offer(PetPriority::kEmotion, 500));
        assert(!pet.Offer(PetPriority::kSensor, 2599));
        assert(pet.Offer(PetPriority::kSensor, 2600));
        assert(pet.Offer(PetPriority::kEmotion, 2700));
        assert(pet.Offer(PetPriority::kExplicit, 2800));
        pet.Control(PetControl::kStop, 2900);
        assert(!pet.Offer(PetPriority::kExplicit, 6000));
        pet.Control(PetControl::kWake, UINT32_MAX - 100);
        assert(pet.Offer(PetPriority::kExplicit, UINT32_MAX - 50));
        assert(!pet.Offer(PetPriority::kSensor, 100));
        assert(pet.Offer(PetPriority::kSensor, 2500));
        break;
    }
    case 3: {
        PetBehavior pet;
        pet.Touch(1, 100);
        assert(pet.PollTouch(100) == TouchResponse::kAffection);
        // R-2R code drift while the finger is still down must not restart the
        // response; only a fresh 0 -> nonzero edge starts a new interaction.
        pet.Touch(3, 150);
        assert(pet.PollTouch(150) == TouchResponse::kNone);
        assert(pet.PollTouch(1299) == TouchResponse::kNone);
        assert(pet.PollTouch(1300) == TouchResponse::kHeld);
        assert(pet.Get(1300).gentle && !pet.Get(1300).local_sound);
        assert(pet.PollTouch(10000) == TouchResponse::kNone);
        assert(pet.Get(10000).gentle);
        // A very long hold still gets a soft acknowledgement every 9 seconds.
        assert(pet.PollTouch(10300) == TouchResponse::kHeld);
        assert(pet.Get(10300).gentle);
        pet.Touch(0, 10400);
        assert(pet.PollTouch(10400) == TouchResponse::kReleased);
        assert(!pet.Get(15000).gentle);
        PetBehavior taps;
        for (uint32_t i = 0; i < 3; ++i) {
            taps.Touch(1, 100 + i * 400);
            assert(taps.PollTouch(100 + i * 400) ==
                   (i == 2 ? TouchResponse::kPlayful : TouchResponse::kAffection));
            taps.Touch(0, 200 + i * 400);
        }
        PetBehavior multi;
        multi.Touch(3, 0);
        assert(multi.PollTouch(0) == TouchResponse::kPlayful);
        break;
    }
    case 4: {
        PresenceGreeting presence;
        for (uint32_t now = 0; now < 20000; now += 20)
            assert(!presence.Update(true, now, now >= 17000));
        assert(presence.present()); // A boot-high level never becomes an arrival.
        for (uint32_t now = 20000; now < 24000; now += 20)
            assert(!presence.Update(false, now, true));
        for (uint32_t now = 24000; now < 24180; now += 20)
            assert(!presence.Update(true, now, true));
        assert(presence.Update(true, 24180, true));
        assert(presence.Update(true, 24200, true)); // Brief callback rejection may retry.
        presence.Accepted(24200);
        for (uint32_t now = 24220; now < 90000; now += 20)
            assert(!presence.Update(true, now, true));
        break;
    }
    case 5:
        assert(ExpressiveAngle(132, 90, 8, 172) > 132);
        assert(ExpressiveAngle(48, 90, 8, 172) < 48);
        assert(ExpressiveAngle(90, 90, 8, 172) == 90);
        assert(ExpressiveAngle(150, 150, 10, 178) == 150);
        assert(ExpressiveAngle(0, 90, 8, 172) == 8);
        assert(ExpressiveAngle(180, 90, 8, 172) == 172);
        assert(PetFrameDuration(164, 180) >= 800 && PetFrameDuration(164, 180) <= 850);
        assert(PetFrameDuration(0, 50) >= 40 && PetFrameDuration(0, 50) <= 120);
        for (int travel = 1; travel <= 180; ++travel) {
            assert(travel * 2000.0f / PetFrameDuration(travel, 180) <= kPetServoSpeed + .001f);
        }
        break;
    case 6: {
        PetBehavior pet;
        pet.Gentle(UINT32_MAX - 1000, 5000);
        assert(pet.Get(100).gentle && !pet.Get(100).local_sound);
        assert(!pet.Get(4000).gentle);
        pet.Control(PetControl::kWake, 10000);
        // Waiting does not disable explicit interactions, even after a long silence.
        assert(pet.Get(610000).motion);
        assert(pet.Offer(PetPriority::kSensor, 620000));
        assert(pet.Get(620000).motion);
        break;
    }
    case 8: {
        PetBehavior pet;
        assert(pet.SensorReady(0));
        assert(pet.SensorReady(1000)); // A rejected request must not consume the event.
        pet.SensorAccepted(1000);
        assert(!pet.SensorReady(3999));
        assert(pet.SensorReady(4000));
        pet.SensorAccepted(UINT32_MAX - 1000);
        assert(!pet.SensorReady(1000));
        assert(pet.SensorReady(2000));
        pet.Control(PetControl::kRest, 3000);
        assert(!pet.SensorReady(10000));
        pet.Control(PetControl::kWake, 11000);
        assert(pet.SensorReady(11000));
        pet.SetMaintenance(true);
        assert(!pet.SensorReady(15000));
        break;
    }
    case 9: {
        PetBehavior pet;
        assert(pet.CanPlayLocalSound(0));
        assert(pet.CanPlayLocalSound(1000)); // Failed playback does not consume the budget.
        pet.LocalSoundStarted(1000);
        assert(!pet.CanPlayLocalSound(12999));
        assert(pet.CanPlayLocalSound(13000));
        pet.Control(PetControl::kWake, 2000);
        assert(!pet.CanPlayLocalSound(2000)); // Repeated wake cannot spam local calls.
        pet.LocalSoundStarted(UINT32_MAX - 1000);
        assert(!pet.CanPlayLocalSound(1000));
        assert(pet.CanPlayLocalSound(11000));
        pet.Gentle(15000, 5000);
        assert(!pet.CanPlayLocalSound(16000));
        assert(pet.CanPlayLocalSound(20000));
        pet.Control(PetControl::kQuiet, 21000);
        assert(!pet.CanPlayLocalSound(22000));
        break;
    }
    case 7:
        assert(!RecentAudioOutput(100000, 0));
        assert(RecentAudioOutput(100000, 100000));
        assert(RecentAudioOutput(299999, 100000));
        assert(!RecentAudioOutput(300000, 100000));
        assert(!RecentAudioOutput(99999, 100000));
        break;
    default: return 2;
    }
}
''', encoding="utf-8")
        cls.executable = directory / "pet"
        subprocess.run([compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "main/boards/common/wzh"), str(source),
                        "-o", str(cls.executable)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, case):
        subprocess.run([str(self.executable), str(case)], check=True, capture_output=True)

    def test_sensor_cooldown_requires_an_accepted_event(self): self.run_case(8)
    def test_local_sound_cooldown_and_wraparound(self): self.run_case(9)

    def test_audio_envelope_expires_after_playback(self): self.run_case(7)

    def test_negation_and_comfort(self): self.run_case(0)
    def test_rest_stop_and_maintenance_are_independent(self): self.run_case(1)
    def test_explicit_commands_win_and_priority_expires(self): self.run_case(2)
    def test_touch_hold_release_and_repeated_taps(self): self.run_case(3)
    def test_presence_requires_a_fresh_arrival_and_acceptance(self): self.run_case(4)
    def test_large_travel_preserves_limits_and_bounds_peak_speed(self): self.run_case(5)
    def test_gentleness_wraparound_and_long_wait(self): self.run_case(6)


if __name__ == "__main__":
    unittest.main()
