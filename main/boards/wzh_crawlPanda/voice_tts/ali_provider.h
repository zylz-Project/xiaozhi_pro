#pragma once
#include "voice_tts_provider.h"
namespace voice_tts {
class AliProvider final : public Provider {
public:
    Result Synthesize(const Request& request, Response& response) override;
};
}  // namespace voice_tts
