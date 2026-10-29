#ifndef WZH_INTERACTION_SOUND_H_
#define WZH_INTERACTION_SOUND_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string_view>

#include "wzh_interaction.h"

namespace wzh {

// Select existing external-Flash animal files only. Names describe the content;
// no additional sound pack or fixed directory indexes are required.
class InteractionSoundSelector {
public:
    template <typename Files>
    int Select(const Files& files, PetInteraction interaction, uint32_t random) const {
        if (interaction == PetInteraction::kNone) {
            return -1;
        }
        // Current pack: exactly two content-appropriate choices per sensor.
        // Keep the semantic fallback below for legacy and user-supplied packs.
        const unsigned event = static_cast<unsigned>(interaction) - 1;
        static constexpr const char* panda[][2] = {
            {"摸头_欢快的“嗯”声_萌丫头", "熊猫撒娇声音"},
            {"挠痒痒笑声_萌丫头", "笑声_萌丫头"},
            {"摸头_回头找人_萌丫头", "类似熊猫声音"},
            {"哼唧_萌丫头", "Cute_baby_panda_voca"},
            {"挠痒痒笑声_萌丫头", "摸头_欢快的“嗯”声_萌丫头"},
            {"熊猫撒娇声音", "哼唧_萌丫头"},
            {"摸头_回头找人_萌丫头", "Cute_baby_panda_voca"},
            {"唤醒_类似伸懒腰状态_萌丫头", "哼唧_萌丫头"},
            {"喷嚏_萌丫头", "熊猫_喷嚏_2"},
            {"熊猫_喷嚏_2", "类似熊猫叫声- Cat"},
            {"唤醒_类似伸懒腰状态_萌丫头", "摸头_欢快的“嗯”声_萌丫头"}};
        static constexpr const char* dinosaur[][2] = {{"叫声音效合集 (79)", "叫声音效合集 (13)"},
                                                      {"叫声音效合集 (8)", "叫声音效合集 (5)"},
                                                      {"叫声音效合集 (46)", "叫声音效合集 (41)"},
                                                      {"叫声音效合集 (124)", "叫声音效合集 (152)"},
                                                      {"脚步音效合集 (19)", "脚步音效合集 (28)"},
                                                      {"叫声音效合集 (13)", "叫声音效合集 (79)"},
                                                      {"叫声音效合集 (8)", "叫声音效合集 (46)"},
                                                      {"叫声音效合集 (14)", "叫声音效合集 (15)"},
                                                      {"叫声音效合集 (5)", "叫声音效合集 (147)"},
                                                      {"叫声音效合集 (124)", "叫声音效合集 (152)"},
                                                      {"脚步音效合集 (19)", "叫声音效合集 (41)"}};
        if (event < std::size(panda)) {
            int pair[2] = {-1, -1};
            for (size_t i = 0; i < files.size(); ++i) {
                if (!Eligible(files[i]))
                    continue;
                for (unsigned j = 0; j < 2; ++j) {
                    if (NameIs(files[i].name, panda[event][j]) ||
                        NameIs(files[i].name, dinosaur[event][j]))
                        pair[j] = static_cast<int>(i);
                }
            }
            if (pair[0] >= 0 || pair[1] >= 0) {
                unsigned choice = random & 1U;
                if (pair[choice] < 0)
                    choice ^= 1U;
                const unsigned other = choice ^ 1U;
                if (pair[other] >= 0 &&
                    std::strcmp(files[pair[choice]].name, last_name_.data()) == 0)
                    choice = other;
                if (pair[other] >= 0 ||
                    std::strcmp(files[pair[choice]].name, last_name_.data()) != 0)
                    return pair[choice];
            }
        }
        bool has_alternative = false;
        for (const auto& file : files) {
            if (Eligible(file) && std::strcmp(file.name, last_name_.data()) != 0) {
                has_alternative = true;
                break;
            }
        }
        int best_score = std::numeric_limits<int>::min();
        int count = 0;
        for (const auto& file : files) {
            if (!Eligible(file) ||
                (has_alternative && std::strcmp(file.name, last_name_.data()) == 0)) {
                continue;
            }
            const int score = Score(file.name, interaction) +
                              (file.duration_ms > 0 && file.duration_ms <= 2500 ? 4 : 0);
            if (score > best_score) {
                best_score = score;
                count = 0;
            }
            if (score == best_score) {
                ++count;
            }
        }
        if (count == 0) {
            return -1;
        }
        unsigned choice = random % static_cast<unsigned>(count);
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];
            if (!Eligible(file) ||
                (has_alternative && std::strcmp(file.name, last_name_.data()) == 0)) {
                continue;
            }
            const int score = Score(file.name, interaction) +
                              (file.duration_ms > 0 && file.duration_ms <= 2500 ? 4 : 0);
            if (score == best_score && choice-- == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Only remember accepted requests, so a busy decoder does not consume variety.
    void Remember(const char* name) {
        std::strncpy(last_name_.data(), name, last_name_.size() - 1);
        last_name_.back() = '\0';
    }

private:
    static bool NameIs(std::string_view name, std::string_view expected) {
        return name == expected ||
               (name.size() == expected.size() + 5 && name.substr(0, expected.size()) == expected &&
                name.substr(expected.size()) == ".opus");
    }
    template <typename File>
    static bool Eligible(const File& file) {
        return file.name[0] != '\0' && file.size > 0 && std::strcmp(file.category, "ambient") != 0;
    }

    static char Fold(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

    static bool Contains(std::string_view name, std::string_view token) {
        for (size_t i = 0; i + token.size() <= name.size(); ++i) {
            size_t j = 0;
            while (j < token.size() && Fold(name[i + j]) == Fold(token[j])) {
                ++j;
            }
            if (j == token.size()) {
                return true;
            }
        }
        return false;
    }

    static bool Any(std::string_view name, std::initializer_list<std::string_view> tokens) {
        for (auto token : tokens) {
            if (Contains(name, token)) {
                return true;
            }
        }
        return false;
    }

    static int Score(std::string_view name, PetInteraction interaction) {
        const bool gentle =
            Any(name, {"撒娇", "亲近", "安慰", "呼噜", "nuzzle", "comfort", "affection", "purr"});
        const bool happy =
            Any(name, {"开心", "雀跃", "嘤嘤", "宝宝", "happy", "joy", "baby", "play"});
        const bool curious =
            Any(name, {"发现", "探索", "好奇", "叫声", "curious", "discover", "call", "chirp"});
        const bool startled = Any(name, {"警觉", "受惊", "惊讶", "startle", "alert"});
        int score = 0;
        switch (interaction) {
            case PetInteraction::kTouch1:
            case PetInteraction::kHeld:
                score = gentle ? 10 : happy ? 4 : 0;
                break;
            case PetInteraction::kTouch2:
            case PetInteraction::kPlayful:
                score = happy ? 10 : gentle ? 4 : 0;
                break;
            case PetInteraction::kTouch3:
            case PetInteraction::kReleased:
            case PetInteraction::kPutDown:
                score = curious ? 10 : gentle ? 4 : 0;
                break;
            case PetInteraction::kTouch4:
                score = gentle || happy ? 10 : 0;
                break;
            case PetInteraction::kPickedUp:
                score = gentle || curious ? 10 : 0;
                break;
            case PetInteraction::kShaken:
            case PetInteraction::kFlipped:
                score = startled ? 10 : curious ? 6 : 0;
                break;
            default:
                break;
        }
        // Eating and sleeping recordings remain available for their explicit actions.
        if (Any(name, {"吃", "咀嚼", "入睡", "打鼾", "sleep", "snore", "eat", "chew"})) {
            score -= 16;
        }
        return score;
    }

    std::array<char, 64> last_name_{};
};

}  // namespace wzh

#endif  // WZH_INTERACTION_SOUND_H_
