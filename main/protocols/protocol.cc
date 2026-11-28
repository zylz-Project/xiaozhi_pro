#include "protocol.h"
#include "assets.h"

#include <esp_log.h>
#include <utility>

#define TAG "Protocol"

void Protocol::AddTextFontCapabilities(cJSON* root) {
    auto capability = Assets::GetInstance().text_font_capability();
    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsObject(features)) {
        cJSON_AddBoolToObject(features, "glyph_push", capability.glyph_push);
    }

    if (!capability.glyph_push) {
        return;
    }
    cJSON* font = cJSON_CreateObject();
    cJSON_AddStringToObject(font, "bundle", capability.bundle.c_str());
    cJSON_AddStringToObject(font, "charset", capability.charset.c_str());
    cJSON_AddNumberToObject(font, "size", capability.size);
    cJSON_AddNumberToObject(font, "bpp", capability.bpp);
    cJSON_AddItemToObject(root, "text_font", font);
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void Protocol::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

std::string Protocol::session_id() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_id_;
}

void Protocol::SetSessionId(std::string id) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_id_ = std::move(id);
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id() + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) { SendListenText(wake_word); }

bool Protocol::SendListenText(const std::string& text) {
    const auto id = session_id();
    auto json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(cJSON_CreateObject(), cJSON_Delete);
    if (!json || !cJSON_AddStringToObject(json.get(), "session_id", id.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "type", "listen") ||
        !cJSON_AddStringToObject(json.get(), "state", "detect") ||
        !cJSON_AddStringToObject(json.get(), "text", text.c_str())) {
        return false;
    }
    auto wire = std::unique_ptr<char, decltype(&cJSON_free)>(cJSON_PrintUnformatted(json.get()),
                                                             cJSON_free);
    return wire && SendText(wire.get());
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id() + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message =
        "{\"session_id\":\"" + session_id() + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message =
        "{\"session_id\":\"" + session_id() + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

size_t Protocol::GetMcpWireOverhead() const {
    static constexpr char kPrefix[] = "{\"session_id\":\"";
    static constexpr char kMiddle[] = "\",\"type\":\"mcp\",\"payload\":";
    std::lock_guard<std::mutex> lock(session_mutex_);
    return sizeof(kPrefix) - 1 + session_id_.size() + sizeof(kMiddle) - 1 + 1;
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_.load());
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
