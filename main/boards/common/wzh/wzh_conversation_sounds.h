#ifndef WZH_CONVERSATION_SOUNDS_H_
#define WZH_CONVERSATION_SOUNDS_H_

#include <string_view>

namespace wzh {

// Immutable, firmware-bundled descending cue; uses the normal AudioService path.
std::string_view ConversationExitSound();

}  // namespace wzh

#endif  // WZH_CONVERSATION_SOUNDS_H_
