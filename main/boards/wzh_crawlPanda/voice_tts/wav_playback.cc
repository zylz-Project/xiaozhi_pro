#include "wav_playback.h"

#include <algorithm>

#include "audio_service.h"
#include "wav_pcm.h"

namespace voice_tts {
bool PlayWav(AudioService& audio, const std::vector<uint8_t>& wav, int speaker_rate,
             uint32_t expected_generation, const std::function<bool()>& cancelled) {
    if (speaker_rate <= 0)
        return false;
    WavPcmView view;
    if (!ParseWavPcm(wav.data(), wav.size(), static_cast<uint32_t>(speaker_rate), view))
        return false;
    const size_t frame_samples = static_cast<size_t>(speaker_rate) * 60 / 1000;
    if (!frame_samples)
        return false;
    for (size_t offset = 0; offset < view.size;) {
        if (cancelled && cancelled())
            return false;
        const size_t samples = std::min(frame_samples, (view.size - offset) / 2);
        std::vector<int16_t> pcm(samples);
        for (size_t i = 0; i < samples; ++i) {
            const uint16_t value = WavU16(view.data + offset + i * 2);
            pcm[i] = value < 0x8000 ? static_cast<int16_t>(value)
                                    : static_cast<int16_t>(static_cast<int32_t>(value) - 65536);
        }
        if (!audio.PushPcmToPlaybackQueue(std::move(pcm), speaker_rate, expected_generation, true))
            return false;
        offset += samples * 2;
    }
    return true;
}
}  // namespace voice_tts
