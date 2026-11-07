#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace voice_tts {
// Performs one create request and bounded status polling in the caller's worker.
bool CreateAliVoice(const std::string& key, const int16_t* samples, size_t count,
                    const std::function<bool()>& cancelled, std::string& voice,
                    int* http_status = nullptr, const std::string& prefix = "panda",
                    bool* create_ack = nullptr, bool* request_submitted = nullptr);
// The list is bounded to the first 11 matching voices: enough to enforce a limit of ten.
bool ListAliVoices(const std::string& key, const std::string& prefix,
                   const std::function<bool()>& cancelled, std::vector<std::string>& voices);
bool DeleteAliVoice(const std::string& key, const std::string& prefix, const std::string& voice,
                    const std::function<bool()>& cancelled);
}  // namespace voice_tts
