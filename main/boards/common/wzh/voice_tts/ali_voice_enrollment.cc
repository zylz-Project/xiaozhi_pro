#include "ali_voice_enrollment.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>

#include "ali_enrollment.h"
#include "application.h"
#include "board.h"
#include "external_tts.h"
#include "voice_config.h"
#include "voice_identity.h"
#include "voice_manager.h"
#include "voice_names.h"

namespace voice_tts {
namespace {
constexpr size_t kRate = 16000;
constexpr size_t kMaxSamples = 20 * kRate;
constexpr size_t kMinSpeechSamples = 5 * kRate;
constexpr size_t kEndSilenceSamples = 3 * kRate / 2;
constexpr char kTag[] = "AliEnrollment";
}  // namespace

bool AliVoiceEnrollment::Start(std::function<void(bool)> capture_switch,
                               std::function<void(bool)> complete) {
    return StartNamed("", std::move(capture_switch), std::move(complete));
}

bool AliVoiceEnrollment::StartNamed(const std::string& name,
                                    std::function<void(bool)> capture_switch,
                                    std::function<void(bool)> complete) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_) {
        error_.store(Error::kBusy);
        return false;
    }
    if (!capture_switch || !complete || LoadAliKey().empty() ||
        (!name.empty() && !IsValidVoiceName(name))) {
        error_.store(Error::kConfig);
        phase_.store(Phase::kFailed);
        return false;
    }
    error_.store(Error::kNone);
    phase_.store(Phase::kPrompt);
    selection_task_ = false;
    requested_name_ = name;
    selection_deadline_us_.store(0);
    capture_switch_ = std::move(capture_switch);
    complete_ = std::move(complete);
    const auto generation = generation_.fetch_add(1) + 1;
    active_.store(true);
    if (xTaskCreate(TaskEntry, "ali_enroll", 10240, this, 3, &task_) != pdPASS) {
        error_.store(Error::kMemory);
        phase_.store(Phase::kFailed);
        active_.store(false);
        task_ = nullptr;
        capture_switch_ = {};
        complete_ = {};
        return false;
    }
    ESP_LOGI(kTag, "Started enrollment generation=%lu", static_cast<unsigned long>(generation));
    return true;
}

bool AliVoiceEnrollment::Capture(const int16_t* samples, size_t count, bool speech) {
    if (!active_.load())
        return false;
    if (!recording_.load() || !samples || !count)
        return true;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !recording_.load() || !samples_)
        return true;
    const size_t n = std::min(count, kMaxSamples - count_);
    std::memcpy(samples_ + count_, samples, n * sizeof(int16_t));
    count_ += n;
    if (speech) {
        if (!speech_samples_)
            first_speech_ = count_ - n;
        last_speech_ = count_;
        speech_samples_ += n;
        silence_samples_ = 0;
    } else if (speech_samples_) {
        silence_samples_ += n;
    }
    if (count_ == kMaxSamples ||
        (speech_samples_ >= kMinSpeechSamples && silence_samples_ >= kEndSilenceSamples)) {
        recording_.store(false);
        audio_done_.store(true);
        if (task_)
            xTaskNotifyGive(task_);
    }
    return true;
}

void AliVoiceEnrollment::Cancel() {
    selection_deadline_us_.store(0);
    if (!active_.exchange(false))
        return;
    generation_.fetch_add(1);
    recording_.store(false);
    if (auto* speech = Board::GetInstance().GetExternalTts())
        speech->Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    phase_.store(Phase::kCancelled);
    if (task_)
        xTaskNotifyGive(task_);
}

void AliVoiceEnrollment::PublishPhase(Phase phase, uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_.load() && generation == generation_.load())
        phase_.store(phase);
}

const char* AliVoiceEnrollment::Status() const {
    switch (phase_.load()) {
        case Phase::kPrompt:
            return "prompt";
        case Phase::kRecording:
            return "recording";
        case Phase::kProcessing:
            return "processing";
        case Phase::kSuccess:
            return "success";
        case Phase::kFailed:
            return "failed";
        case Phase::kCancelled:
            return "cancelled";
        default:
            return "idle";
    }
}
const char* AliVoiceEnrollment::Failure() const {
    switch (error_.load()) {
        case Error::kBusy:
            return "busy";
        case Error::kConfig:
            return "config";
        case Error::kPrompt:
            return "prompt";
        case Error::kMemory:
            return "memory";
        case Error::kShort:
            return "short";
        case Error::kCloud:
            return "cloud";
        case Error::kAuth:
            return "auth";
        case Error::kLimit:
            return "limit";
        case Error::kVoiceFull:
            return "voice_full";
        case Error::kUncertain:
            return "uncertain";
        case Error::kSave:
            return "save";
        default:
            return "";
    }
}

