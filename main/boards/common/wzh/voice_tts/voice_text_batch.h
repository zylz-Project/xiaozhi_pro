#pragma once
#include <deque>
#include <string>
#include <utility>

namespace voice_tts {
// Called under the session lock. Never wait for new text or exceed the
// provider's 512-byte sentence limit. Only already queued fragments merge.
inline std::string PopTextBatch(std::deque<std::string>& queue, size_t& queued_bytes, bool merge) {
    if (queue.empty())
        return {};
    std::string result = std::move(queue.front());
    queue.pop_front();
    queued_bytes -= result.size();
    while (merge && !queue.empty() && result.size() + 1 + queue.front().size() <= 240) {
        queued_bytes -= queue.front().size();
        result += ' ';
        result += queue.front();
        queue.pop_front();
    }
    return result;
}
}  // namespace voice_tts
