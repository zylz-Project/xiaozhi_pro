#ifndef AUDIO_PLAYOUT_BUFFER_H_
#define AUDIO_PLAYOUT_BUFFER_H_

#include <algorithm>
#include <cstdint>

// Output-task-owned, allocation-free jitter admission. A bounded prefix absorbs
// bursty downlink delivery. Once running, the estimated I2S horizon prevents an
// empty software queue from inserting a new delay between every normal packet.
class AudioPlayoutBuffer {
public:
    void Enable(bool enabled) { enabled_ = enabled; }
    void ConfigureExternalSpeech(uint32_t generation) {
        ConfigureSpeech(generation, Speech::kExternal);
    }
    void ConfigureServerSpeech(uint32_t generation) {
        ConfigureSpeech(generation, Speech::kServer);
    }
    bool IsExternalSpeech(uint32_t generation) const {
        return IsNetworkSpeech(generation) && speech_ == Speech::kExternal;
    }
    bool IsNetworkSpeech(uint32_t generation) const {
        return speech_ != Speech::kNone && speech_generation_ == generation;
    }
    void FinishSpeech(uint32_t generation) {
        if (IsNetworkSpeech(generation))
            speech_finished_ = true;
    }
    bool SpeechFinished(uint32_t generation) const {
        return IsNetworkSpeech(generation) && speech_finished_;
    }
    uint32_t WaitMs(uint32_t generation, uint32_t buffered_ms, int64_t now) {
        CheckGeneration(generation);
        if (!enabled_ || !buffered_ms)
            return 0;
        const bool external = IsExternalSpeech(generation);
        const bool server = IsNetworkSpeech(generation) && !external;
        if (!waiting_ && (!primed_ || now > horizon_ms_ + 20)) {
            if (primed_) {
                ++rebuffer_count_;
                rebuffering_ = true;
                if (external) {
                    external_rebuffer_target_ms_ = std::max(
                        external_rebuffer_target_ms_,
                        kExternalRecoveryMs + static_cast<uint32_t>(
                            std::min<int64_t>(now - horizon_ms_, kExternalMaxExtraMs)));
                }
            }
            waiting_ = true;
            since_ms_ = now;
        }
        if (!waiting_)
            return 0;
        const auto elapsed = std::max<int64_t>(0, now - since_ms_);
        // For external speech the caller also counts queued Opus. The extra
        // prefix absorbs read jitter without allocating more decoded PCM.
        uint32_t target_ms = server ? 360 : 120;
        uint32_t max_wait_ms = server ? 380 : 80;
        if (external) {
            target_ms = primed_ ? external_rebuffer_target_ms_ : kExternalInitialMs;
            max_wait_ms = primed_ ? kExternalRecoveryWaitMs : kExternalInitialWaitMs;
        }
        if (SpeechFinished(generation) || buffered_ms >= target_ms || elapsed >= max_wait_ms) {
            waiting_ = false;
            primed_ = true;
            return 0;
        }
        return max_wait_ms - elapsed;
    }
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
    // Network audio must have ready-to-write PCM. An Opus backlog absorbs
    // network jitter, but only counts toward the second watermark.
    struct NetworkSupply {
        uint32_t pcm_ms;
        uint32_t buffered_ms;
        bool pcm_full;
        bool decoding_finished;
        bool waited_for_pcm;
        bool decode_work_pending;
    };

