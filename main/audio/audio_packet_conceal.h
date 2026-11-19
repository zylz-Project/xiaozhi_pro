#ifndef AUDIO_PACKET_CONCEAL_H_
#define AUDIO_PACKET_CONCEAL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Fills sequence gaps in a lossy downlink audio stream. A gap is a packet that
// never arrived, so there is no Opus payload to hand the decoder; repeating the
// last decoded frame with a decaying gain keeps the speaker fed for the few
// frames a burst takes instead of letting playback stutter into silence.
//
// The class only owns PCM it was given by the decoder, so it is independent of
// the codec and can be exercised on the host.
class AudioPacketConcealment {
public:
    // Consecutive concealed frames are faded to silence over this many frames
    // (8 * 60 ms = 480 ms), after which a longer outage is heard as silence
    // rather than as a repeating tone.
    static constexpr uint32_t kFadeFrames = 8;

    void Reset() {
        last_.clear();
        streak_ = 0;
    }

    // Remember the last real decoded frame. A shorter frame replaces the
    // buffer so a rate or frame-size change can never splice stale samples.
    void Observe(const int16_t* pcm, size_t samples) {
        if (pcm == nullptr || samples == 0) {
            return;
        }
        last_.assign(pcm, pcm + samples);
        streak_ = 0;
    }

    uint32_t streak() const { return streak_; }

    // Build one concealed frame of frame_samples. Returns false when nothing
    // has been decoded yet, so the caller can skip the packet entirely rather
    // than emit a silent frame at the start of a stream.
    bool BuildFrame(std::vector<int16_t>& out, size_t frame_samples, int16_t gain_q8 = 0) {
        out.clear();
        if (last_.empty() || frame_samples == 0) {
            return false;
        }
        ++streak_;
        out.resize(frame_samples);
        // The first concealed frame plays at full volume (it is only one frame
        // behind real audio); each later one drops a step until silence.
        const int32_t gain =
            gain_q8 > 0
                ? gain_q8
                : static_cast<int32_t>(
                      std::max<int64_t>(0, static_cast<int64_t>(kFadeFrames + 1) -
                                               static_cast<int64_t>(streak_)) *
                      256 / kFadeFrames);
        for (size_t i = 0; i < frame_samples; ++i) {
            const int32_t sample = last_[i % last_.size()];
            out[i] = static_cast<int16_t>(std::clamp<int32_t>((sample * gain) / 256, -32768,
                                                              32767));
        }
        return true;
    }

private:
    std::vector<int16_t> last_;
    uint32_t streak_ = 0;
};

#endif
