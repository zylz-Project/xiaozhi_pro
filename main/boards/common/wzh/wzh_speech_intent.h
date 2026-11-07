#ifndef WZH_SPEECH_INTENT_H_
#define WZH_SPEECH_INTENT_H_

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace wzh {
enum class SpeechIntent : uint8_t {
    kNeutral,
    kGreeting,
    kJoy,
    kComfort,
    kQuestion,
    kStory,
    kSleep
};

// A bounded local style hint from the assistant's current sentence. This never
// executes commands, changes listening, or interprets quoted text as controls.
inline SpeechIntent SpeechIntentFromText(std::string_view text) {
    text = text.substr(0, 1536);
    auto has = [text](std::initializer_list<std::string_view> words) {
        for (auto word : words)
            if (text.find(word) != std::string_view::npos)
                return true;
        return false;
    };
    if (has({"难过", "伤心", "别担心", "不要担心", "没关系", "陪着你", "慢慢来", "我理解",
             "别怕", "抱抱", "没事"}))
        return SpeechIntent::kComfort;
    if (has({"晚安", "睡个好觉", "好梦", "闭上眼", "休息一下", "睡觉"}))
        return SpeechIntent::kSleep;
    if (has({"从前", "有一天", "小故事", "很久以前", "后来", "森林里", "竹林里", "讲个故事",
             "讲故事"}))
        return SpeechIntent::kStory;
    if (has({"太棒了", "真棒", "恭喜", "好开心", "开心", "哈哈", "好耶", "成功了", "喜欢",
             "爱你", "太好了"}))
        return SpeechIntent::kJoy;
    if (has({"你好", "我在呢", "见到你", "欢迎", "一起玩", "嗨", "早上好", "晚上好", "hello",
             "hi"}))
        return SpeechIntent::kGreeting;
    if (has({"？", "?", "为什么", "怎么", "什么", "哪", "谁", "多少", "是不是", "能不能",
             "可以吗", "吗", "呢", "猜一猜", "你知道", "想一想"}))
        return SpeechIntent::kQuestion;
    return SpeechIntent::kNeutral;
}
}  // namespace wzh
#endif