    uint32_t WaitForNetworkPcm(uint32_t generation, const NetworkSupply& supply, int64_t now) {
        CheckGeneration(generation);
        if (!enabled_)
            return 0;
        if (!waiting_) {
            const bool starved = primed_ && supply.waited_for_pcm && horizon_ms_ &&
                                 now > horizon_ms_ + 20;
            if (primed_ && !starved)
                return 0;
            if (starved) {
                ++rebuffer_count_;
                if (supply.decode_work_pending)
                    ++decode_pending_rebuffers_;
                rebuffering_ = true;
            }
            waiting_ = true;
            since_ms_ = now;
        }
        const bool external = IsExternalSpeech(generation);
        const uint32_t target_ms = external ? (primed_ ? 480 : 850) : 0;
        const uint32_t max_wait_ms = primed_ ? 320 : (external ? 1050 : 380);
        const auto elapsed = std::max<int64_t>(0, now - since_ms_);
        if (supply.decoding_finished ||
            ((supply.pcm_ms >= 240 || supply.pcm_full) && supply.buffered_ms >= target_ms) ||
            elapsed >= max_wait_ms) {
            waiting_ = false;
            primed_ = true;
            return 0;
        }
        return max_wait_ms - elapsed;
    }
    uint32_t decode_pending_rebuffers() const { return decode_pending_rebuffers_; }
    uint32_t late_write_count() const { return late_write_count_; }
    uint32_t short_write_count() const { return short_write_count_; }
    void RecordShortWrite(uint32_t generation) {
        if (IsNetworkSpeech(generation))
            ++short_write_count_;
    }
#endif
    void Written(uint32_t duration_ms, int64_t started_ms) {
        if (!first_output_ms_)
            first_output_ms_ = started_ms;
        if (rebuffering_) {
            gap_ms_ += std::max<int64_t>(0, started_ms - horizon_ms_);
            rebuffering_ = false;
        }
        horizon_ms_ = std::max(horizon_ms_, started_ms) + duration_ms;
    }
    void Written(uint32_t duration_ms, int64_t started_ms, int64_t submitted_ms,
                 uint32_t dma_capacity_ms) {
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
        if (horizon_ms_ && IsNetworkSpeech(generation_) && !rebuffering_ &&
            started_ms > horizon_ms_ + 20) {
            // PCM was available at selection time. Count the scheduling/write
            // delay without pausing again to refill an already usable queue.
            ++late_write_count_;
        }
#endif
        Written(duration_ms, started_ms);
        // The writer cannot leave more audio queued than can fit in the DMA ring.
        // A stale estimate can otherwise hide real underflow between packets.
        horizon_ms_ = std::min(horizon_ms_, submitted_ms + dma_capacity_ms);
        const auto write_ms = std::max<int64_t>(0, submitted_ms - started_ms);
        longest_write_ms_ = std::max(longest_write_ms_, static_cast<uint32_t>(write_ms));
        if (write_ms > duration_ms + 25)
            ++long_write_count_;
    }
    uint32_t rebuffer_count() const { return rebuffer_count_; }
    int64_t first_output_ms() const { return first_output_ms_; }
    int64_t gap_ms() const { return gap_ms_; }
    uint32_t longest_write_ms() const { return longest_write_ms_; }
    uint32_t long_write_count() const { return long_write_count_; }

private:
    enum class Speech { kNone, kServer, kExternal };
    void CheckGeneration(uint32_t generation) {
        if (generation_ != generation) {
            generation_ = generation;
            ResetPrefix();
        }
    }
    void ResetPrefix() {
        primed_ = waiting_ = rebuffering_ = false;
        horizon_ms_ = first_output_ms_ = 0;
        external_rebuffer_target_ms_ = kExternalRecoveryMs;
    }
    void ConfigureSpeech(uint32_t generation, Speech speech) {
        speech_generation_ = generation;
        speech_ = speech;
        speech_finished_ = false;
        longest_write_ms_ = long_write_count_ = 0;
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
        decode_pending_rebuffers_ = late_write_count_ = short_write_count_ = 0;
#endif
        if (generation == generation_)
            ResetPrefix();
    }
    static constexpr uint32_t kExternalInitialMs = 850;
    static constexpr uint32_t kExternalRecoveryMs = 950;
    static constexpr uint32_t kExternalMaxExtraMs = 500;
    static constexpr uint32_t kExternalInitialWaitMs = 1050;
    static constexpr uint32_t kExternalRecoveryWaitMs = 1350;
    bool enabled_ = false, primed_ = false, waiting_ = false, rebuffering_ = false;
    uint32_t generation_ = 0, rebuffer_count_ = 0;
    Speech speech_ = Speech::kNone;
    bool speech_finished_ = false;
    uint32_t speech_generation_ = 0;
    uint32_t external_rebuffer_target_ms_ = kExternalRecoveryMs;
    int64_t horizon_ms_ = 0, since_ms_ = 0;
    int64_t first_output_ms_ = 0, gap_ms_ = 0;
    uint32_t longest_write_ms_ = 0, long_write_count_ = 0;
#if CONFIG_BOARD_TYPE_WZH_TAILREDPANDA || CONFIG_WZH_DIALOGUE_PARITY
    uint32_t decode_pending_rebuffers_ = 0, late_write_count_ = 0, short_write_count_ = 0;
#endif
};
#endif
