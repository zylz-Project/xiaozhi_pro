#pragma once

#include <memory>
#include "esp_http_client.h"

namespace voice_tts {
struct VoiceHttpDelete {
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    void* transport = nullptr;
    void operator()(esp_http_client* client) const;
#else
    void operator()(esp_http_client* client) const { esp_http_client_cleanup(client); }
#endif
};
using VoiceHttpClient = std::unique_ptr<esp_http_client, VoiceHttpDelete>;

#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
VoiceHttpClient CreateVoiceHttpClient(const esp_http_client_config_t& config);
bool VoiceHttpNetworkAvailable();
#else
inline VoiceHttpClient CreateVoiceHttpClient(const esp_http_client_config_t& config) {
    return VoiceHttpClient(esp_http_client_init(&config));
}
#endif
}  // namespace voice_tts
