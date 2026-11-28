#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace voice_tts {
struct QueuedText {
    std::string text;
    int64_t enqueued_us;

    int64_t AgeMs(int64_t now_us) const { return (now_us - enqueued_us) / 1000; }
};

// Each cellular synthesis request opens its own HTTPS connection. Let a sole
// short fragment pick up the next one, counting time behind an earlier request.
inline int64_t ShortCellularBatchWaitMs(const std::deque<QueuedText>& queue, bool finished,
                                        int64_t now_us) {
    constexpr int64_t kMaxWaitMs = 1800;
    if (finished || queue.size() != 1 || queue.front().text.size() > 36)
        return 0;
    const auto age_ms = queue.front().AgeMs(now_us);
    return age_ms >= kMaxWaitMs ? 0 : kMaxWaitMs - age_ms;
}
// Called under the session lock. Never wait for new text or exceed the
// provider's 512-byte sentence limit. Only already queued fragments merge.
inline std::string PopTextBatch(std::deque<QueuedText>& queue, size_t& queued_bytes, bool merge) {
    if (queue.empty())
        return {};
    std::string result = std::move(queue.front().text);
    queue.pop_front();
    queued_bytes -= result.size();
    while (merge && !queue.empty() && result.size() + 1 + queue.front().text.size() <= 240) {
        queued_bytes -= queue.front().text.size();
        result += ' ';
        result += queue.front().text;
        queue.pop_front();
    }
    return result;
}
}  // namespace voice_tts
