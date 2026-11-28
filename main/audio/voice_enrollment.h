#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

// Conservative phrase recognition for user requests. This is a local rule
// matcher, not a general language-understanding model: require both a voice
// object and an action, and leave negated/educational statements to the LLM.
inline bool VoiceTextHasAny(std::string_view text,
                            std::initializer_list<std::string_view> terms) {
    for (const auto term : terms) {
        if (text.find(term) != std::string_view::npos)
            return true;
    }
    return false;
}

inline bool IsVoiceChangeRequestContext(std::string_view text) {
    if (text.empty() || text.size() > 384 ||
        !VoiceTextHasAny(text, {"音色", "声音", "声线", "嗓音", "嗓子"}))
        return false;
    return !VoiceTextHasAny(text, {"不要", "不用", "不想", "不需要", "不必", "别换", "别切换", "别复刻", "别复克",
                                   "别克隆", "别模仿", "别用", "不换", "不复刻", "不复克", "不克隆", "取消",
                                   "完成了吗", "成功了吗", "好了吗", "进度", "状态", "什么是",
                                   "是什么意思", "怎么", "如何", "为什么", "介绍",
                                   "解释", "听说", "听别人", "有人说", "别人说", "引用", "\"", "“", "”"});
}

inline bool IsVoiceEnrollmentRequest(std::string_view text) {
    return IsVoiceChangeRequestContext(text) &&
           !VoiceTextHasAny(text, {"复刻过", "复克过", "克隆过", "已经复刻", "已经复克", "已经克隆"}) &&
           VoiceTextHasAny(text, {"复刻", "复克", "克隆", "复制", "模仿", "仿制", "复原", "复现"});
}

inline bool IsVoiceSelectionRequest(std::string_view text) {
    if (!IsVoiceChangeRequestContext(text) || IsVoiceEnrollmentRequest(text))
        return false;
    if (VoiceTextHasAny(text, {"切换", "更换", "换", "切成", "改成", "改为", "恢复"}))
        return true;
    const auto use = text.find("用");
    if (use == std::string_view::npos)
        return false;
    for (const auto voice : {"音色", "声音", "声线", "嗓音", "嗓子"}) {
        const auto position = text.find(voice);
        if (position != std::string_view::npos && use < position)
            return true;
    }
    return false;
}

inline bool IsNativeVoiceSelection(std::string_view text, bool allow_name) {
    if (text.size() > 384 || text.find("原声") == std::string_view::npos)
        return false;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return false;
    text.remove_prefix(first);
    while (text.substr(0, sizeof("　") - 1) == "　")
        text.remove_prefix(sizeof("　") - 1);
    const auto last = text.find_last_not_of(" \t\r\n.!?");
    if (last == std::string_view::npos)
        return false;
    text = text.substr(0, last + 1);
    for (const std::string_view mark : {"。", "！", "？"}) {
        if (text.size() >= mark.size() && text.substr(text.size() - mark.size()) == mark) {
            text.remove_suffix(mark.size());
            break;
        }
    }
    if (text.substr(0, sizeof("请") - 1) == "请")
        text.remove_prefix(sizeof("请") - 1);
    if (text.substr(0, sizeof("帮我") - 1) == "帮我")
        text.remove_prefix(sizeof("帮我") - 1);
    return (allow_name && text == "原声") || text == "切回原声" || text == "换回原声" ||
           text == "恢复原声" || text == "切换到原声" || text == "切换音色为原声" ||
           text == "用原声" || text == "使用原声" || text == "用原声说话";
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
    virtual bool IsConfigured() const { return true; }
    virtual bool SelectionPending() const { return false; }
    virtual bool StartSelection(const std::string&, std::function<void(bool)>) { return false; }
};
