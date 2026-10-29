"""Exercise production wake selection/routing and MultiNet re-arm with host stubs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_application_interaction import function

ROOT = Path(__file__).resolve().parents[2]


class AfeWakeRoutingTest(unittest.TestCase):
    def compile_and_run(self, program, definitions=()):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ is required')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'test.cc'
            source.write_text(program)
            executable = Path(directory) / 'test'
            result = subprocess.run(
                [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                 *definitions, str(source), '-o', str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(executable)], check=True, timeout=5, capture_output=True)

    def test_primary_selection_routing_and_reactivation(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        program = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr bool kUseWakeNet = USE_WN, kUseMultiNetWakeWord = USE_MN, kUseLocalCommands = LOCAL;
using EventBits_t = unsigned;
void xEventGroupSetBits(unsigned* group, unsigned bits) { *group |= bits; }
void xEventGroupClearBits(unsigned* group, unsigned bits) { *group &= ~bits; }
constexpr int WAKENET_DETECTED = 1;
struct afe_fetch_result_t {
    int16_t* data; int data_size; int wakeup_state; int wakenet_model_index;
};
const char* esp_srmodel_get_wake_words(void*, char*) { return "小龙小龙"; }
struct CustomWakeWord {
    bool running = false, local_only = false;
    int feeds = 0, stops = 0;
    std::function<void(const std::string&)> callback;
    void OnWakeWordDetected(std::function<void(const std::string&)> cb) { callback = cb; }
    bool Initialize(void*, void*, bool local) { local_only = local; return true; }
    void Start() { running = true; }
    void Stop() { running = false; ++stops; }
    void FeedMono(const int16_t*, size_t) { assert(running); ++feeds; }
};
struct AfeAudioEngine {
    enum class WakeDetector { kNone, kWakeNet, kMultiNet };
    static constexpr unsigned kWakeWordEnabled = 1;
    WakeDetector wake_detector_ = WakeDetector::kNone;
    std::unique_ptr<CustomWakeWord> custom_wake_word_;
    std::vector<std::string> wake_words_;
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> wake_word_detected_callback_;
    void* models_ = nullptr; void* codec_ = nullptr;
    unsigned bits = 0; unsigned* event_group_ = &bits;
    bool HasWakeWord() const { return wake_detector_ != WakeDetector::kNone; }
    void UpdateActiveState() {}
    bool InitializeWakeWordDetectors(char*, char*);
    void EnableWakeWordDetection(bool);
    void HandleWakeWordResult(const afe_fetch_result_t*);
    void HandleDetectedWakeWord(const std::string&);
};
'''
        for marker in ['bool AfeAudioEngine::InitializeWakeWordDetectors(',
                       'void AfeAudioEngine::EnableWakeWordDetection(',
                       'void AfeAudioEngine::HandleWakeWordResult(',
                       'void AfeAudioEngine::HandleDetectedWakeWord(']:
            program += function(source, marker) + '\n'
        program += r'''
int main() {
    AfeAudioEngine engine;
    char wn[] = "wn9", mn[] = "mn7";
    assert(engine.InitializeWakeWordDetectors(wn, mn));
    assert(engine.HasWakeWord() == (kUseWakeNet || kUseMultiNetWakeWord));
    assert((engine.wake_detector_ == AfeAudioEngine::WakeDetector::kWakeNet) == kUseWakeNet);
    bool needs_mn = kUseMultiNetWakeWord || (kUseWakeNet && kUseLocalCommands);
    assert(bool(engine.custom_wake_word_) == needs_mn);
    if (!engine.HasWakeWord()) return 0;
    std::vector<std::string> detected;
    engine.wake_word_detected_callback_ = [&](const std::string& word) { detected.push_back(word); };
    engine.EnableWakeWordDetection(true);
    assert(engine.bits == 1);
    int16_t pcm[4] = {};
    afe_fetch_result_t result{pcm, sizeof(pcm), 0, 1};
    if (needs_mn) {
        assert(engine.custom_wake_word_->running);
        assert(engine.custom_wake_word_->local_only == kUseWakeNet);
        engine.HandleWakeWordResult(&result);
        assert(engine.custom_wake_word_->feeds == 1 && detected.empty());
    }
    if (kUseWakeNet) {
        result.wakeup_state = WAKENET_DETECTED;
        result.wakenet_model_index = 99;
        engine.HandleWakeWordResult(&result);
        assert(detected.empty() && engine.bits == 1);
        result.wakenet_model_index = 1;
        engine.HandleWakeWordResult(&result);
        assert(detected.size() == 1 && detected.back() == "小龙小龙" && engine.bits == 0);
        if (needs_mn) {
            assert(!engine.custom_wake_word_->running && engine.custom_wake_word_->feeds == 1);
            engine.EnableWakeWordDetection(true);
            assert(engine.custom_wake_word_->running);
            engine.custom_wake_word_->callback("local:stop");
            assert(detected.back() == "local:stop" && engine.bits == 0);
        }
        AfeAudioEngine missing;
        assert(!missing.InitializeWakeWordDetectors(nullptr, mn));
    }
    engine.EnableWakeWordDetection(true);
    engine.EnableWakeWordDetection(false);
    assert(engine.bits == 0);
    if (needs_mn) {
        assert(!engine.custom_wake_word_->running);
        AfeAudioEngine missing;
        assert(!missing.InitializeWakeWordDetectors(wn, nullptr));
    }
}
'''
        for wn, mn, local in [(1, 0, 1), (1, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 0)]:
            with self.subTest(wakenet=wn, multinet=mn, local=local):
                self.compile_and_run(program, [f'-DUSE_WN={wn}', f'-DUSE_MN={mn}', f'-DLOCAL={local}'])

    def test_multinet_callback_can_stop_and_rearm_without_stale_audio(self):
        source = (ROOT / 'main/audio/wake_words/custom_wake_word.cc').read_text()
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
enum esp_mn_state_t { ESP_MN_STATE_DETECTED, ESP_MN_STATE_TIMEOUT };
struct esp_mn_results_t { int num = 1; int command_id[1] = {1}; };
struct Model {
    int cleaned = 0, detects = 0; esp_mn_results_t results;
    int get_samp_chunksize(void*) { return 4; }
    void clean(void*) { ++cleaned; }
    esp_mn_state_t detect(void*, const int16_t*) { ++detects; return ESP_MN_STATE_DETECTED; }
    esp_mn_results_t* get_results(void*) { return &results; }
};
struct Codec { int input_channels() { return 1; } };
struct CustomWakeWord {
    struct Command { std::string command, text, action; };
    Model model; Model* multinet_ = &model;
    void* multinet_model_data_ = &model;
    Codec codec; Codec* codec_ = &codec;
    std::atomic<bool> running_{false}, reset_pending_{true};
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    std::deque<Command> commands_{{"ting xia", "local:stop", "wake"}};
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> wake_word_detected_callback_;
    void Start(); void Stop();
    void FeedSamples(const int16_t*, size_t, bool);
};
'''
        for marker in ['void CustomWakeWord::Start()', 'void CustomWakeWord::Stop()',
                       'void CustomWakeWord::FeedSamples(']:
            program += function(source, marker) + '\n'
        program += r'''
int main() {
    CustomWakeWord word;
    int events = 0;
    word.wake_word_detected_callback_ = [&](const std::string& value) {
        assert(value == "local:stop"); ++events; word.Stop(); word.Start();
    };
    int16_t pcm[8] = {};
    word.Start();
    word.FeedSamples(pcm, 8, true);
    assert(events == 1 && word.input_buffer_.empty());
    word.FeedSamples(pcm, 4, true);
    assert(events == 2 && word.model.detects == 2);
    word.FeedSamples(pcm, 2, true);
    word.Stop(); word.Start();
    word.FeedSamples(pcm, 2, true);
    assert(events == 2);
    word.FeedSamples(pcm, 2, true);
    assert(events == 3 && word.input_buffer_.empty());
}
'''
        self.compile_and_run(program)


if __name__ == '__main__':
    unittest.main()
