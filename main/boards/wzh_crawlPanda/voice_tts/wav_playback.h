#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class AudioService;

namespace voice_tts {
// Synchronous bounded producer. Call only on a worker, never the application
// or audio tasks. A decoder reset invalidates expected_generation.
bool PlayWav(AudioService& audio, const std::vector<uint8_t>& wav, int speaker_rate,
             uint32_t expected_generation, const std::function<bool()>& cancelled);
}  // namespace voice_tts
