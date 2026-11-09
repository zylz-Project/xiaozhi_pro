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
bool VoiceWeb::Command(const std::string& action, const std::string& id, const std::string& name) {
    if (action != "list" && action != "select" && action != "rename" && action != "delete")
        return false;
    if (id.size() > 128 || name.size() > 48 || VoiceResetActive() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        Application::GetInstance().IsVoiceEnrollmentActive() ||
        Application::GetInstance().IsExternalVoicePreviewActive())
        return false;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(false);
    action_ = action;
    id_ = id;
    name_ = name;
    status_ = "busy";
    if (xTaskCreate(TaskEntry, "voice_manage", 10240, this, 3, nullptr) != pdPASS) {
        status_ = "memory";
        busy_.store(false);
        return false;
    }
    return true;
}
bool VoiceWeb::Upload(uint8_t* data, size_t length) {
    if (!data || length < 2 || VoiceResetActive() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        Application::GetInstance().IsVoiceEnrollmentActive() ||
        Application::GetInstance().IsExternalVoicePreviewActive())
        return false;
    const size_t name_length = data[0];
    const size_t offset = 1 + name_length;
    if (!name_length || name_length > 48 || offset >= length || (length - offset) % 2 ||
        length - offset < 5 * 16000 * 2 || length - offset > 20 * 16000 * 2)
        return false;
    std::string name(reinterpret_cast<char*>(data + 1), name_length);
    if (!IsValidVoiceName(name))
        return false;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(false);
    const size_t pcm_size = length - offset;
    std::memmove(data, data + offset, pcm_size);
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
        return false;
    }
    return true;
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
    auto listed = ListManagedVoices(list);
    if (action == "list")
        result = listed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listed == VoiceOperation::kOk || listed == VoiceOperation::kUncertain) {
            voices_ = std::move(list);
            have_list_ = true;
        }
        status_ = StatusName(result);
        busy_.store(false);
    }
}
void VoiceWeb::AddStatus(cJSON* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON_AddBoolToObject(out, "voice_management_busy", busy_.load());
    cJSON_AddBoolToObject(out, "voice_management_cancellable",
                          busy_.load() && action_ == "create" && !cancelled_.load());
    cJSON_AddStringToObject(out, "voice_management_status", status_.c_str());
    cJSON_AddBoolToObject(out, "voice_management_loaded", have_list_);
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
