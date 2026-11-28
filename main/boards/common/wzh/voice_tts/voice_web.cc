#include "voice_web.h"

#include <esp_heap_caps.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "application.h"
#include "board.h"
#include "voice_config.h"
#include "voice_identity.h"
#include "voice_names.h"

namespace voice_tts {
namespace {
const char* StatusName(VoiceOperation op) {
    switch (op) {
        case VoiceOperation::kOk:
            return "ok";
        case VoiceOperation::kBusy:
            return "busy";
        case VoiceOperation::kInvalid:
            return "invalid";
        case VoiceOperation::kConfig:
            return "config";
        case VoiceOperation::kNetwork:
            return "network";
        case VoiceOperation::kFull:
            return "full";
        case VoiceOperation::kSave:
            return "save";
        case VoiceOperation::kCancelled:
            return "cancelled";
        case VoiceOperation::kUncertain:
            return "uncertain";
    }
    return "unknown";
}
}  // namespace
uint32_t VoiceWeb::Command(const std::string& action, const std::string& id,
                           const std::string& name) {
    if (action != "list" && action != "select" && action != "rename" && action != "delete")
        return 0;
    if (id.size() > 128 || name.size() > 48 || VoiceResetActive() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        Application::GetInstance().IsVoiceEnrollmentActive() ||
        Application::GetInstance().IsExternalVoicePreviewActive())
        return 0;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true))
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(false);
    action_ = action;
    id_ = id;
    name_ = name;
    status_ = "busy";
    if (xTaskCreate(TaskEntry, "voice_manage", 10240, this, 3, nullptr) != pdPASS) {
        status_ = "memory";
        busy_.store(false);
        return 0;
    }
    if (action == "list") {
        active_request_ = 0;
        return 1;  // Internal refresh, with no result for a user action.
    }
    return TrackRequestLocked();
}

uint32_t VoiceWeb::TrackRequestLocked() {
    if (++next_request_ == 0)
        ++next_request_;
    active_request_ = next_request_;
    results_[(active_request_ - 1) % results_.size()] = {active_request_, "busy"};
    return active_request_;
}

