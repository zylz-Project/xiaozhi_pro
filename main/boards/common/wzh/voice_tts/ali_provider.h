#pragma once
#include <esp_http_client.h>
#include "voice_tts_provider.h"
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#include "voice_http.h"
#endif
namespace voice_tts {
class AliProvider final : public Provider {
public:
    AliProvider() = default;
    ~AliProvider() override;
    AliProvider(const AliProvider&) = delete;
    AliProvider& operator=(const AliProvider&) = delete;
    Result Synthesize(const Request& request, Response& response) override;
    void Close();

private:
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    VoiceHttpClient client_owner_;
#endif
    esp_http_client_handle_t client_ = nullptr;
    bool reuse_connection_ = false;
};
}  // namespace voice_tts
