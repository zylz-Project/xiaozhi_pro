#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() { event_group_handle_ = xEventGroupCreate(); }

WebsocketProtocol::~WebsocketProtocol() {
    channel_ready_.store(false);
    // The receive task must stop before its handshake event group is deleted.
    websocket_.reset();
    if (event_group_handle_)
        vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (!channel_ready_.load() || websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!channel_ready_.load() || websocket_ == nullptr || !websocket_->IsConnected()) {
            return false;
        }
        generation = channel_generation_.load();
        if (websocket_->Send(text)) {
            return true;
        }
    }
    ESP_LOGE(TAG, "Failed to send text (%u bytes)", static_cast<unsigned>(text.size()));
    if (generation == channel_generation_.load())
        SetError(Lang::Strings::SERVER_ERROR);
    return false;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return channel_ready_.load() && websocket_ != nullptr && websocket_->IsConnected() &&
           !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    std::unique_ptr<WebSocket> closed;
    bool was_ready;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        channel_generation_.fetch_add(1);
        was_ready = channel_ready_.exchange(false);
        closed = std::move(websocket_);
        if (closed)
            closing_ = true;
        if (event_group_handle_)
            xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT);
    }
    if (closed) {
        // Teardown joins the receive task and may wait for the modem. Keep it
        // outside the lock and keep the connection ID reserved until it ends.
        closed.reset();
        std::lock_guard<std::mutex> lock(channel_mutex_);
        closing_ = false;
    }
    if (was_ready && on_audio_channel_closed_)
        on_audio_channel_closed_();
}

bool WebsocketProtocol::OpenAudioChannel() {
    auto& board = Board::GetInstance();
    if (!board.IsNetworkAvailable() || board.IsAudioMaintenanceActive()) {
        ESP_LOGI(TAG, "WebSocket connection deferred: network unavailable or maintenance active");
        if (!board.IsAudioMaintenanceActive())
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    Settings settings("websocket", false);
    if (!event_group_handle_)
        return false;
    std::unique_lock<std::mutex> opening(open_mutex_, std::try_to_lock);
    if (!opening.owns_lock())
        return false;
    if (IsAudioChannelOpened())
        return true;
    CloseAudioChannel(false);
    constexpr EventBits_t kHandshakeEvents =
        WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT | WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (closing_ || !board.IsNetworkAvailable() || board.IsAudioMaintenanceActive())
            return false;
        generation = channel_generation_.load();
        xEventGroupClearBits(event_group_handle_, kHandshakeEvents);
    }
    SetSessionId({});
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    const int version = settings.GetInt("version");
    version_ = version == 2 || version == 3 ? version : 1;

    error_occurred_ = false;

    auto network = board.GetNetwork();
    auto socket = network ? network->CreateWebSocket(1) : nullptr;
    if (!socket) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        socket->SetHeader("Authorization", token.c_str());
    }
    socket->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    socket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    socket->SetHeader("Client-Id", board.GetUuid().c_str());

    socket->OnData([this, generation, hello_received = false](const char* data, size_t len,
                                                              bool binary) mutable {
        if (generation != channel_generation_.load())
            return;
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (binary) {
            if (hello_received)
                HandleBinaryAudio(data, len);
            return;
        }

        auto root = cJSON_ParseWithLength(data, len);
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Missing message type (%u bytes)", static_cast<unsigned>(len));
        } else if (strcmp(type->valuestring, "hello") == 0) {
            if (!hello_received && ParseServerHello(root)) {
                hello_received = true;
                xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
            }
        } else if (hello_received && on_incoming_json_) {
            // A server can send an MCP message immediately after hello in the
            // same read, before the connection worker finishes waking up.
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
    });

    socket->OnDisconnected([this, generation]() {
        if (generation != channel_generation_.load())
            return;
        ESP_LOGI(TAG, "Websocket disconnected");
        xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT);
        if (channel_ready_.exchange(false) && on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server (protocol version %d)", version_);
    if (auto connected = socket->Connect(url.c_str()); !connected) {
        ESP_LOGE(TAG, "Failed to connect to websocket server: %s",
                 connected.error().ToString().c_str());
        if (generation == channel_generation_.load())
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    if (generation != channel_generation_.load())
        return false;
    if (!socket->IsConnected() || !socket->Send(GetHelloMessage())) {
        if (generation == channel_generation_.load())
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Wait for server hello
    const auto bits = xEventGroupWaitBits(event_group_handle_, kHandshakeEvents, pdTRUE, pdFALSE,
                                          pdMS_TO_TICKS(10000));
    if (generation != channel_generation_.load())
        return false;
    if ((bits & WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT) || !socket->IsConnected()) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (generation != channel_generation_.load() || !board.IsNetworkAvailable())
            return false;
        channel_ready_.store(true);
        if (!socket->IsConnected()) {
            channel_ready_.store(false);
            return false;
        }
        websocket_ = std::move(socket);
    }
    if (generation != channel_generation_.load() || !IsAudioChannelOpened())
        return false;
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return generation == channel_generation_.load() && IsAudioChannelOpened();
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::HandleBinaryAudio(const char* data, size_t len) {
    if (!on_incoming_audio_ || !data || len == 0)
        return;

    size_t header_size = 0;
    size_t payload_size = len;
    uint32_t timestamp = 0;
    if (version_ == 2) {
        BinaryProtocol2 header;
        header_size = sizeof(header);
        if (len < header_size)
            return;
        std::memcpy(&header, data, header_size);
        timestamp = ntohl(header.timestamp);
        payload_size = ntohl(header.payload_size);
    } else if (version_ == 3) {
        BinaryProtocol3 header;
        header_size = sizeof(header);
        if (len < header_size)
            return;
        std::memcpy(&header, data, header_size);
        payload_size = ntohs(header.payload_size);
    }
    if (payload_size == 0 || payload_size > len - header_size)
        return;

    const auto* payload = reinterpret_cast<const uint8_t*>(data + header_size);
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = server_sample_rate_;
    packet->frame_duration = server_frame_duration_;
    packet->timestamp = timestamp;
    packet->payload.assign(payload, payload + payload_size);
    on_incoming_audio_(std::move(packet));
}

bool WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport)) {
        ESP_LOGE(TAG, "Missing or non-string transport in server hello");
        return false;
    }
    if (strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return false;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        SetSessionId(session_id->valuestring);
        ESP_LOGI(TAG, "Session established");
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
        ESP_LOGI(TAG, "Server audio: %d Hz, %d ms frames", server_sample_rate_,
                 server_frame_duration_);
    }

    return true;
}