uint32_t VoiceWeb::Upload(uint8_t* data, size_t length) {
    if (!data || length < 2 || VoiceResetActive() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        Application::GetInstance().IsVoiceEnrollmentActive() ||
        Application::GetInstance().IsExternalVoicePreviewActive())
        return 0;
    const size_t name_length = data[0];
    const size_t offset = 1 + name_length;
    if (!name_length || name_length > 48 || offset >= length || (length - offset) % 2 ||
        length - offset < 5 * 16000 * 2 || length - offset > 20 * 16000 * 2)
        return 0;
    std::string name(reinterpret_cast<char*>(data + 1), name_length);
    if (!IsValidVoiceName(name))
        return 0;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true))
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(false);
    const size_t pcm_size = length - offset;
    action_ = "create";
    name_ = std::move(name);
    id_.clear();
    data_ = data;
    sample_count_ = pcm_size / 2;
    status_ = "busy";
    if (xTaskCreate(TaskEntry, "voice_manage", 10240, this, 3, nullptr) != pdPASS) {
        data_ = nullptr;
        sample_count_ = 0;
        status_ = "memory";
        busy_.store(false);
        return 0;
    }
    // Run() acquires the same lock before reading the buffer and request ID.
    std::memmove(data, data + offset, pcm_size);
    return TrackRequestLocked();
}
bool VoiceWeb::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!busy_.load() || action_ != "create")
        return false;
    cancelled_.store(true);
    status_ = "cancelling";
    return true;
}
void VoiceWeb::TaskEntry(void* context) {
    auto* self = static_cast<VoiceWeb*>(context);
    self->Run();
    vTaskDelete(nullptr);
}
void VoiceWeb::Run() {
    std::string action, id, name;
    uint8_t* data = nullptr;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        action = action_;
        id = id_;
        name = name_;
        data = data_;
        count = sample_count_;
        data_ = nullptr;
        sample_count_ = 0;
    }
    VoiceOperation result = VoiceOperation::kInvalid;
    if (action == "create") {
        std::string created;
        result = EnrollManagedVoice(
            reinterpret_cast<const int16_t*>(data), count, name,
            [this] { return cancelled_.load(); }, created);
    } else if (action == "rename")
        result = RenameManagedVoice(id, name);
    else if (action == "select")
        result = SelectManagedVoice(id);
    else if (action == "delete") {
        // A selected clone must be replaced before its cloud record is deleted.
        if (id == LoadAliVoice())
            result = SelectManagedVoice(kBuiltinVoices[0].id);
        else
            result = VoiceOperation::kOk;
        if (result == VoiceOperation::kOk)
            result = DeleteManagedVoice(id);
    }
    free(data);
    std::vector<ManagedVoice> list;
    bool list_loaded = false;
    // Selecting a voice does not change catalog membership or labels. The
    // requested selection already validates cloud IDs when necessary.
    if (action != "select") {
        const auto listed = ListManagedVoices(list);
        list_loaded = listed == VoiceOperation::kOk || listed == VoiceOperation::kUncertain;
        if (action == "list")
            result = listed;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (list_loaded) {
            voices_ = std::move(list);
            have_list_ = true;
        } else if (result == VoiceOperation::kOk && action == "rename") {
            for (auto& voice : voices_)
                if (voice.id == id)
                    voice.name = name;
        } else if (result == VoiceOperation::kOk && action == "delete") {
            voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                         [&](const auto& voice) { return voice.id == id; }),
                          voices_.end());
        }
        status_ = StatusName(result);
        if (active_request_ != 0)
            results_[(active_request_ - 1) % results_.size()].status = StatusName(result);
        busy_.store(false);
    }
}
void VoiceWeb::AddStatus(cJSON* out, bool include_catalog) {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON_AddBoolToObject(out, "voice_management_busy", busy_.load());
    cJSON_AddBoolToObject(out, "voice_management_cancellable",
                          busy_.load() && action_ == "create" && !cancelled_.load());
    cJSON_AddStringToObject(out, "voice_management_status", status_.c_str());
    cJSON_AddBoolToObject(out, "voice_management_loaded", have_list_);
    if (auto* results = cJSON_AddArrayToObject(out, "voice_operations")) {
        for (const auto& result : results_) {
            if (!result.request)
                continue;
            auto* item = cJSON_CreateObject();
            if (!item)
                break;
            if (!cJSON_AddNumberToObject(item, "request", result.request) ||
                !cJSON_AddStringToObject(item, "status", result.status) ||
                !cJSON_AddItemToArray(results, item)) {
                cJSON_Delete(item);
                break;
            }
        }
    }
    if (!include_catalog)
        return;
    cJSON* array = cJSON_AddArrayToObject(out, "voice_catalog");
    if (!array)
        return;
    const auto description = [](const std::string& id) -> const char* {
        for (const auto& builtin : kBuiltinVoices)
            if (id == builtin.id)
                return builtin.description;
        return "";
    };
    auto add = [array, &description](const std::string& id, const std::string& name, bool builtin) {
        auto* item = cJSON_CreateObject();
        if (!item)
            return false;
        if (!cJSON_AddStringToObject(item, "id", id.c_str()) ||
            !cJSON_AddStringToObject(item, "name", name.c_str()) ||
            !cJSON_AddBoolToObject(item, "builtin", builtin) ||
            !cJSON_AddStringToObject(item, "description", builtin ? description(id) : "") ||
            !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            return false;
        }
        return true;
    };
    if (!add("xiaozhi-native", "原声", true))
        return;
    if (have_list_) {
        for (const auto& voice : voices_)
            if (!add(voice.id, voice.name, voice.builtin))
                break;
    } else {
        for (const auto& voice : kBuiltinVoices)
            if (!add(voice.id, voice.name, true))
                break;
    }
}
}  // namespace voice_tts
