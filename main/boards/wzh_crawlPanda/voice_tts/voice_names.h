#pragma once

#include <string>
#include <vector>

namespace voice_tts {
struct VoiceName {
    std::string id;
    std::string name;
};
bool IsValidVoiceName(const std::string& name);
// Local labels never change the provider's immutable voice ID.
// All operations fail closed on a corrupt or unavailable NVS record.
bool LoadVoiceNames(std::vector<VoiceName>& names);
bool SetVoiceName(const std::string& id, const std::string& name);
bool RemoveVoiceName(const std::string& id);
}  // namespace voice_tts
