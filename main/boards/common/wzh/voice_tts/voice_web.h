#pragma once

#include <array>
#include <atomic>
#include <cstdint>
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
    uint32_t Command(const std::string& action, const std::string& id, const std::string& name);
    // A nonzero request takes ownership of the PSRAM buffer. On failure the caller keeps it.
    uint32_t Upload(uint8_t* data, size_t length);
    bool Cancel();
    bool Busy() const { return busy_.load(); }
    bool BusyWithMutation() {
        std::lock_guard<std::mutex> lock(mutex_);
        return busy_.load() && action_ != "list";
    }
    void AddStatus(cJSON* out, bool include_catalog = true);

private:
    static void TaskEntry(void* context);
    void Run();
    uint32_t TrackRequestLocked();
    std::mutex mutex_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    std::string action_, id_, name_, status_ = "idle";
    struct Result {
        uint32_t request = 0;
        const char* status = "busy";
    };
    std::array<Result, 4> results_{};
    uint32_t next_request_ = 0;
    uint32_t active_request_ = 0;
    std::vector<ManagedVoice> voices_;
    bool have_list_ = false;
    uint8_t* data_ = nullptr;
    size_t sample_count_ = 0;
};
}  // namespace voice_tts
