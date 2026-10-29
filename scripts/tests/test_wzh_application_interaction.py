"""Run the production local-wake and stop branches with deterministic task stubs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class WzhApplicationInteractionTest(unittest.TestCase):
    def test_local_wake_restoration_and_scheduled_stop(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ is required")
        source = (ROOT / "main/application.cc").read_text()
        wake = function(source, "void Application::HandleWakeWordDetectedEvent()")
        wake = wake[:wake.index("    if (!protocol_)")] + "}\n"
        stop = function(source, "void Application::StopInteraction(")
        resume = function(source, "void Application::SetListeningMode(")
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <vector>
enum DeviceState { kDeviceStateStarting, kDeviceStateIdle, kDeviceStateWifiConfiguring,
    kDeviceStateActivating, kDeviceStateSpeaking, kDeviceStateListening,
    kDeviceStateUpgrading, kDeviceStateFatalError, kDeviceStateConnecting,
    kDeviceStateAudioTesting };
using ListeningMode = int;
constexpr int kAbortReasonNone = 0;
namespace Lang{namespace Sounds{constexpr int OGG_POPUP=1;}}
struct Audio {
    void PlaySound(int){}
    bool wake = false, afe = true, voice = true;
    int resets = 0;
    std::string GetLastWakeWord() { return "local:happy"; }
    void EnableWakeWordDetection(bool enabled) { wake = enabled; }
    bool IsAfeWakeWord() { return afe; }
    void ResetDecoder() { ++resets; }
    void EnableVoiceProcessing(bool enabled) { voice = enabled; }
    std::unique_ptr<int> PopPacketFromSendQueue() { return nullptr; }
};
struct Protocol {
    bool opened = true;
    int closed = 0;
    bool IsAudioChannelOpened() { return opened; }
    void CloseAudioChannel() { ++closed; opened = false; }
};
struct Board {
    bool maintenance=false;bool IsAudioMaintenanceActive(){return maintenance;}
    bool local = true;
    static Board& GetInstance() { static Board board; return board; }
    bool HandleLocalVoiceCommand(const std::string&) { return true; }
    bool SupportsLocalInteraction() { return local; }
};
struct Application {
    DeviceState state = kDeviceStateIdle;
    Audio audio_service_;
    Protocol protocol;
    Protocol* protocol_ = &protocol;
    std::atomic<bool> interaction_stopped_{false};
    std::atomic<unsigned> interaction_generation_{0};
    bool play_popup_on_listening_=false,pending_listening_start_=false;
    bool entry_prompt_before_listening_=false;
    int aborted = 0;
    ListeningMode listening_mode_ = 0;
    std::vector<std::function<void()>> scheduled;
    DeviceState GetDeviceState() { return state; }
    void SetDeviceState(DeviceState next) { state = next; }
    void ConfigureWakeWordForListening() { audio_service_.wake = false; }
    void AbortSpeaking(int) { ++aborted; }
    void ClearEarlyAudio() {}
    void Schedule(std::function<void()> callback) { scheduled.push_back(callback); }
    void HandleWakeWordDetectedEvent();
    void StopInteraction(bool play_exit_sound=false);
    void SetListeningMode(ListeningMode mode);
    void Drain() { auto work = std::move(scheduled); for (auto& item : work) item(); }
};
'''
        # The production prefix captures state for the network path outside this branch.
        wake = wake.replace("    auto state = GetDeviceState();\n", "")
        program += wake + stop + resume
        program += r'''
int main() {
    Application app;
    for (auto state : {kDeviceStateStarting, kDeviceStateIdle, kDeviceStateWifiConfiguring,
                       kDeviceStateActivating, kDeviceStateSpeaking, kDeviceStateListening}) {
        app.state = state;
        app.audio_service_.wake = false;
        app.HandleWakeWordDetectedEvent();
        assert(app.audio_service_.wake);
    }
    for (auto state : {kDeviceStateUpgrading, kDeviceStateFatalError, kDeviceStateConnecting,
                       kDeviceStateAudioTesting}) {
        app.state = state;
        app.audio_service_.wake = true;
        app.HandleWakeWordDetectedEvent();
        assert(!app.audio_service_.wake);
    }
    Board::GetInstance().local = false;
    app.state = kDeviceStateListening;
    app.HandleWakeWordDetectedEvent();
    assert(!app.audio_service_.wake);
    app.state = kDeviceStateSpeaking;
    app.StopInteraction();
    assert(app.interaction_stopped_.load());
    assert(app.audio_service_.resets == 0 && app.state == kDeviceStateSpeaking);
    app.Drain();
    assert(app.state == kDeviceStateIdle && app.audio_service_.resets == 1);
    assert(app.protocol.closed == 1 && app.aborted == 1 && !app.audio_service_.voice);
    app.SetListeningMode(2);
    // Listening setup alone must not resurrect a stopped session.
    assert(app.interaction_stopped_.load() && app.state == kDeviceStateListening);
    app.interaction_stopped_.store(false);  // New entry clears before listening.
    app.SetListeningMode(2);
    assert(!app.interaction_stopped_.load() && app.state == kDeviceStateListening);
    app.state = kDeviceStateWifiConfiguring;
    app.StopInteraction();
    app.Drain();
    assert(app.state == kDeviceStateWifiConfiguring && app.protocol.closed == 1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            cpp = directory / "application.cc"
            cpp.write_text(program)
            executable = directory / "application"
            result = subprocess.run(
                [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                 str(cpp), "-o", str(executable)], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
