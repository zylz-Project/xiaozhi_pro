#include "voice_manager.h"

#include <esp_mac.h>
#include <algorithm>
#include <atomic>
#include <mutex>

#include "ali_enrollment.h"
#include "voice_config.h"
#include "voice_identity.h"
#include "voice_names.h"
#include "voice_reservation.h"

namespace voice_tts {
namespace {
std::mutex operations;
std::atomic<bool> reset_active{false};
constexpr auto kNeverCancel = [] { return false; };
std::string DevicePrefix() {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        return {};
    return VoicePrefixFromMac(mac, sizeof(mac));
}
const BuiltinVoice* Builtin(const std::string& id) {
    for (const auto& entry : kBuiltinVoices)
        if (id == entry.id)
            return &entry;
    return nullptr;
}
VoiceOperation LoadRemote(std::vector<std::string>& ids, std::vector<VoiceName>& names,
                          const std::function<bool()>& cancelled, std::string* key_out = nullptr,
                          std::string* prefix_out = nullptr) {
    const auto key = LoadAliKey();
    const auto prefix = DevicePrefix();
    if (key.empty() || prefix.empty())
        return VoiceOperation::kConfig;
    if (!LoadVoiceNames(names))
        return VoiceOperation::kSave;
    if (!ListAliVoices(key, prefix, cancelled, ids))
        return cancelled() ? VoiceOperation::kCancelled : VoiceOperation::kNetwork;
    int reserved = -1;
    if (!LoadVoiceReservation(reserved))
        return VoiceOperation::kSave;
    if (reserved >= 0 && ids.size() > static_cast<size_t>(reserved) && !SaveVoiceReservation(-1))
        return VoiceOperation::kSave;
    if (key_out)
        *key_out = key;
    if (prefix_out)
        *prefix_out = prefix;
    return VoiceOperation::kOk;
}
bool NameAllowed(const std::string& name, const std::string&, const std::vector<VoiceName>&) {
    return IsValidVoiceName(name);
}
}  // namespace

bool BeginVoiceReset() {
    bool expected = false;
    return reset_active.compare_exchange_strong(expected, true);
}
void EndVoiceReset() { reset_active.store(false); }
bool VoiceResetActive() { return reset_active.load(); }

VoiceOperation ListManagedVoices(std::vector<ManagedVoice>& result) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    if (reset_active.load())
        return VoiceOperation::kBusy;
    std::vector<std::string> ids;
    std::vector<VoiceName> names;
    const auto status = LoadRemote(ids, names, kNeverCancel);
    if (status != VoiceOperation::kOk)
        return status;
    result.clear();
    for (const auto& builtin : kBuiltinVoices)
        result.push_back({builtin.id, builtin.name, true});
    for (const auto& id : ids) {
        auto match = std::find_if(names.begin(), names.end(),
                                  [&](const VoiceName& n) { return n.id == id; });
        const std::string label =
            match == names.end() ? "音色" + id.substr(id.size() - 4) : match->name;
        result.push_back({id, label, false});
    }
    int reserved = -1;
    if (!LoadVoiceReservation(reserved))
        return VoiceOperation::kSave;
    return reserved >= 0 ? VoiceOperation::kUncertain : VoiceOperation::kOk;
}
VoiceOperation EnrollManagedVoice(const int16_t* samples, size_t count, const std::string& name,
                                  const std::function<bool()>& cancelled, std::string& created) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    if (reset_active.load())
        return VoiceOperation::kBusy;
    created.clear();
    std::vector<std::string> ids;
    std::vector<VoiceName> names;
    std::string key, prefix;
    const auto status = LoadRemote(ids, names, cancelled, &key, &prefix);
    if (status != VoiceOperation::kOk)
        return status;
    if (ids.size() >= 10)
        return VoiceOperation::kFull;
    int reserved = -1;
    if (!LoadVoiceReservation(reserved))
        return VoiceOperation::kSave;
    if (reserved >= 0)
        return VoiceOperation::kUncertain;
    // Avoid creating a paid cloud voice if there is no room to persist its label.
    // A stale or corrupt local catalog must be reconciled before creating more.
    if (names.size() >= 10)
        return VoiceOperation::kSave;
    std::string label = name;
    if (label.empty()) {
        for (int i = 1; i <= 100; ++i) {
            const auto candidate = "我的音色" + std::to_string(i);
            if (NameAllowed(candidate, "", names) &&
                std::none_of(names.begin(), names.end(),
                             [&](const VoiceName& entry) { return entry.name == candidate; })) {
                label = candidate;
                break;
            }
        }
    }
    if (!NameAllowed(label, "", names))
        return VoiceOperation::kInvalid;
    if (!SaveVoiceReservation(static_cast<int>(ids.size())))
        return VoiceOperation::kSave;
    int http_status = 0;
    bool acknowledged = false;
    bool submitted = false;
    if (!CreateAliVoice(key, samples, count, cancelled, created, &http_status, prefix,
                        &acknowledged, &submitted)) {
        if (!submitted ||
            (!acknowledged && http_status >= 400 && http_status < 500 && http_status != 408))
            SaveVoiceReservation(-1);
        return cancelled() ? VoiceOperation::kCancelled : VoiceOperation::kNetwork;
    }
    // Keep the cloud ID in `created` on local failure so callers can explain recovery.
    if (!SetVoiceName(created, label) || !CommitAliVoice(created) || !SaveVoiceReservation(-1))
        return VoiceOperation::kSave;
    return VoiceOperation::kOk;
}
VoiceOperation RenameManagedVoice(const std::string& id, const std::string& name) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    if (reset_active.load())
        return VoiceOperation::kBusy;
    std::vector<std::string> ids;
    std::vector<VoiceName> names;
    const auto status = LoadRemote(ids, names, kNeverCancel);
    if (status != VoiceOperation::kOk)
        return status;
    if (std::find(ids.begin(), ids.end(), id) == ids.end() || !NameAllowed(name, id, names))
        return VoiceOperation::kInvalid;
    return SetVoiceName(id, name) ? VoiceOperation::kOk : VoiceOperation::kSave;
}
VoiceOperation DeleteManagedVoice(const std::string& id) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    if (reset_active.load())
        return VoiceOperation::kBusy;
    std::vector<std::string> ids;
    std::vector<VoiceName> names;
    std::string key, prefix;
    const auto status = LoadRemote(ids, names, kNeverCancel, &key, &prefix);
    if (status != VoiceOperation::kOk)
        return status;
    if (std::find(ids.begin(), ids.end(), id) == ids.end() || id == LoadAliVoice())
        return VoiceOperation::kInvalid;
    if (!DeleteAliVoice(key, prefix, id, kNeverCancel))
        return VoiceOperation::kNetwork;
    const bool previous_ok = ForgetPreviousAliVoice(id);
    const bool names_ok = RemoveVoiceName(id);
    return previous_ok && names_ok ? VoiceOperation::kOk : VoiceOperation::kSave;
}
VoiceOperation ResetManagedVoices(const std::function<bool()>& cancelled,
                                  const std::function<void(size_t, size_t)>& progress,
                                  const std::function<void()>& clearing_local) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    std::vector<std::string> ids;
    std::vector<VoiceName> names;
    std::string key, prefix;
    auto status = LoadRemote(ids, names, cancelled, &key, &prefix);
    if (status != VoiceOperation::kOk)
        return status;
    int unresolved = -1;
    if (!LoadVoiceReservation(unresolved))
        return VoiceOperation::kSave;
    if (unresolved >= 0)
        return VoiceOperation::kUncertain;
    size_t completed = 0;
    size_t total = ids.size();
    if (progress)
        progress(completed, total);
    // Provider list is bounded to 11 IDs per fetch. Re-list after each batch so
    // no matching cloud voice is missed even if a prior client exceeded limits.
    while (!ids.empty()) {
        for (const auto& id : ids) {
            if (cancelled())
                return VoiceOperation::kCancelled;
            bool removed = false;
            for (int attempt = 0; attempt < 2 && !removed && !cancelled(); ++attempt) {
                removed = DeleteAliVoice(key, prefix, id, cancelled, 5000);
                if (!removed) {
                    std::vector<std::string> remaining;
                    if (ListAliVoices(key, prefix, cancelled, remaining) && remaining.size() < 11 &&
                        std::find(remaining.begin(), remaining.end(), id) == remaining.end())
                        removed = true;  // Already absent (including a prior successful retry).
                }
            }
            if (!removed)
                return cancelled() ? VoiceOperation::kCancelled : VoiceOperation::kNetwork;
            // Commit local reconciliation immediately after each confirmed cloud deletion.
            if (id == LoadAliVoice() && !SaveAliVoice(kBuiltinVoices[0].id))
                return VoiceOperation::kSave;
            if (!ForgetPreviousAliVoice(id) || !RemoveVoiceName(id))
                return VoiceOperation::kSave;
            if (++completed > 64)
                return VoiceOperation::kNetwork;
            if (progress)
                progress(completed, total);
        }
        ids.clear();
        if (!ListAliVoices(key, prefix, cancelled, ids))
            return cancelled() ? VoiceOperation::kCancelled : VoiceOperation::kNetwork;
        total = completed + ids.size();
        if (progress)
            progress(completed, total);
    }
    // Clean stale local entries left by previously interrupted cloud operations.
    if (clearing_local)
        clearing_local();
    if (!LoadVoiceNames(names))
        return VoiceOperation::kSave;
    for (const auto& entry : names)
        if (!RemoveVoiceName(entry.id))
            return VoiceOperation::kSave;
    if (!ClearAliVoiceSelection() || !SaveVoiceReservation(-1))
        return VoiceOperation::kSave;
    return VoiceOperation::kOk;
}

