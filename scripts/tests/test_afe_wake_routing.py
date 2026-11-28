"""Exercise production wake selection/routing and MultiNet re-arm with host stubs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_firmware_runtime_optimizations as runtime
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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr bool kUseWakeNet = USE_WN, kUseMultiNetWakeWord = USE_MN, kUseLocalCommands = LOCAL;
constexpr bool kUseAfeForVoiceProcessing = true;
using EventBits_t = unsigned;
void xEventGroupSetBits(unsigned* group, unsigned bits) { *group |= bits; }
void xEventGroupClearBits(unsigned* group, unsigned bits) { *group &= ~bits; }
constexpr int WAKENET_DETECTED = 1;
constexpr int VAD_SILENCE = 0, VAD_SPEECH = 1;
struct afe_fetch_result_t {
    int16_t* data; int data_size; int wakeup_state; int wake_word_index;
    int wakenet_model_index; int vad_state;
};
const char* model_words = ";小龙小龙;;第二唤醒词;";
char* esp_srmodel_get_wake_words(void*, char*) {
    if (!model_words) return nullptr;
    auto* result = static_cast<char*>(std::malloc(std::strlen(model_words) + 1));
    assert(result);
    return std::strcpy(result, model_words);
}
struct CustomWakeWord {
    enum class CommandMode { kAll, kStandby, kInterruption };
    static bool init_ok;
    bool running = false, local_only = false;
    bool vad_silence = false;
    CommandMode mode = CommandMode::kAll;
    int feeds = 0, stops = 0;
    std::function<void(const std::string&)> callback;
    void OnWakeWordDetected(std::function<void(const std::string&)> cb) { callback = cb; }
    bool Initialize(void*, void*, bool local) { local_only = local; return init_ok; }
    void Start() { running = true; }
    void Stop() { running = false; ++stops; }
    void FeedMono(const int16_t*, size_t, CommandMode next, bool silence = false) {
        assert(running); ++feeds; mode = next; vad_silence = silence;
    }
};
bool CustomWakeWord::init_ok = true;
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
    bool local_commands_suspended_ = false;
    bool IsVoiceProcessingEnabled() const { return bits & 2; }
    bool HasWakeWord() const { return wake_detector_ != WakeDetector::kNone; }
    void UpdateActiveState() {}
    bool InitializeWakeWordDetectors(char*, char*);
    void EnableWakeWordDetection(bool);
    bool IsLocalCommandCapture() const;
    void HandleWakeWordResult(const afe_fetch_result_t*);
    void HandleDetectedWakeWord(const std::string&);
};
'''
        for marker in ['bool AfeAudioEngine::InitializeWakeWordDetectors(',
                       'void AfeAudioEngine::EnableWakeWordDetection(',
                       'bool AfeAudioEngine::IsLocalCommandCapture(',
                       'void AfeAudioEngine::HandleWakeWordResult(',
                       'void AfeAudioEngine::HandleDetectedWakeWord(']:
            program += function(source, marker) + '\n'
        program += r'''
int main() {
    using CommandMode = CustomWakeWord::CommandMode;
    AfeAudioEngine engine;
    char wn[] = "wn9", mn[] = "mn7";
    assert(engine.InitializeWakeWordDetectors(wn, mn));
    assert(engine.HasWakeWord() == (kUseWakeNet || kUseMultiNetWakeWord));
    assert((engine.wake_detector_ == AfeAudioEngine::WakeDetector::kWakeNet) == kUseWakeNet);
    if (kUseWakeNet) {
        assert((engine.wake_words_ == std::vector<std::string>{"小龙小龙", "第二唤醒词"}));
        for (const char* invalid : {static_cast<const char*>(nullptr), "", ";;;"}) {
            model_words = invalid;
            AfeAudioEngine missing_words;
            assert(!missing_words.InitializeWakeWordDetectors(wn, mn));
        }
        model_words = ";小龙小龙;;第二唤醒词;";
    }
    bool needs_mn = kUseMultiNetWakeWord || (kUseWakeNet && kUseLocalCommands);
    assert(bool(engine.custom_wake_word_) == needs_mn);
    if (!engine.HasWakeWord()) return 0;
    std::vector<std::string> detected;
    engine.wake_word_detected_callback_ = [&](const std::string& word) { detected.push_back(word); };
    engine.EnableWakeWordDetection(true);
    assert(engine.bits == 1);
    int16_t pcm[4] = {};
    afe_fetch_result_t result{pcm, sizeof(pcm), 0, 0, 1, VAD_SILENCE};
    if (needs_mn) {
        assert(engine.custom_wake_word_->running);
        assert(engine.custom_wake_word_->local_only == kUseWakeNet);
        for (int size : {-1, 0, 3}) {
            result.data_size = size;
            engine.HandleWakeWordResult(&result);
        }
        result.data_size = sizeof(pcm);
        result.data = nullptr;
        engine.HandleWakeWordResult(&result);
        assert(engine.custom_wake_word_->feeds == 0);
        result.data = pcm;
        engine.HandleWakeWordResult(&result);
        assert(engine.custom_wake_word_->feeds == 1 && detected.empty());
        assert(engine.custom_wake_word_->mode ==
               (kUseWakeNet ? CommandMode::kStandby : CommandMode::kAll));
        assert(engine.custom_wake_word_->vad_silence == kUseWakeNet);
        result.vad_state = VAD_SPEECH;
    }
    int expected_feeds = needs_mn ? 1 : 0;
#if CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND
    if (needs_mn) {
        engine.bits |= 2;
        engine.HandleWakeWordResult(&result);
        if (kUseWakeNet) {
            assert(engine.custom_wake_word_->feeds == expected_feeds);
            assert(!engine.custom_wake_word_->running);
            int stopped = engine.custom_wake_word_->stops;
            engine.HandleWakeWordResult(&result);
            assert(engine.custom_wake_word_->stops == stopped);
            engine.bits &= ~2;
            engine.HandleWakeWordResult(&result);
            assert(engine.custom_wake_word_->running && !engine.local_commands_suspended_);
        } else {
            // Primary MultiNet wake must keep working during capture.
            engine.bits &= ~2;
        }
        ++expected_feeds;
        assert(engine.custom_wake_word_->feeds == expected_feeds);
    }
#endif
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    if (needs_mn) {
        // A queued WakeNet result must not reset a partial interruption keyword.
        engine.bits |= 2;
        result.wakeup_state = WAKENET_DETECTED;
        int stops = engine.custom_wake_word_->stops;
        engine.HandleWakeWordResult(&result);
        assert(engine.custom_wake_word_->feeds == ++expected_feeds);
        assert(engine.custom_wake_word_->mode == CommandMode::kInterruption);
        assert(engine.custom_wake_word_->stops == stops && detected.empty());
        engine.bits &= ~2;
        assert(!engine.IsLocalCommandCapture());
    }
#endif
    if (kUseWakeNet) {
        result.wakeup_state = WAKENET_DETECTED;
        result.wake_word_index = 99;
        engine.HandleWakeWordResult(&result);
        assert(detected.empty() && engine.bits == 1);
        result.wake_word_index = 2; // Two distinct words in the same WakeNet model.
        engine.HandleWakeWordResult(&result);
        assert(detected.size() == 1 && detected.back() == "第二唤醒词" && engine.bits == 0);
        if (needs_mn) {
            assert(!engine.custom_wake_word_->running && engine.custom_wake_word_->feeds == expected_feeds);
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
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        if (kUseWakeNet) {
            assert(missing.InitializeWakeWordDetectors(wn, nullptr));
            assert(missing.HasWakeWord()&&!missing.custom_wake_word_);
            AfeAudioEngine failed;CustomWakeWord::init_ok=false;
            assert(failed.InitializeWakeWordDetectors(wn, mn));
            assert(failed.HasWakeWord()&&!failed.custom_wake_word_);
        } else
#endif
        assert(!missing.InitializeWakeWordDetectors(wn, nullptr));
    }
}
'''
        for neural in (0, 1):
            for wn, mn, local in [(1, 0, 1), (1, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 0)]:
                with self.subTest(wakenet=wn, multinet=mn, local=local, neural=neural):
                    self.compile_and_run(program, [f'-DUSE_WN={wn}', f'-DUSE_MN={mn}',
                                                   f'-DLOCAL={local}',
                                                   f'-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND={neural}'])
        self.compile_and_run(program, ['-DUSE_WN=1', '-DUSE_MN=0', '-DLOCAL=1',
                                       '-DCONFIG_WZH_DINO_KEYWORD_BARGE_IN=1',
                                       '-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND=0'])

    def test_multinet_keeps_legacy_asset_order_and_filters_only_for_local_commands(self):
        wake = ROOT / 'main/audio/wake_words'
        source = (wake / 'custom_wake_word.cc').read_text()
        header = (wake / 'custom_wake_word.h').read_text()
        command = function(header, 'struct Command {') + ';'
        parse = function(source, 'void CustomWakeWord::ParseWakenetModelConfig()')
        filter_local = function(source, 'if (local_commands_only_) {')
        runtime.FirmwareRuntimeOptimizationsTest.run_cpp(self, r'''
#include "cJSON.h"
#include "esp_log.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
#define ESP_LOGI(...) ((void)0)
struct Assets {
    static Assets& GetInstance() { static Assets assets; return assets; }
    bool GetAssetData(const char* name, void*& data, size_t& size) {
        assert(std::strcmp(name, "index.json") == 0);
        static char index[] = R"index({"multinet_model":{"language":"cn","duration":3000,
          "threshold":0.5,"commands":[
          {"command":"old action","text":"local:old","action":"command"},
          {"command":"xiao tu dou","text":"wake name","action":"wake"},
          {"command":"yao tou","text":"local:shake_head","action":"wake"},
          {"command":"case check","text":"local:old","action":"Wake"},
          {"command":"invalid","text":"local:invalid","action":123},
          {"command":"missing","text":"local:invalid"}]}})index";
        data = index;
        size = sizeof(index) - 1;
        return true;
    }
};
struct CustomWakeWord {
''' + command + r'''
    std::deque<Command> commands_;
    std::string language_;
    int duration_ = 0;
    float threshold_ = 0;
    bool local_commands_only_ = false;
    void ParseWakenetModelConfig();
    void Load(bool local) {
        local_commands_only_ = local;
        ParseWakenetModelConfig();
''' + filter_local + r'''
    }
};
''' + parse + r'''
int main() {
    CustomWakeWord standard;
    standard.Load(false);
    assert(standard.language_ == "cn" && standard.duration_ == 3000 && standard.threshold_ == .5f);
    assert(standard.commands_.size() == 4);
    assert(standard.commands_[0].command == "old action" && !standard.commands_[0].wake);
    assert(standard.commands_[1].command == "xiao tu dou" && standard.commands_[1].wake);
    assert(standard.commands_[2].command == "yao tou" && standard.commands_[2].wake);
    assert(standard.commands_[3].command == "case check" && !standard.commands_[3].wake);
    CustomWakeWord standby;
    standby.Load(true);
    assert(standby.commands_.size() == 1);
    assert(standby.commands_[0].command == "yao tou" && standby.commands_[0].wake);
}
''', use_json=True)

    def test_multinet_mode_filter_rearm_and_short_shutdown_disambiguation(self):
        source = (ROOT / 'main/audio/wake_words/custom_wake_word.cc').read_text()
        program = r'''
#include <atomic>
#include <algorithm>
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
enum esp_mn_state_t { ESP_MN_STATE_DETECTED, ESP_MN_STATE_TIMEOUT, ESP_MN_STATE_DETECTING };
struct esp_mn_results_t {
    int num = 1;
    int command_id[2] = {1, 0};
    float prob[2] = {.8f, .7f};
};
struct Model {
    int cleaned = 0, detects = 0, chunk = 4;
    esp_mn_results_t results;
    esp_mn_state_t state = ESP_MN_STATE_DETECTED;
    int get_samp_chunksize(void*) { return chunk; }
    void clean(void*) { ++cleaned; }
    esp_mn_state_t detect(void*, const int16_t*) { ++detects; return state; }
    esp_mn_results_t* get_results(void*) { return &results; }
};
struct Codec { int input_channels() { return 1; } };
struct CustomWakeWord {
    enum class CommandMode { kAll, kStandby, kInterruption };
    struct Command { std::string command, text; bool wake; };
    struct ShutdownAliasTail {
        bool active = false, cancelled = false;
        size_t elapsed_samples = 0, quiet_samples = 0;
    } shutdown_alias_;
    Model model; Model* multinet_ = &model;
    void* multinet_model_data_ = &model;
    Codec codec; Codec* codec_ = &codec;
    float threshold_ = .5f;
    std::atomic<bool> running_{false}, reset_pending_{true};
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    std::deque<Command> commands_{{"ting", "local:stop", true},
                                 {"ni hao", "local:interrupt", true},
                                 {"bai bai", "local:shutdown", true},
                                 {"xiao long xiao long", "local:chat_wake", true},
                                 {"bai bai wei ba", "local:wag_tail", true},
                                 {"guan ji", "local:shutdown", true},
                                 {"old xing dong", "unsupported", false}};
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> wake_word_detected_callback_;
    void Start(); void Stop();
    void FeedSamples(const int16_t*, size_t, bool, CommandMode, bool = false);
};
'''
        for marker in ['void CustomWakeWord::Start()', 'void CustomWakeWord::Stop()',
                       'void CustomWakeWord::FeedSamples(']:
            program += function(source, marker) + '\n'
        program += r'''
int main() {
    using CommandMode = CustomWakeWord::CommandMode;
    CustomWakeWord word;
    int events = 0;
    word.wake_word_detected_callback_ = [&](const std::string& value) {
        assert(value == "local:stop"); ++events; word.Stop(); word.Start();
    };
    int16_t pcm[8] = {};
    word.Start();
    word.FeedSamples(pcm, 8, true, CommandMode::kStandby);
    assert(events == 1 && word.input_buffer_.empty());
    word.FeedSamples(pcm, 4, true, CommandMode::kStandby);
    assert(events == 2 && word.model.detects == 2);
    word.FeedSamples(pcm, 2, true, CommandMode::kStandby);
    word.Stop(); word.Start();
    word.FeedSamples(pcm, 2, true, CommandMode::kStandby);
    assert(events == 2);
    word.FeedSamples(pcm, 2, true, CommandMode::kStandby);
    assert(events == 3 && word.input_buffer_.empty());
    word.model.results = {2, {2, 1}};
    word.FeedSamples(pcm, 4, true, CommandMode::kStandby);
    assert(events == 3 && word.running_ && word.input_buffer_.empty());
    word.model.results = {1, {7, 0}};
    word.FeedSamples(pcm, 4, true, CommandMode::kAll);
    assert(events == 3 && word.running_ && word.input_buffer_.empty());
    word.model.results = {2, {3, 2}};
    word.FeedSamples(pcm, 4, true, CommandMode::kInterruption);
    assert(events == 3 && word.running_ && word.input_buffer_.empty());
    word.model.results = {2, {2, 1}};
    word.wake_word_detected_callback_ = [&](const std::string& value) {
        assert(value == "local:interrupt"); ++events;
    };
    word.FeedSamples(pcm, 4, true, CommandMode::kInterruption);
    assert(events == 4 && !word.running_);
    word.Start();
    word.model.results = {2, {4, 3}};
    word.wake_word_detected_callback_ = [&](const std::string& value) {
        assert(value == "local:chat_wake"); ++events;
    };
    word.FeedSamples(pcm, 4, true, CommandMode::kInterruption);
    assert(events == 5 && !word.running_);

    std::string selected;
    word.wake_word_detected_callback_ = [&](const std::string& value) { selected = value; };
    int16_t chunk[512] = {};
    word.model.chunk = 512; // 32 ms per AFE frame.
    auto feed = [&](bool quiet, CommandMode current = CommandMode::kStandby) {
        word.FeedSamples(chunk, 512, true, current, quiet);
    };
    auto hit = [&](int command_id) {
        word.model.state = ESP_MN_STATE_DETECTED;
        word.model.results = {1, {command_id, 0}};
        feed(false);
    };
    auto short_phrase = [&]() {
        word.Start(); selected.clear(); hit(3);
        assert(selected.empty() && word.running_ && word.shutdown_alias_.active);
        word.model.state = ESP_MN_STATE_DETECTING;
    };
    auto quiet_tail = [&]() {
        word.model.state = ESP_MN_STATE_DETECTING;
        for (int i = 0; i < 6; ++i) feed(true);
    };

    short_phrase();
    for (int i = 0; i < 5; ++i) {
        feed(true); assert(selected.empty() && word.running_);
    }
    feed(true);
    assert(selected == "local:shutdown" && !word.running_);

    short_phrase(); feed(false); hit(5);
    assert(selected == "local:wag_tail" && !word.running_);

    word.Start(); selected.clear();
    word.model.results = {2, {3, 5}};
    feed(false);
    assert(selected == "local:wag_tail" && !word.running_);

    word.Start(); selected.clear();
    word.model.results = {2, {3, 5}, {.8f, .3f}};
    feed(false);
    assert(selected.empty() && word.running_);
    quiet_tail();
    assert(selected == "local:shutdown" && !word.running_);

    short_phrase();
    for (int i = 0; i < 8; ++i) hit(3); // Repeated prefix results cannot refresh the deadline.
    quiet_tail();
    assert(selected.empty() && word.running_ && !word.shutdown_alias_.active);

    short_phrase(); feed(true); feed(false); quiet_tail();
    assert(selected.empty() && word.running_);

    short_phrase(); word.Stop(); word.Start(); quiet_tail();
    assert(selected.empty() && word.running_);

    short_phrase(); feed(true, CommandMode::kInterruption); quiet_tail();
    assert(selected.empty() && word.running_);

    hit(6); // Explicit shutdown needs no prefix disambiguation delay.
    assert(selected == "local:shutdown" && !word.running_);
}
'''
        self.compile_and_run(program)


if __name__ == '__main__':
    unittest.main()
