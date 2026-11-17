#ifndef WZH_PROMPT_AUDIO_GATE_H_
#define WZH_PROMPT_AUDIO_GATE_H_

#include <mutex>

namespace wzh {
// Only prompt producer tasks use this lock; audio and application tasks never
// wait on a long prompt. Shutdown keeps its existing higher-priority path.
inline std::mutex& PromptAudioGate() {
    static std::mutex gate;
    return gate;
}
}  // namespace wzh
#endif  // WZH_PROMPT_AUDIO_GATE_H_
