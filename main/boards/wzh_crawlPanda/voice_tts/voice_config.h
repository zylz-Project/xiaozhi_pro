#pragma once

#include <string>

namespace voice_tts {
// The current firmware does not enable NVS encryption. Never print the key.
std::string LoadAliKey();
bool SaveAliKey(const std::string& key);
std::string LoadAliVoice();
std::string LoadPreviousAliVoice();
bool SaveAliVoice(const std::string& voice);
bool CommitAliVoice(const std::string& voice);
bool RestorePreviousAliVoice();
bool ForgetPreviousAliVoice(const std::string& voice);
}  // namespace voice_tts