VoiceOperation SelectManagedVoice(const std::string& id) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    if (reset_active.load())
        return VoiceOperation::kBusy;
    if (!Builtin(id)) {
        std::vector<std::string> ids;
        std::vector<VoiceName> names;
        const auto status = LoadRemote(ids, names, kNeverCancel);
        if (status != VoiceOperation::kOk)
            return status;
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            return VoiceOperation::kInvalid;
    }
    return SaveAliVoice(id) ? VoiceOperation::kOk : VoiceOperation::kSave;
}

VoiceOperation SelectKnownVoice(const std::string& id, const std::function<bool()>& cancelled) {
    std::unique_lock<std::mutex> lock(operations, std::try_to_lock);
    if (!lock.owns_lock())
        return VoiceOperation::kBusy;
    if (reset_active.load())
        return VoiceOperation::kBusy;
    if (!Builtin(id)) {
        std::vector<VoiceName> names;
        if (!LoadVoiceNames(names))
            return VoiceOperation::kSave;
        if (std::none_of(names.begin(), names.end(),
                         [&](const VoiceName& n) { return n.id == id; }))
            return VoiceOperation::kInvalid;
    }
    if (cancelled())
        return VoiceOperation::kCancelled;
    return SaveAliVoice(id) ? VoiceOperation::kOk : VoiceOperation::kSave;
}
}  // namespace voice_tts
