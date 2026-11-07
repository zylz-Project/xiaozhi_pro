#pragma once

#include <string>

namespace voice_tts {
// The upstream protocol has no documented song-end event. Recognize only
// observed conversational announcements, never arbitrary lyric lines.
inline bool IsPostMusicAnnouncement(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return false;
    // Observed official service format: 《title》已经播放完啦，...
    // Require a leading quoted title followed immediately by an explicit
    // playback-ended phrase to avoid treating ordinary lyrics as control.
    if (text.compare(begin, std::string("《").size(), "《") == 0) {
        const auto close = text.find("》", begin + std::string("《").size());
        if (close != std::string::npos) {
            const auto suffix = close + std::string("》").size();
            if (text.compare(suffix, std::string("已经播放完").size(), "已经播放完") == 0 ||
                text.compare(suffix, std::string("已经放完").size(), "已经放完") == 0 ||
                text.compare(suffix, std::string("播放结束").size(), "播放结束") == 0 ||
                text.compare(suffix, std::string("放完").size(), "放完") == 0)
                return true;
        }
    }
    return text.compare(begin, std::string("好嘞，给你放了").size(), "好嘞，给你放了") == 0 ||
           text.compare(begin, std::string("歌曲播放完毕").size(), "歌曲播放完毕") == 0 ||
           text.compare(begin, std::string("歌曲播放结束").size(), "歌曲播放结束") == 0;
}
inline bool IsMusicStart(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    const auto end = text.find_last_not_of(" \t\r\n");
    return begin != std::string::npos &&
           text.compare(begin, end - begin + 1, "% play_music...") == 0;
}
// XiaoZhi can send a tool-status caption as a TTS sentence, for example
// "% search_music...". Only suppress this narrowly defined status form.
inline bool IsToolStatus(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || text[begin] != '%')
        return false;
    auto pos = begin + 1;
    if (pos == text.size() || (text[pos] != ' ' && text[pos] != '\t'))
        return false;
    pos = text.find_first_not_of(" \t", pos);
    if (pos == std::string::npos)
        return false;
    const auto name_start = pos;
    while (pos < text.size()) {
        const char c = text[pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.') {
            if (text.compare(pos, 3, "...") == 0)
                break;
            ++pos;
        } else {
            break;
        }
    }
    if (pos == name_start || text.compare(pos, 3, "...") != 0)
        return false;
    return text.find_first_not_of(" \t\r\n", pos + 3) == std::string::npos;
}
}  // namespace voice_tts
