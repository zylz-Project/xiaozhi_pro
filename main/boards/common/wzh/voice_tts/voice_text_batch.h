#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace voice_tts {
// Hold only a short first fragment, with a hard deadline. Completed turns and
// multiple fragments are sent immediately, including enrollment prompts.
inline bool WaitForShortCellularBatch(const std::deque<std::string>& queue, bool finished,
                                      int64_t waited_ms) {
    return !finished && queue.size() == 1 && queue.front().size() <= 36 && waited_ms < 1800;
}
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
