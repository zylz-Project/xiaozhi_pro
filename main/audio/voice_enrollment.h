#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// Match only explicit enrollment commands, allowing common ASR punctuation.
inline bool IsVoiceEnrollmentRequest(std::string_view text) {
    constexpr std::string_view whitespace[] = {" ", "\t", "\r", "\n", "　"};
    constexpr std::string_view suffixes[] = {" ", "\t", "\r", "\n", "　", "。", "！",
                                             "!", "？", "?",  "，", ",",  "."};
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto prefix : whitespace) {
            if (text.substr(0, prefix.size()) == prefix) {
                text.remove_prefix(prefix.size());
                changed = true;
                break;
            }
        }
    }
    changed = true;
    while (changed) {
        changed = false;
        for (auto suffix : suffixes) {
            if (text.size() >= suffix.size() &&
                text.substr(text.size() - suffix.size()) == suffix) {
                text.remove_suffix(suffix.size());
                changed = true;
                break;
            }
        }
    }
    return text == "复刻音色" || text == "我想复刻音色" || text == "帮我复刻音色";
}

// Optional board-owned enrollment. Capture runs on the audio task and must be bounded.
class VoiceEnrollment {
public:
    virtual ~VoiceEnrollment() = default;
    virtual bool Start(std::function<void(bool)> capture_switch,
                       std::function<void(bool)> complete) = 0;
    virtual bool StartNamed(const std::string&, std::function<void(bool)>,
                            std::function<void(bool)>) {
        return false;
    }
    virtual bool Capture(const int16_t* samples, size_t count, bool speech) = 0;
    virtual void Cancel() = 0;
    virtual const char* Status() const { return "unavailable"; }
    virtual const char* Failure() const { return ""; }
    virtual bool SelectionPending() const { return false; }
    virtual bool StartSelection(const std::string&, std::function<void(bool)>) { return false; }
};
