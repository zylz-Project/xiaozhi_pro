#ifndef WZH_BODY_COMMANDS_H_
#define WZH_BODY_COMMANDS_H_

#include <string>
#include <string_view>
#include "wzh_pet_behavior.h"

namespace wzh {
constexpr unsigned kBodyCrawl = 1, kBodyTail = 2, kBodyDinosaur = 4;
struct BodyPhrase {
    unsigned products;
    const char* action;
    const char* text;
    const char* pinyin;
};
inline constexpr BodyPhrase kBodyPhrases[] = {
#define WZH_BODY_PHRASE(products, action, text, pinyin) {products, action, text, pinyin},
#include "wzh_body_phrases.inc"
#undef WZH_BODY_PHRASE
};

// Exact MCP values and local recognizer tokens share this capability check.
// Chinese aliases are accepted only as a complete parameter, never as an
// arbitrary substring in untrusted tool input.
inline std::string_view CanonicalBodyCommand(std::string_view value, unsigned product) {
    for (const auto& phrase : kBodyPhrases) {
        if ((phrase.products & product) && (value == phrase.action || value == phrase.text))
            return phrase.action;
    }
    return {};
}

// Used to reserve explicit body intent for MCP while processing cloud STT;
// it must not execute a second gesture before the cloud calls the tool.
inline std::string_view MatchBodyCommand(std::string_view text, unsigned product) {
    std::string_view best;
    size_t length = 0;
    for (const auto& phrase : kBodyPhrases) {
        const std::string_view word(phrase.text);
        if ((phrase.products & product) && word.size() > length && PositiveMention(text, word)) {
            best = phrase.action;
            length = word.size();
        }
    }
    return best;
}

inline std::string BodyCommandHelp(unsigned product) {
    std::string text =
        "按用户明确要求控制关节。一次执行一段有限动作，结束柔和归中。方向以宠物自身左右为准。";
    text += product == kBodyDinosaur
                ? "头可左右转，颈可俯仰侧倾，尾可上下左右；点头由颈俯仰实现。"
                : "头只有左右转动，范围30到150度（正前90度左右各60度），不支持抬头、低头或点头。";
    std::string_view previous;
    for (const auto& phrase : kBodyPhrases) {
        if (!(phrase.products & product) || previous == phrase.action)
            continue;
        previous = phrase.action;
        text += " ";
        text += phrase.action;
        text += "=";
        text += phrase.text;
        text += "；";
    }
    text += "用户说停下、别动或安静时使用pet_control；不要重复执行已调用的动作。";
    return text;
}
}  // namespace wzh
#endif
