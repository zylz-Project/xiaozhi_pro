#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voice_manager.h"

namespace voice_tts {
// One bounded worker; HTTP callbacks only receive and enqueue work.
class VoiceWeb {
public:
    bool Command(const std::string& action, const std::string& id, const std::string& name);
    // On success takes ownership of the PSRAM buffer; on failure caller keeps it.
    bool Upload(uint8_t* data, size_t length);
    bool Cancel();
    bool Busy() const { return busy_.load(); }
    void AddStatus(cJSON* out);

private:
    static void TaskEntry(void* context);
    void Run();
    std::mutex mutex_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    std::string action_, id_, name_, status_ = "idle";
    std::vector<ManagedVoice> voices_;
    bool have_list_ = false;
    uint8_t* data_ = nullptr;
    size_t sample_count_ = 0;
};
}  // namespace voice_tts
