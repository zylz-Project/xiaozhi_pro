"""Run the hardware-independent protection policies on the host."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WzhProtectionTest(unittest.TestCase):
    def test_battery_and_power_budget_boundaries(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is required for the protection policy test")
        source = r'''
#include <cassert>
#include <cstdint>
#include "wzh_protection.h"
int main() {
    wzh::BatteryProtection battery;
    assert(!battery.ready());
    battery.Update(3300);
    battery.Update(3300);
    assert(!battery.low() && !battery.ready());
    battery.Update(3400); // a transient sag must not latch protection
    battery.Update(3300);
    battery.Update(3300);
    battery.Update(3300);
    assert(battery.low() && !battery.critical());
    for (int i = 0; i < 10; ++i) battery.Update(3399);
    assert(battery.low());
    battery.Update(3400);
    battery.Update(3400);
    assert(battery.low());
    battery.Update(3400);
    assert(!battery.low());
    battery.Update(3000);
    battery.Update(3000);
    battery.Invalidate(); // failed ADC reads break the consecutive sequence
    battery.Update(3000);
    assert(!battery.critical() && !battery.ready());
    battery.Update(3000);
    battery.Update(3000);
    assert(battery.low() && battery.critical());

    wzh::ServoPowerBudget budget;
    budget.PowerOn(100);
    assert(budget.Allow(60099));
    assert(!budget.Allow(60100));
    budget.Release(); // release must not cancel mandatory cooling
    assert(!budget.Allow(65099));
    assert(budget.Allow(65100));
    budget.PowerOn(65100);
    budget.Release();
    budget.PowerOn(100000);
    assert(budget.Allow(159999));
    assert(!budget.Allow(160000));

    wzh::ServoPowerBudget wrap;
    const uint32_t start = UINT32_MAX - 30000;
    wrap.PowerOn(start);
    assert(wrap.Allow(start + uint32_t(59999)));
    assert(!wrap.Allow(start + uint32_t(60000)));
    assert(!wrap.Allow(start + uint32_t(64999)));
    assert(wrap.Allow(start + uint32_t(65000)));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            cpp = directory / "protection.cc"
            executable = directory / "protection"
            cpp.write_text(source)
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main/boards/common/wzh"), str(cpp),
                 "-o", str(executable)], check=True, capture_output=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True)

    def test_faults_cut_power_and_watchdog_releases_without_motion_updates(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is required for the servo output test")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "driver").mkdir()
            (directory / "driver/gpio.h").write_text(
                "#pragma once\nusing gpio_num_t = int;\n"
                "constexpr int GPIO_NUM_NC = -1;\n"
                "int gpio_set_level(gpio_num_t, int);\n"
            )
            (directory / "driver/ledc.h").write_text(
                "#pragma once\n#include <cstdint>\nusing ledc_channel_t = int;\n"
                "constexpr int LEDC_LOW_SPEED_MODE = 0;\n"
                "int ledc_set_duty(int, ledc_channel_t, uint32_t);\n"
                "int ledc_update_duty(int, ledc_channel_t);\n"
                "int ledc_stop(int, ledc_channel_t, int);\n"
            )
            (directory / "esp_log.h").write_text("#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n")
            (directory / "esp_timer.h").write_text(
                "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n"
            )
            cpp = directory / "servo_test.cc"
            cpp.write_text(r'''#include <cassert>
#include "wzh_servo_output.h"
static int power = 0, stops = 0, writes = 0;
static uint32_t first_duty=0;
static int64_t now = 0;
int64_t esp_timer_get_time() { return now; }
int gpio_set_level(gpio_num_t, int value) { power = value; return 0; }
int ledc_set_duty(int, ledc_channel_t channel, uint32_t duty) {
    ++writes;if(channel==0)first_duty=duty;return 0;
}
int ledc_update_duty(int, ledc_channel_t) { return 0; }
int ledc_stop(int, ledc_channel_t, int) { ++stops; return 0; }
int main() {
    const ledc_channel_t channels[] = {0, 1, 2};
    const int angles[] = {90, 90, 150};
    wzh::ServoOutput output;
    output.Initialize(4, channels, 3);
    assert(!power && stops == 3);
    assert(output.blocked() && !output.Write(angles, 3));
    output.SetFault(wzh::kBatteryPending, false);
    assert(output.Write(angles, 3) && power && output.powered());
    float fractional[] = {90,90,150};
    assert(output.WriteDegrees(fractional,3));const auto duty0=first_duty;
    fractional[0]=90.25f;assert(output.WriteDegrees(fractional,3));const auto duty1=first_duty;
    fractional[0]=90.5f;assert(output.WriteDegrees(fractional,3));
    assert(duty1>duty0&&first_duty>duty1); // Sub-degree updates reach actual LEDC duty.
    output.SetFault(wzh::kLowBattery, true);
    output.SetFault(wzh::kOverheat, true);
    const int previous_writes = writes;
    assert(!power && !output.Write(angles, 3) && writes == previous_writes);
    output.SetFault(wzh::kLowBattery, false);
    assert(output.blocked() && !output.Write(angles, 3));
    output.SetFault(wzh::kOverheat, false);
    assert(output.Write(angles, 3) && power);
    for (now = 100000; now < 60000000; now += 100000) {
        assert(output.Write(angles, 3));
        output.Watchdog();
    }
    output.Watchdog(); // continuous power still has a separate 60-second cap
    assert(!power && output.blocked() && output.faults() == 16);
    now = 64999000;
    assert(!output.Write(angles, 3) && !power);
    now = 65000000;
    output.Watchdog();
    assert(!output.blocked() && !power);
    assert(output.Write(angles, 3) && power);
    output.Release();
    assert(!power && !output.powered());
    assert(output.Write(angles, 3));
    now += 499000;
    output.Watchdog();
    assert(power);
    now += 1000;
    output.Watchdog();
    assert(!power && (output.faults() & wzh::kMotionTimeout));
    assert(!output.Write(angles, 3));
    output.SetFault(wzh::kMotionTimeout, false);
    assert(output.Write(angles, 3));
    output.SetFault(wzh::kPetPaused, true);
    output.SetFault(wzh::kMaintenance, true);
    output.SetFault(wzh::kPetPaused, false);
    assert(!power && !output.Write(angles, 3));
    output.SetFault(wzh::kMaintenance, false);
    assert(output.Write(angles, 3));
    output.SetFault(wzh::kShutdown, true);
    assert(!output.Write(angles, 3) && !power);
}
''')
            executable = directory / "servo_test"
            subprocess.run(
                [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                 "-I", str(directory), "-I", str(ROOT / "main/boards/common/wzh"),
                 str(cpp), str(ROOT / "main/boards/common/wzh/wzh_servo_output.cc"),
                 "-o", str(executable)], check=True, capture_output=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
