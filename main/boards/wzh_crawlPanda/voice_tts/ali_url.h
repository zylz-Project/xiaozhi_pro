#pragma once
#include <string>

namespace voice_tts {
// DashScope Beijing result URLs point to this OSS bucket. The signed URL is a
// credential: do not log it and do not forward the API key when downloading.
inline bool AliAudioHttpsUrl(const std::string& input, std::string& output) {
    output.clear();
    if (input.size() > 2048)
        return false;
    size_t start = 0;
    if (input.compare(0, 7, "http://") == 0)
        start = 7;
    else if (input.compare(0, 8, "https://") == 0)
        start = 8;
    else
        return false;
    const auto slash = input.find('/', start);
    if (slash == std::string::npos)
        return false;
    if (input.substr(start, slash - start) != "dashscope-result-bj.oss-cn-beijing.aliyuncs.com")
        return false;
    for (unsigned char c : input) {
        if (c <= 0x20 || c >= 0x7f || c == '#')
            return false;
    }
    output = "https://" + input.substr(start);
    return true;
}
}  // namespace voice_tts
