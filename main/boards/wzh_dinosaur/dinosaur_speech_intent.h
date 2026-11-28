#ifndef WZH_DINOSAUR_SPEECH_INTENT_H_
#define WZH_DINOSAUR_SPEECH_INTENT_H_

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "wzh_pet_behavior.h"
#include "wzh_speech_intent.h"

namespace wzh_dino {

// Convert dialogue text into a style for speaker-driven motion.
inline wzh::SpeechIntent AssistantSpeechIntent(std::string_view text) {
    using wzh::SpeechIntent;
    text = text.substr(0, 1536);
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return SpeechIntent::kNeutral;
    }
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    auto starts_with = [text](std::initializer_list<std::string_view> phrases) {
        return std::any_of(phrases.begin(), phrases.end(), [text](std::string_view phrase) {
            return text.substr(0, phrase.size()) == phrase;
        });
    };
    auto ends_with = [text](std::string_view suffix) {
        return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
    };

    const bool question = ends_with("？") || ends_with("?") || ends_with("吗");
    // In an actual narrative, feelings may belong to the characters.
    if (!question && starts_with({"从前", "很久以前", "有一天，", "后来，", "故事里", "这个故事"})) {
        return SpeechIntent::kStory;
    }
    if (wzh::ContainsAny(text, {"别难过", "别担心", "不用担心", "没关系", "陪着你", "慢慢来",
                                "我理解你的感受", "别怕", "没有开心", "没有高兴", "没开心",
                                "没高兴", "没有那么开心", "没那么开心"})) {
        return SpeechIntent::kComfort;
    }
    const auto reaction = wzh::ReactionFromText(text);
    if (reaction == wzh::PetReaction::kComfort) {
        return SpeechIntent::kComfort;
    }
    if (question || text == "你呢" || text == "那你呢") {
        return SpeechIntent::kQuestion;
    }
    if (starts_with({"你好", "您好", "嗨，", "嗨！", "早上好", "晚上好", "欢迎", "Hello", "hello"}) ||
        text == "嗨" || text == "我在呢") {
        return SpeechIntent::kGreeting;
    }
    if (wzh::PositiveMention(text, "晚安") || wzh::PositiveMention(text, "做个好梦") ||
        wzh::PositiveMention(text, "睡个好觉") || wzh::PositiveMention(text, "早点休息") ||
        reaction == wzh::PetReaction::kSleepy) {
        return SpeechIntent::kSleep;
    }
    if (wzh::PositiveMention(text, "讲个故事") || wzh::PositiveMention(text, "讲一个故事")) {
        return SpeechIntent::kStory;
    }
    if (reaction == wzh::PetReaction::kHappy || reaction == wzh::PetReaction::kAffection ||
        reaction == wzh::PetReaction::kProud || wzh::PositiveMention(text, "好耶")) {
        return SpeechIntent::kJoy;
    }
    return SpeechIntent::kNeutral;
}

inline wzh::SpeechIntent ServerSpeechIntent(std::string_view emotion) {
    using wzh::SpeechIntent;
    auto is = [emotion](std::initializer_list<std::string_view> names) {
        return std::find(names.begin(), names.end(), emotion) != names.end();
    };
    if (is({"happy", "laughing", "winking", "funny", "excited", "delighted", "joyful", "proud",
            "confident", "cool", "silly"})) {
        return SpeechIntent::kJoy;
    }
    if (is({"sad", "crying", "depressed", "lonely", "angry", "frustrated", "fearful", "scared",
            "worried", "annoyed", "loving", "embarrassed", "relaxed", "grateful", "gratitude",
            "love"})) {
        return SpeechIntent::kComfort;
    }
    if (is({"sleepy", "tired", "bored"})) {
        return SpeechIntent::kSleep;
    }
    if (is({"surprised", "shocked", "thinking", "confused", "curious"})) {
        return SpeechIntent::kQuestion;
    }
    return SpeechIntent::kNeutral;
}

}  // namespace wzh_dino

#endif
