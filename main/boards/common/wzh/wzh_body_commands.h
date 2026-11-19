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
    // This string is sent as an MCP tool description. Cellular MQTT allows at
    // most 1024 bytes per publish, so keep the canonical action list compact;
    // local speech aliases remain in kBodyPhrases and are not needed online.
    std::string text =
        "按明确要求控制关节，一次一段，结束回中，左右以宠物自身为准。";
    text += product == kBodyDinosaur
                ? "头可左右转，颈可俯仰侧倾，尾可上下左右；点头由颈俯仰。"
                : "头只能左右转30到150度，不能抬头、低头或点头。";
    text += "action:";
    std::string_view previous;
    bool first = true;
    for (const auto& phrase : kBodyPhrases) {
        if (!(phrase.products & product) || previous == phrase.action)
            continue;
        previous = phrase.action;
        if (!first)
            text += ",";
        first = false;
        text += phrase.action;
    }
    text += "。停止或安静用pet_control，勿重复调用。";
    return text;
}
}  // namespace wzh
#endif
