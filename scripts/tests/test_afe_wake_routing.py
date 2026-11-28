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
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr bool kUseWakeNet = USE_WN, kUseMultiNetWakeWord = USE_MN, kUseLocalCommands = LOCAL,
               kUseProductMultiNet = PRODUCT_MN;
using EventBits_t = unsigned;
void xEventGroupSetBits(unsigned* group, unsigned bits) { *group |= bits; }
void xEventGroupClearBits(unsigned* group, unsigned bits) { *group &= ~bits; }
constexpr int WAKENET_DETECTED = 1;
struct afe_fetch_result_t {
    int16_t* data; int data_size; int wakeup_state; int wakenet_model_index;
};
const char* esp_srmodel_get_wake_words(void*, char*) { return "小龙小龙"; }
struct CustomWakeWord {
    static bool init_ok;
    bool running = false, local_only = false;
    int feeds = 0, stops = 0, contextual_feeds = 0;
    std::function<void(const std::string&)> callback;
    void OnWakeWordDetected(std::function<void(const std::string&)> cb) { callback = cb; }
    bool Initialize(void*, void*, bool local) { local_only = local; return init_ok; }
    void Start() { running = true; }
    void Stop() { running = false; ++stops; }
    void FeedMono(const int16_t*, size_t, bool contextual = false) {
        assert(running); ++feeds; contextual_feeds += contextual;
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
    std::atomic<bool> phrase_interruption_{false};
    bool IsVoiceProcessingEnabled() const { return bits & 2; }
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
    bool real_wakenet = kUseWakeNet && !kUseProductMultiNet;
    assert(engine.HasWakeWord() == (kUseWakeNet || kUseMultiNetWakeWord || kUseProductMultiNet));
    assert((engine.wake_detector_ == AfeAudioEngine::WakeDetector::kWakeNet) == real_wakenet);
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
    int expected_feeds = needs_mn ? 1 : 0;
#if CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND
    if (needs_mn) {
        engine.bits |= 2;
        engine.HandleWakeWordResult(&result);
        if (real_wakenet) {
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
        engine.bits |= 2; // Keep keyword recognition running during speech capture.
        engine.HandleWakeWordResult(&result);
        assert(engine.custom_wake_word_->feeds == ++expected_feeds);
        engine.bits &= ~2;
    }
#endif
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA && CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    engine.phrase_interruption_ = true;
    result.wakeup_state = WAKENET_DETECTED;
    engine.HandleWakeWordResult(&result);
    assert(detected.empty() && engine.bits == 1);
    assert(engine.custom_wake_word_->contextual_feeds == 1);
    assert(engine.custom_wake_word_->feeds == ++expected_feeds);
    engine.phrase_interruption_ = false;
    result.wakeup_state = 0;
#endif
    if (real_wakenet) {
        result.wakeup_state = WAKENET_DETECTED;
        result.wakenet_model_index = 99;
        engine.HandleWakeWordResult(&result);
        assert(detected.empty() && engine.bits == 1);
        result.wakenet_model_index = 1;
        engine.HandleWakeWordResult(&result);
        assert(detected.size() == 1 && detected.back() == "小龙小龙" && engine.bits == 0);
        if (needs_mn) {
            assert(!engine.custom_wake_word_->running && engine.custom_wake_word_->feeds == expected_feeds);
            engine.EnableWakeWordDetection(true);
            assert(engine.custom_wake_word_->running);
            engine.custom_wake_word_->callback("local:stop");
            assert(detected.back() == "local:stop" && engine.bits == 0);
        }
        AfeAudioEngine missing;
        assert(!missing.InitializeWakeWordDetectors(nullptr, mn));
    } else if (kUseProductMultiNet) {
        AfeAudioEngine no_old_model;
        assert(no_old_model.InitializeWakeWordDetectors(nullptr, mn));
        // Even if an old WakeNet result is presented, only MultiNet is fed.
        result.wakeup_state = WAKENET_DETECTED;
        engine.HandleWakeWordResult(&result);
        assert(detected.empty() && engine.bits == 1);
        engine.custom_wake_word_->callback("local:chat_wake");
        assert(detected.back() == "local:chat_wake" && engine.bits == 0);
    }
    engine.EnableWakeWordDetection(true);
    engine.EnableWakeWordDetection(false);
    assert(engine.bits == 0);
    if (needs_mn) {
        assert(!engine.custom_wake_word_->running);
        AfeAudioEngine missing;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        if (real_wakenet) {
            assert(missing.InitializeWakeWordDetectors(wn, nullptr));
            assert(missing.HasWakeWord()&&!missing.custom_wake_word_);
            AfeAudioEngine failed;CustomWakeWord::init_ok=false;
            assert(failed.InitializeWakeWordDetectors(wn, mn));
            assert(failed.HasWakeWord()&&!failed.custom_wake_word_);
        } else
#endif
        assert(!missing.InitializeWakeWordDetectors(wn, nullptr));
        if (kUseProductMultiNet) {
            AfeAudioEngine failed; CustomWakeWord::init_ok = false;
            assert(!failed.InitializeWakeWordDetectors(wn, mn));
        }
    }
}
'''
        for neural in (0, 1):
            for wn, mn, local in [(1, 0, 1), (1, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 0)]:
                with self.subTest(wakenet=wn, multinet=mn, local=local, neural=neural):
                    self.compile_and_run(program, [f'-DUSE_WN={wn}', f'-DUSE_MN={mn}',
                                                   f'-DLOCAL={local}',
                                                   '-DPRODUCT_MN=0',
                                                   f'-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND={neural}'])
        self.compile_and_run(program, ['-DUSE_WN=1', '-DUSE_MN=0', '-DLOCAL=1',
                                       '-DPRODUCT_MN=0',
                                       '-DCONFIG_WZH_DINO_KEYWORD_BARGE_IN=1',
                                       '-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND=0'])
        self.compile_and_run(program, ['-DUSE_WN=1', '-DUSE_MN=0', '-DLOCAL=1',
                                       '-DPRODUCT_MN=1',
                                       '-DCONFIG_BOARD_TYPE_WZH_TAILREDPANDA=1',
                                       '-DCONFIG_WZH_DINO_KEYWORD_BARGE_IN=1',
                                       '-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND=0'])

    def test_multinet_callback_can_stop_and_rearm_without_stale_audio(self):
        source = (ROOT / 'main/audio/wake_words/custom_wake_word.cc').read_text()
        program = r'''
#include <algorithm>
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
constexpr int ESP_MN_RESULT_MAX_NUM = 5;
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
    void FeedSamples(const int16_t*, size_t, bool, bool = false);
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

    def test_tail_matches_configured_keywords_inside_live_recognition(self):
        source = (ROOT / 'main/audio/wake_words/custom_wake_word.cc').read_text()
        program = r'''
#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 1
#define CONFIG_WZH_DINO_KEYWORD_BARGE_IN 1
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
enum esp_mn_state_t { ESP_MN_STATE_DETECTING, ESP_MN_STATE_DETECTED, ESP_MN_STATE_TIMEOUT };
constexpr int ESP_MN_RESULT_MAX_NUM = 5;
struct esp_mn_results_t { int num=0, command_id[5]={}; char raw_string[256]={}; };
struct Model {
    int cleaned=0, detects=0, queried=0;
    esp_mn_state_t state=ESP_MN_STATE_DETECTING;
    esp_mn_results_t result;
    int get_samp_chunksize(void*) { return 4; }
    void clean(void*) { ++cleaned; }
    esp_mn_state_t detect(void*, const int16_t*) { ++detects; return state; }
    esp_mn_results_t* get_results(void*) { ++queried; return &result; }
};
struct Codec { int input_channels() { return 1; } };
struct CustomWakeWord {
    struct Command { std::string command, text, action; };
    Model model; Model* multinet_=&model;
    void* multinet_model_data_=&model;
    Codec codec; Codec* codec_=&codec;
    std::atomic<bool> running_{true}, reset_pending_{true};
    bool supports_phrase_interruption_=true, phrase_interruption_=false;
    bool poll_raw_phrase_=true;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    std::deque<Command> commands_{
        {"ting", "local:interrupt", "wake"},
        {"ni hao", "local:interrupt", "wake"},
        {"xiao wu", "local:interrupt", "wake"},
        {"bai shou", "local:body_wave", "wake"},
    };
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> wake_word_detected_callback_;
    void Start(); void Stop();
    void FeedSamples(const int16_t*, size_t, bool, bool = false);
};
'''
        for marker in ['bool IsInterruptionToken(', 'bool ContainsPinyinPhrase(',
                       'void CustomWakeWord::Start()', 'void CustomWakeWord::Stop()',
                       'void CustomWakeWord::FeedSamples(']:
            program += function(source, marker) + '\n'
        program += r'''
int main() {
    CustomWakeWord word;
    int16_t samples[4]={};
    std::vector<std::string> events;
    word.wake_word_detected_callback_=[&](const std::string& token) {
        events.push_back(token);
        word.Stop(); word.Start();
    };
    const auto feed=[&](const char* pinyin, bool contextual=true, int command_id=0) {
        std::strcpy(word.model.result.raw_string,pinyin);
        word.model.state=command_id?ESP_MN_STATE_DETECTED:ESP_MN_STATE_DETECTING;
        word.model.result.num=command_id?1:0;
        word.model.result.command_id[0]=command_id;
        word.FeedSamples(samples,4,true,contextual);
    };
    for (auto* phrase : {"ting yi xia", "ting xia lai", "qing ni ting",
                         "ting bu yao jiang le", "ma fan ni ting yi xia",
                         "wei xiao wu wo you hua", "wo shuo ni hao ni shou dao le ma"}) {
        const auto before=events.size();
        feed(phrase);
        assert(events.size()==before+1&&events.back()=="local:interrupt");
    }
    const auto before=events.size();
    // Intermediate strings are sampled, with exact command IDs always checked.
    feed("qing ni");
    const int queried=word.model.queried;
    feed("qing ni");
    assert(word.model.queried==queried);
    feed("qing ni");
    assert(word.model.queried==queried+1);
    for (auto* text : {"qing ni", "bu tong", "tingzhi", "shao tingting",
                       "zhe ge ni shuo hao le", ""})
        feed(text);
    feed("qing ni ting",false); // Entirely disable contextual matching in standby.
    feed("bai shou",true,4);   // No unrelated motion dispatch while monitoring.
    assert(events.size()==before);
    feed("bai shou",false,4);
    assert(events.size()==before+1&&events.back()=="local:body_wave");
    feed("",true,1); // Retain the registered command as a fast path.
    assert(events.size()==before+2&&events.back()=="local:interrupt");
    word.supports_phrase_interruption_=false;
    feed("qing ni ting");
    assert(events.size()==before+2);
    word.supports_phrase_interruption_=true;
    word.FeedSamples(samples,2,true,false);
    const int previous=word.model.detects;
    word.FeedSamples(samples,2,true,true);
    assert(word.model.detects==previous); // No samples carried across mode switches.
    std::memset(word.model.result.raw_string,'a',sizeof(word.model.result.raw_string));
    word.FeedSamples(samples,2,true,true);
    assert(events.size()==before+2);
    std::strcpy(word.model.result.raw_string,"qing ni ting");
    word.model.state=ESP_MN_STATE_TIMEOUT;
    word.FeedSamples(samples,4,true,true);
    assert(events.size()==before+3&&events.back()=="local:interrupt");
}
'''
        for selector in ("CONFIG_BOARD_TYPE_WZH_TAILREDPANDA", "CONFIG_WZH_DIALOGUE_PARITY"):
            with self.subTest(selector=selector):
                self.compile_and_run(program.replace("CONFIG_BOARD_TYPE_WZH_TAILREDPANDA",
                                                     selector))


if __name__ == '__main__':
    unittest.main()