void AliVoiceEnrollment::TaskEntry(void* arg) {
    auto* self = static_cast<AliVoiceEnrollment*>(arg);
    bool selection = false;
    std::string text;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        selection = self->selection_task_;
        text = self->selection_text_;
        name = self->requested_name_;
    }
    if (selection)
        self->RunSelection(self->generation_.load(), text);
    else
        self->Run(self->generation_.load(), name);
    vTaskDelete(nullptr);
}

bool AliVoiceEnrollment::SelectionPending() const {
    const auto deadline = selection_deadline_us_.load();
    return deadline && esp_timer_get_time() < deadline;
}

bool AliVoiceEnrollment::IsConfigured() const {
    return !LoadAliKey().empty();
}

bool AliVoiceEnrollment::StartSelection(const std::string& text,
                                        std::function<void(bool)> complete) {
    if (text.empty() || text.size() > 512 || !complete)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ || LoadAliKey().empty())
        return false;
    selection_text_ = text;
    selection_task_ = true;
    complete_ = std::move(complete);
    const auto generation = generation_.fetch_add(1) + 1;
    active_.store(true);
    if (xTaskCreate(TaskEntry, "ali_voice_select", 10240, this, 3, &task_) != pdPASS) {
        active_.store(false);
        task_ = nullptr;
        complete_ = {};
        return false;
    }
    ESP_LOGI(kTag, "Started voice selection generation=%lu",
             static_cast<unsigned long>(generation));
    return true;
}

void AliVoiceEnrollment::RunSelection(uint32_t generation, const std::string& text) {
    const auto cancelled = [this, generation]() {
        return generation != generation_.load() || !active_.load();
    };
    std::vector<VoiceName> names;
    const bool loaded = LoadVoiceNames(names);
    bool ok = false;
    if (loaded && !cancelled()) {
        const bool request = text.find("切换音色") != std::string::npos;
        if (request) {
            std::vector<std::string> phrases;
            std::string line = "可以切换的音色有：";
            auto append = [&](const std::string& name) {
                if (line.size() + name.size() + 8 > 440) {
                    line += "。";
                    phrases.push_back(line);
                    line = "还有：";
                }
                line += name + "，";
            };
            for (const auto& builtin : kBuiltinVoices)
                append(builtin.name);
            for (const auto& entry : names)
                append(entry.name);
            line += "你想用哪个音色？";
            phrases.push_back(line);
            ok = true;
            for (const auto& phrase : phrases)
                if (!Speak(phrase.c_str(), generation)) {
                    ok = false;
                    break;
                }
            if (ok && !cancelled())
                selection_deadline_us_.store(esp_timer_get_time() + 60000000);
        } else {
            selection_deadline_us_.store(0);
            std::string chosen;
            size_t longest = 0;
            bool ambiguous = false;
            auto match = [&](const std::string& name, const std::string& id) {
                if (text.find(name) == std::string::npos)
                    return;
                if (name.size() > longest) {
                    chosen = id;
                    longest = name.size();
                    ambiguous = false;
                } else if (name.size() == longest && id != chosen) {
                    ambiguous = true;
                }
            };
            for (const auto& builtin : kBuiltinVoices)
                match(builtin.name, builtin.id);
            for (const auto& entry : names)
                match(entry.name, entry.id);
            if (text.find("取消") != std::string::npos) {
                ok = Speak("好的，保持现在的音色。", generation);
            } else if (ambiguous) {
                ok = Speak("有多个相同名称的音色，请在设备页面选择。", generation);
            } else if (!chosen.empty()) {
                const auto result = SelectKnownVoice(chosen, cancelled);
                if (result == VoiceOperation::kOk)
                    ok = Speak("切换音色完成。", generation);
                else if (!cancelled())
                    ok = Speak("音色暂时无法切换，请稍后再试。", generation);
            } else {
                ok = Speak("没有找到这个音色，请重新说切换音色。", generation);
            }
        }
    } else if (!cancelled()) {
        Speak("音色列表暂时不可用。", generation);
    }
    std::function<void(bool)> complete;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled())
            complete = std::move(complete_);
        selection_text_.clear();
        complete_ = {};
        selection_task_ = false;
        task_ = nullptr;
        active_.store(false);
    }
    if (complete)
        complete(ok);
}

