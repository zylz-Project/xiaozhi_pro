"""Drive the production idle-countdown worker through the documented timeline."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'main/boards/common/wzh'

HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#define CONFIG_WZH_IDLE_POWER_ENABLE 1
#define CONFIG_WZH_IDLE_AUTO_OFF @AUTO_OFF@
#define CONFIG_WZH_IDLE_MISS_PROMPT @MISS@
#define CONFIG_BOARD_TYPE_WZH_CRAWLPANDA 1
#define CONFIG_WZH_SYSTEM_PROMPTS @PROMPTS@

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

constexpr int kDeviceStateIdle = 0;

uint32_t clock_ms = 0;
bool inside_worker = false, tutorial = false;
void (*entry)(void*) = nullptr;
struct Yield {};

int64_t esp_timer_get_time() { return static_cast<int64_t>(clock_ms) * 1000; }
// One worker tick per Run(): the test owns the clock so a single Advance() maps
// to exactly that much virtual time.
void vTaskDelay(TickType_t) {
    if (inside_worker)
        throw Yield{};
}
void vTaskDelete(void*) {}

struct Audio {
    uint32_t until = 0, generation = 1;
    std::deque<std::string> played;
    bool IsPlaybackIdle() { return clock_ms >= until; }
    bool IsAudioProcessorRunning() { return false; }
    uint32_t GetPlaybackGeneration() { return generation; }
    void PlaySound(std::string_view sound, std::optional<uint32_t> expected = {}) {
        if (expected && *expected != generation) return;
        played.push_back(std::string(sound));
        until = clock_ms + 3000;
    }
};
struct Application {
    int state = kDeviceStateIdle;
    Audio audio;
    static Application& GetInstance() { static Application app; return app; }
    Audio& GetAudioService() { return audio; }
    int GetDeviceState() { return state; }
};
struct Board {
    bool maintenance = false, local_pending = false;
    static Board& GetInstance() { static Board board; return board; }
    bool IsAudioMaintenanceActive() { return maintenance; }
    bool IsLocalPlaybackPending() { return local_pending; }
};
namespace wzh { bool IsOnboardingActive() { return tutorial; } }

#include "IDLE_SOURCE"

int xTaskCreate(void (*fn)(void*), const char*, int, void*, int, void*) {
    entry = fn;
    return pdPASS;
}

// Run the worker for one internal tick, then yield back so the test can
// advance the wall clock.
void Run() {
    if (entry == nullptr) return;
    inside_worker = true;
    try { entry(nullptr); } catch (Yield&) {}
    inside_worker = false;
}

void Advance(uint32_t ms) {
    const uint32_t target = clock_ms + ms;
    while (clock_ms < target) {
        const uint32_t step = target - clock_ms > 100 ? 100 : target - clock_ms;
        clock_ms += step;
        Run();
    }
}
'''


class IdleCountdownTest(unittest.TestCase):
    def compile_run(self, source):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'assets').mkdir()
            (root / 'freertos').mkdir()
            (root / 'esp_log.h').write_text(
                '#pragma once\n'
                'template <class... T> void log_stub(T...) {}\n'
                '#define ESP_LOGI(...) log_stub(__VA_ARGS__)\n'
                '#define ESP_LOGW(...) log_stub(__VA_ARGS__)\n'
                '#define ESP_LOGE(...) log_stub(__VA_ARGS__)\n')
            (root / 'esp_timer.h').write_text(
                '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n')
            (root / 'freertos/FreeRTOS.h').write_text(
                '#pragma once\n#include <cstdint>\n'
                'using TickType_t = uint32_t;\n'
                '#define pdMS_TO_TICKS(ms) (ms)\n'
                'constexpr int pdPASS = 1;\n')
            (root / 'freertos/task.h').write_text(
                '#pragma once\n#include "FreeRTOS.h"\n'
                'int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*);\n'
                'void vTaskDelay(TickType_t);\n'
                'void vTaskDelete(void*);\n')
            (root / 'assets/lang_config.h').write_text(
                '#pragma once\n#include <string_view>\n'
                'namespace Lang { namespace Sounds { constexpr std::string_view '
                'OGG_IDLE_MISS="miss"; } }\n')
            (root / 'application.h').write_text('#pragma once\n')
            (root / 'board.h').write_text('#pragma once\n')
            program = root / 'test.cc'
            program.write_text(source)
            binary = root / 'test'
            result = subprocess.run(
                ['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-I', str(root), '-I', str(COMMON), str(program), '-o', str(binary)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def harness(self, auto_off='1', miss='1', prompts='1'):
        return (HARNESS.replace('IDLE_SOURCE', str(COMMON / 'wzh_idle_power.cc'))
                .replace('@AUTO_OFF@', auto_off).replace('@MISS@', miss)
                .replace('@PROMPTS@', prompts))

    def test_miss_at_ten_minutes_silences_sound_but_never_before(self):
        self.compile_run(self.harness() + r'''
int main(){
    auto& app = Application::GetInstance();
    wzh::StartIdlePower([]{ assert(false); });
    assert(entry);
    Run();
    Advance(299000);
    assert(app.audio.played.empty() && !wzh::IdleSoundSuppressed());
    Advance(2000);  // 4:59 -> 5:01
    assert(app.audio.played.size() == 1 && app.audio.played.front() == "miss");
    // Silence starts only once that sentence has been heard to the end.
    assert(!wzh::IdleSoundSuppressed());
    Advance(3200);
    assert(wzh::IdleSoundSuppressed());
    Advance(240000);
    assert(app.audio.played.size() == 1);  // Exactly one sentence, not a loop.
}
''')

    def test_interaction_resets_quiet_and_rearms_the_prompt(self):
        self.compile_run(self.harness() + r'''
int main(){
    auto& app = Application::GetInstance();
    wzh::StartIdlePower([]{ assert(false); });
    Run();
    Advance(304000);
    assert(app.audio.played.size() == 1 && wzh::IdleSoundSuppressed());
    wzh::NotifyIdleInteraction();
    Advance(100);
    assert(!wzh::IdleSoundSuppressed() && app.audio.played.size() == 1);
    Advance(304000);  // The next miss needs a full five minutes of its own.
    assert(app.audio.played.size() == 2);
    assert(wzh::IdleSoundSuppressed());
}
''')

    def test_auto_off_fires_once_and_leaves_the_goodbye_to_shutdown(self):
        self.compile_run(self.harness() + r'''
int main(){
    auto& app = Application::GetInstance();
    int shutdowns = 0;
    wzh::StartIdlePower([&]{ ++shutdowns; });
    Run();
    Advance(599000);
    assert(shutdowns == 0);
    Advance(2000);  // 9:59 -> 10:01
    assert(shutdowns == 1);
    // Shutdown() plays OGG_POWER_OFF itself; the countdown must not say it too.
    assert(app.audio.played.size() == 1 && app.audio.played.front() == "miss");
    Advance(600000);
    assert(shutdowns == 1);
}
''')

    def test_interaction_after_the_shutdown_request_keeps_the_worker_alive(self):
        self.compile_run(self.harness() + r'''
int main(){
    auto& app = Application::GetInstance();
    int shutdowns = 0;
    wzh::StartIdlePower([&]{ ++shutdowns; });
    Run();
    Advance(601000);
    assert(shutdowns == 1);
    // Shutdown() takes seconds to park the servos. A touch during that window
    // is a real interaction and must not leave the countdown dead for this boot.
    wzh::NotifyIdleInteraction();
    Advance(200);
    assert(!wzh::IdleSoundSuppressed());
    Advance(303000);  // A whole new five minutes has to elapse first.
    assert(app.audio.played.size() == 2);
    assert(shutdowns == 1);  // The re-armed timeline waits its full 10 minutes.
    Advance(300000);
    assert(shutdowns == 2);
}
''')

    def test_auto_off_disabled_never_requests_shutdown(self):
        self.compile_run(self.harness(auto_off='0') + r'''
int main(){
    Application::GetInstance();
    int shutdowns = 0;
    wzh::StartIdlePower([&]{ ++shutdowns; });
    Run();
    Advance(1800000);
    assert(shutdowns == 0);
}
''')

    def test_miss_prompt_disabled_still_enters_quiet_mode_without_speech(self):
        self.compile_run(self.harness(miss='0') + r'''
int main(){
    auto& app = Application::GetInstance();
    wzh::StartIdlePower([]{ assert(false); });
    Run();
    Advance(301000);
    assert(app.audio.played.empty());
    assert(wzh::IdleSoundSuppressed());
}
''')

    def test_tutorial_holds_the_timeline(self):
        self.compile_run(self.harness() + r'''
int main(){
    auto& app = Application::GetInstance();
    wzh::StartIdlePower([]{ assert(false); });
    tutorial = true;
    Run();
    Advance(1200000);  // Twenty minutes of tutorial must not count as silence.
    assert(app.audio.played.empty() && !wzh::IdleSoundSuppressed());
    tutorial = false;
    Advance(304000);
    assert(app.audio.played.size() == 1 && wzh::IdleSoundSuppressed());
}
''')

    def test_conversation_pins_the_timeline_indefinitely(self):
        self.compile_run(self.harness() + r'''
int main(){
    auto& app = Application::GetInstance();
    int shutdowns = 0;
    wzh::StartIdlePower([&]{ ++shutdowns; });
    Run();
    // A long conversation is continuous interaction: the pet must never say
    // "想你" or power off in the middle of one, however long it runs.
    app.state = 1;
    Advance(3600000);
    assert(app.audio.played.empty() && shutdowns == 0);
    // Maintenance is the same kind of "someone is using it" state.
    app.state = kDeviceStateIdle;
    Board::GetInstance().maintenance = true;
    Advance(1800000);
    assert(app.audio.played.empty() && shutdowns == 0);
    Board::GetInstance().maintenance = false;
    // Once the device is handed back, the full countdown starts from zero.
    Advance(304000);
    assert(app.audio.played.size() == 1 && wzh::IdleSoundSuppressed());
    Advance(300000);
    assert(shutdowns == 1);
}
''')

    def test_interaction_before_start_and_after_stop_is_harmless(self):
        self.compile_run(self.harness() + r'''
int main(){
    Application::GetInstance();
    wzh::NotifyIdleInteraction();  // Nothing armed yet.
    wzh::StartIdlePower([]{ assert(false); });
    Run();
    wzh::StopIdlePower();
    wzh::NotifyIdleInteraction();  // No pending work to disturb.
}
''')

    def test_disabled_switch_compiles_to_no_ops(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            program = root / 'test.cc'
            # No CONFIG_WZH_IDLE_POWER_ENABLE at all: the whole TU is a no-op.
            program.write_text(r'''
#include <cassert>
#include <functional>
#include "wzh_idle_power.h"
int main(){
    wzh::NotifyIdleInteraction();
    assert(!wzh::IdleSoundSuppressed());
    wzh::StartIdlePower([]{ assert(false); });
    wzh::StopIdlePower();
}
''')
            binary = root / 'test'
            result = subprocess.run(
                ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-I', str(COMMON), str(program), str(COMMON / 'wzh_idle_power.cc'),
                 '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()