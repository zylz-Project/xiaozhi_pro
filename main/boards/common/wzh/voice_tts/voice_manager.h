#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace voice_tts {
struct ManagedVoice {
    std::string id;
    std::string name;
    bool builtin = false;
};
enum class VoiceOperation {
    kOk,
    kBusy,
    kInvalid,
    kConfig,
    kNetwork,
    kFull,
    kSave,
    kCancelled,
    kUncertain
};

// The reset reservation excludes new voice operations until Wi-Fi is cleared.
bool BeginVoiceReset();
void EndVoiceReset();
bool VoiceResetActive();

// All network operations must run outside the HTTP/audio/application tasks.
// The create, list, rename and delete operations share one ownership mutex.
VoiceOperation ListManagedVoices(std::vector<ManagedVoice>& result);
VoiceOperation EnrollManagedVoice(const int16_t* samples, size_t count, const std::string& name,
                                  const std::function<bool()>& cancelled, std::string& created);
VoiceOperation RenameManagedVoice(const std::string& id, const std::string& name);
VoiceOperation DeleteManagedVoice(const std::string& id);
VoiceOperation SelectManagedVoice(const std::string& id);
// Worker only: serializes with voice creation/deletion and stops on first error.
VoiceOperation ResetManagedVoices(const std::function<bool()>& cancelled,
                                  const std::function<void(size_t, size_t)>& progress,
                                  const std::function<void()>& clearing_local = {});
VoiceOperation SelectKnownVoice(const std::string& id, const std::function<bool()>& cancelled);
}  // namespace voice_tts