bool AliVoiceEnrollment::Speak(const char* text, uint32_t generation) {
    auto* external = Board::GetInstance().GetExternalTts();
    auto& audio = Application::GetInstance().GetAudioService();
    // First-time enrollment may have a Key but no selected voice yet. Use a
    // built-in voice only for the prompt without persisting a selection.
    const std::string prompt_voice = LoadAliVoice().empty() ? kBuiltinVoices[0].id : "";
    if (!external || !active_.load() || !external->ReadyForVoice(prompt_voice) ||
        generation != generation_.load())
        return false;
    prompt_done_.store(false);
    prompt_ok_.store(false);
    if (!external->BeginWithVoice(audio.GetPlaybackGeneration(), prompt_voice,
                                  [this, generation](bool ok) {
            if (generation != generation_.load())
                return;
            prompt_ok_.store(ok);
            prompt_done_.store(true);
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_)
                xTaskNotifyGive(task_);
        }))
        return false;
    if (!external->Enqueue(text)) {
        external->Cancel();
        return false;
    }
    external->Finish();
    const int64_t deadline = esp_timer_get_time() + 45000000;
    while (generation == generation_.load() && !prompt_done_.load() &&
           esp_timer_get_time() < deadline)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    if (!prompt_done_.load() || !prompt_ok_.load()) {
        external->Cancel();
        return false;
    }
    return generation == generation_.load();
}

void AliVoiceEnrollment::Run(uint32_t generation, const std::string& name) {
    const auto cancelled = [this, generation]() {
        return generation != generation_.load() || !active_.load();
    };
    bool ok = false;
    int16_t* buffer = nullptr;
    if (Speak("那请你从一数到二十吧。", generation) && !cancelled())
        buffer = static_cast<int16_t*>(
            heap_caps_malloc(kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    else if (!cancelled())
        error_.store(Error::kPrompt);
    if (!buffer && !cancelled() && error_.load() == Error::kNone)
        error_.store(Error::kMemory);
    if (buffer && !cancelled()) {
        std::function<void(bool)> capture_switch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples_ = buffer;
            count_ = speech_samples_ = silence_samples_ = 0;
            first_speech_ = last_speech_ = 0;
            audio_done_.store(false);
            recording_.store(true);
            capture_switch = capture_switch_;
        }
        ESP_LOGI(kTag, "Recording reference: up to 20 seconds");
        PublishPhase(Phase::kRecording, generation);
        capture_switch(true);
        const int64_t deadline = esp_timer_get_time() + 20000000;
        while (!cancelled() && !audio_done_.load() && esp_timer_get_time() < deadline)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        recording_.store(false);
        capture_switch(false);
        size_t count = 0, speech = 0, first = 0, last = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            count = count_;
            speech = speech_samples_;
            first = first_speech_;
            last = last_speech_;
            samples_ = nullptr;
        }
        ESP_LOGI(kTag, "Recording complete: samples=%u speech_ms=%u", static_cast<unsigned>(count),
                 static_cast<unsigned>(speech * 1000 / kRate));
        PublishPhase(Phase::kProcessing, generation);
        std::string voice;
        const size_t begin = first > kRate / 4 ? first - kRate / 4 : 0;
        const size_t end = std::min(count, last + kRate / 4);
        if (!cancelled()) {
            if (speech < kMinSpeechSamples || end <= begin) {
                error_.store(Error::kShort);
            } else if (!cancelled()) {
                const auto result =
                    EnrollManagedVoice(buffer + begin, end - begin, name, cancelled, voice);
                if (result == VoiceOperation::kOk) {
                    ok = true;
                    ESP_LOGI(kTag, "Voice is ready and saved");
                } else if (!cancelled()) {
                    error_.store(result == VoiceOperation::kFull        ? Error::kVoiceFull
                                 : result == VoiceOperation::kUncertain ? Error::kUncertain
                                 : result == VoiceOperation::kSave      ? Error::kSave
                                 : result == VoiceOperation::kBusy      ? Error::kBusy
                                                                        : Error::kCloud);
                }
            }
        }
    }
    if (buffer)
        heap_caps_free(buffer);
    if (!cancelled()) {
        // Notify the user with the newly saved voice only after successful commit.
        if (ok)
            Speak("我完成复刻啦。", generation);
        else if (error_.load() == Error::kVoiceFull)
            Speak("音色已经有十个了，请先在管理页面删除一个。", generation);
        else if (error_.load() == Error::kUncertain)
            Speak("上一次复刻状态还没有确认，请先在管理页面刷新音色列表。", generation);
        else
            Speak("复刻没有成功，请稍后再试。", generation);
        PublishPhase(ok ? Phase::kSuccess : Phase::kFailed, generation);
    }
    std::function<void(bool)> complete;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_.store(false);
        samples_ = nullptr;
        if (!cancelled())
            complete = std::move(complete_);
        capture_switch_ = {};
        complete_ = {};
        task_ = nullptr;
        active_.store(false);
    }
    if (complete)
        complete(ok);
}
}  // namespace voice_tts
