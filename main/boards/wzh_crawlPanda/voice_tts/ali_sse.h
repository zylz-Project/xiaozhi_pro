#pragma once

#include <functional>
#include <string>
#include <utility>

namespace voice_tts {
// Incrementally frame SSE records; TCP reads may split any character.
class AliSse {
public:
    explicit AliSse(std::function<bool(const std::string&)> on_data)
        : on_data_(std::move(on_data)) {}
    bool Feed(const char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            const char c = data[i];
            if (c == '\n') {
                if (!line_.empty() && line_.back() == '\r')
                    line_.pop_back();
                if (line_.empty()) {
                    if (!event_.empty() && !on_data_(event_))
                        return false;
                    event_.clear();
                } else if (line_.compare(0, 5, "data:") == 0) {
                    size_t offset = line_.size() > 5 && line_[5] == ' ' ? 6 : 5;
                    if (!event_.empty())
                        event_ += '\n';
                    event_ += line_.substr(offset);
                    if (event_.size() > kLimit)
                        return false;
                }
                line_.clear();
            } else {
                line_ += c;
                if (line_.size() > kLimit)
                    return false;
            }
        }
        return true;
    }
    bool Complete() const { return line_.empty() && event_.empty(); }

private:
    static constexpr size_t kLimit = 96 * 1024;
    std::function<bool(const std::string&)> on_data_;
    std::string line_;
    std::string event_;
};
}  // namespace voice_tts
