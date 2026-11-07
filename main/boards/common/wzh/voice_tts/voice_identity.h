#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace voice_tts {
// A 48-bit factory MAC fits in ten base-36 digits (36^10 > 2^48).
// The fixed width makes prefixes unique rather than prefixes of one another.
inline std::string VoicePrefixFromMac(const uint8_t* mac, size_t size) {
    if (!mac || size != 6)
        return {};
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value = (value << 8) | mac[i];
    constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string prefix(10, '0');
    for (size_t i = prefix.size(); i != 0; --i) {
        prefix[i - 1] = alphabet[value % 36];
        value /= 36;
    }
    return prefix;
}

struct BuiltinVoice {
    const char* name;
    const char* id;
    const char* description;
};
// Qwen-Audio 3.1 TTS Flash, Beijing region, per Ali's model-specific list.
inline constexpr BuiltinVoice kBuiltinVoices[] = {
    {"龙安欢", "longanhuan_v3.1", "活泼亲切"},      {"龙安灵心", "longanlingxin_v3.1", "温柔自然"},
    {"龙安风悦", "longanfengyue_v3.1", "自然清晰"}, {"许南川", "xunanchuan_v3.1", "温暖男声"},
    {"于小云", "yuxiaoyun_v3.1", "元气亲切"},       {"温怀清", "wenhuaiqing_v3.1", "柔和故事声"},
};
inline constexpr size_t kBuiltinVoiceCount = sizeof(kBuiltinVoices) / sizeof(kBuiltinVoices[0]);
}  // namespace voice_tts
