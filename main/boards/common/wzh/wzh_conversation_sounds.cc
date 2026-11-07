#include "wzh_conversation_sounds.h"

namespace wzh {

std::string_view ConversationExitSound() {
    extern const char start[] asm("_binary_conversation_exit_ogg_start");
    extern const char end[] asm("_binary_conversation_exit_ogg_end");
    return {start, static_cast<size_t>(end - start)};
}

}  // namespace wzh
