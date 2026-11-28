#include "mqtt_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <freertos/task.h>
#include <charconv>
#include <cstring>
#include <new>
#include <utility>
#include "assets/lang_config.h"

#define TAG "MQTT"

namespace {
constexpr int kLinkResumeRetryMs = 250;
}

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback =
            [](void* arg) {
                MqttProtocol* protocol = (MqttProtocol*)arg;
                auto& app = Application::GetInstance();
                auto& board = Board::GetInstance();
                if (app.GetDeviceState() == kDeviceStateIdle && board.IsNetworkAvailable() &&
                    !board.IsAudioMaintenanceActive()) {
                    ESP_LOGI(TAG, "Reconnecting to MQTT server");
                    auto alive = protocol->alive_;  // Capture alive flag
                    app.Schedule([protocol, alive]() {
                        if (!*alive || protocol->reconnect_running_)
                            return;
                        {
                            std::unique_lock<std::mutex> lock(protocol->mqtt_mutex_,
                                                              std::try_to_lock);
                            if (!lock.owns_lock()) {
                                esp_timer_start_once(protocol->reconnect_timer_,
                                                     MQTT_DEFERRED_RETRY_MS * 1000);
                                return;
                            }
                            if (protocol->IsMqttConnectedLocked())
                                return;
                        }
                        auto& board = Board::GetInstance();
                        auto& app = Application::GetInstance();
                        if (app.GetDeviceState() != kDeviceStateIdle ||
                            !board.IsNetworkAvailable() || board.IsAudioMaintenanceActive()) {
                            esp_timer_start_once(protocol->reconnect_timer_,
                                                 MQTT_DEFERRED_RETRY_MS * 1000);
                        } else {
                            protocol->StartReconnectTask();
                        }
                    });
                } else {
                    if ((!board.IsNetworkAvailable() || board.IsAudioMaintenanceActive()) &&
                        !protocol->suspend_queued_.exchange(true)) {
                        auto alive = protocol->alive_;
                        app.Schedule([protocol, alive]() {
                            if (*alive) {
                                protocol->suspend_queued_.store(false);
                                protocol->SuspendUnavailableMqtt();
                            }
                        });
                    }
                    esp_timer_start_once(protocol->reconnect_timer_, MQTT_DEFERRED_RETRY_MS * 1000);
                }
            },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");

    // Mark as dead first to prevent any pending scheduled tasks from executing
    *alive_ = false;

    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    std::shared_ptr<Udp> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
    }
    udp.reset();
    {
        std::lock_guard<std::mutex> lock(mqtt_mutex_);
        mqtt_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (aes_key_id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(aes_key_id_);
            aes_key_id_ = PSA_KEY_ID_NULL;
        }
    }

    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    const bool connected = StartMqttClient(false);
    if (!connected) {
        const auto& board = Board::GetInstance();
        const int delay_ms = !board.IsNetworkAvailable() || board.UsesCellularNetwork()
                                 ? MQTT_DEFERRED_RETRY_MS
                                 : MQTT_RECONNECT_INTERVAL_MS;
        esp_timer_start_once(reconnect_timer_, delay_ms * 1000);
    }
    return connected;
}

void MqttProtocol::OnNetworkLost() {
    link_generation_.fetch_add(1);
    mqtt_connected_.store(false);
}

void MqttProtocol::OnNetworkReady() {
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, kLinkResumeRetryMs * 1000);
}

void MqttProtocol::CloseTransport() {
    alive_->store(false);
    OnNetworkLost();
    esp_timer_stop(reconnect_timer_);
    CloseAudioChannel(false);
    std::lock_guard<std::mutex> lock(mqtt_mutex_);
    mqtt_.reset();
}

bool MqttProtocol::DeferUntilReconnectFinishes(std::function<void()> resume) {
    if (!reconnect_running_)
        return false;
    reconnect_completion_ = std::move(resume);
    return true;
}

void MqttProtocol::StartReconnectTask() {
    if (reconnect_running_)
        return;
    reconnect_running_ = true;
    // DNS, TLS and modem commands can take several seconds.
    auto* owner = new (std::nothrow) std::shared_ptr<MqttProtocol>(weak_from_this().lock());
    if (owner && *owner &&
        xTaskCreate(
            [](void* arg) {
                {
                    std::unique_ptr<std::shared_ptr<MqttProtocol>> request(
                        static_cast<std::shared_ptr<MqttProtocol>*>(arg));
                    auto protocol = std::move(*request);
                    auto& board = Board::GetInstance();
                    auto& app = Application::GetInstance();
                    const auto link_generation = protocol->link_generation_.load();
                    int delay_ms = MQTT_DEFERRED_RETRY_MS;
                    if (board.IsNetworkAvailable() && !board.IsAudioMaintenanceActive() &&
                        app.GetDeviceState() == kDeviceStateIdle) {
                        delay_ms = protocol->StartMqttClient() ? 0 : MQTT_RECONNECT_INTERVAL_MS;
                    }
                    app.Schedule([protocol = std::move(protocol), delay_ms, link_generation]() {
                        auto& board = Board::GetInstance();
                        int next_retry_ms = delay_ms;
                        if (!board.IsNetworkAvailable())
                            next_retry_ms = MQTT_DEFERRED_RETRY_MS;
                        else if (link_generation != protocol->link_generation_.load())
                            next_retry_ms = kLinkResumeRetryMs;
                        if (next_retry_ms > 0) {
                            esp_timer_start_once(protocol->reconnect_timer_, next_retry_ms * 1000);
                        }
                        protocol->reconnect_running_ = false;
                        auto resume = std::exchange(protocol->reconnect_completion_, {});
                        if (resume)
                            resume();
                    });
                }
                vTaskDelete(nullptr);
            },
            "mqtt_reconnect", 4096, owner, 2, nullptr) == pdPASS) {
        return;
    }
    delete owner;
    reconnect_running_ = false;
    ESP_LOGW(TAG, "Cannot start MQTT reconnect task");
    esp_timer_start_once(reconnect_timer_, MQTT_DEFERRED_RETRY_MS * 1000);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    std::unique_lock<std::mutex> lock(mqtt_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    auto& board = Board::GetInstance();
    if (!board.IsNetworkAvailable() || board.IsAudioMaintenanceActive()) {
        ESP_LOGI(TAG, "MQTT connection deferred: network unavailable or maintenance active");
        if (report_error && !board.IsAudioMaintenanceActive())
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    if (mqtt_ != nullptr) {
        if (IsMqttConnectedLocked())
            return true;
        mqtt_connected_.store(false);
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = board.GetNetwork();
    mqtt_transport_cellular_ = board.UsesCellularNetwork();
    mqtt_link_generation_ = link_generation_.load();
    mqtt_ = network ? network->CreateMqtt(0) : nullptr;
    if (!mqtt_) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        if (report_error)
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        mqtt_connected_.store(false);
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        std::shared_ptr<Udp> disconnected_channel;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            disconnected_channel = udp_;
        }
        // The ESP MQTT transport has its own automatic reconnect. Stop that
        // client on the application task if the link is gone or the board is
        // shutting down, so it cannot keep resolving DNS behind our retry gate.
        auto alive = alive_;
        Application::GetInstance().Schedule([this, alive, disconnected_channel]() {
            if (!*alive)
                return;
            std::shared_ptr<Udp> closed_channel;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                if (disconnected_channel && udp_ == disconnected_channel)
                    closed_channel = std::move(udp_);
            }
            if (closed_channel && on_audio_channel_closed_)
                on_audio_channel_closed_();
            auto& board = Board::GetInstance();
            if (!board.IsNetworkAvailable() || board.IsAudioMaintenanceActive()) {
                SuspendUnavailableMqtt();
                esp_timer_stop(reconnect_timer_);
                esp_timer_start_once(reconnect_timer_, MQTT_DEFERRED_RETRY_MS * 1000);
            }
        });
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds",
                 MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this, cellular = mqtt_transport_cellular_,
                        link_generation = mqtt_link_generation_]() {
        auto& board = Board::GetInstance();
        if (link_generation != link_generation_.load() || !board.IsNetworkAvailable() ||
            board.UsesCellularNetwork() != cellular) {
            return;
        }
        mqtt_connected_.store(true);
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse JSON message (%u bytes)",
                     static_cast<unsigned>(payload.size()));
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message");
            if (cJSON_IsString(id) && session_id() == id->valuestring) {
                std::weak_ptr<Udp> channel;
                {
                    std::lock_guard<std::mutex> lock(channel_mutex_);
                    channel = udp_;
                }
                auto alive = alive_;  // Capture alive flag
                Application::GetInstance().Schedule([this, alive, channel]() {
                    if (!*alive)
                        return;
                    auto expected = channel.lock();
                    if (!expected)
                        return;
                    std::shared_ptr<Udp> closed;
                    {
                        std::lock_guard<std::mutex> lock(channel_mutex_);
                        if (udp_ == expected)
                            closed = std::move(udp_);
                    }
                    if (!closed)
                        return;
                    expected.reset();
                    closed.reset();
                    if (on_audio_channel_closed_)
                        on_audio_channel_closed_();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to MQTT endpoint");
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        const auto port = endpoint.substr(pos + 1);
        int parsed = 0;
        const auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), parsed);
        if (ec == std::errc() && ptr == port.data() + port.size() && parsed > 0 &&
            parsed <= UINT16_MAX) {
            broker_port = parsed;
        } else {
            ESP_LOGW(TAG, "Invalid MQTT port, using %d", broker_port);
        }
    } else {
        broker_address = endpoint;
    }
    if (auto connected = mqtt_->Connect(broker_address, broker_port, client_id, username, password);
        !connected) {
        mqtt_connected_.store(false);
        ESP_LOGE(TAG, "Failed to connect to endpoint: %s", connected.error().ToString().c_str());
        // EspMqtt otherwise keeps retrying internally after Connect fails.
        mqtt_->Disconnect();
        // A failed cellular connect can also mean that the shared AT channel
        // stopped answering after an earlier HTTP request. Ask the cellular
        // task to check PDP asynchronously rather than leaving the board
        // marked ready and repeatedly attempting MQTT against a dead modem.
        if (mqtt_transport_cellular_)
            ScheduleCellularRecheck();
        if (report_error)
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    if (mqtt_link_generation_ != link_generation_.load() || !board.IsNetworkAvailable() ||
        board.UsesCellularNetwork() != mqtt_transport_cellular_) {
        mqtt_connected_.store(false);
        mqtt_->Disconnect();
        return false;
    }
    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

void MqttProtocol::SuspendUnavailableMqtt() {
    auto& board = Board::GetInstance();
    if (board.IsNetworkAvailable() && !board.IsAudioMaintenanceActive())
        return;
    std::unique_lock<std::mutex> lock(mqtt_mutex_, std::try_to_lock);
    if (lock.owns_lock() && mqtt_) {
        mqtt_connected_.store(false);
        mqtt_->Disconnect();
    }
}

bool MqttProtocol::IsMqttConnectedLocked() {
    if (!mqtt_)
        return false;
    // ML307 IsConnected issues AT+MQTTSTATE and can wait for 13 seconds.
    // Connection events already update this cache; actual Publish failures
    // invalidate it so a later open can reconnect instead of trusting it.
    auto& board = Board::GetInstance();
    if (!board.IsNetworkAvailable() || board.UsesCellularNetwork() != mqtt_transport_cellular_ ||
        mqtt_link_generation_ != link_generation_.load())
        return false;
    return mqtt_transport_cellular_ ? mqtt_connected_.load() : mqtt_->IsConnected();
}

void MqttProtocol::ScheduleCellularRecheck() {
    auto alive = alive_;
    Application::GetInstance().Schedule([alive]() {
        if (!*alive)
            return;
        auto& board = Board::GetInstance();
        if (board.UsesCellularNetwork())
            board.RequestNetworkRecheck();
    });
}

bool MqttProtocol::SendText(const std::string& text) {
    std::unique_lock<std::mutex> lock(mqtt_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    if (publish_topic_.empty() || !IsMqttConnectedLocked()) {
        return false;
    }
    const bool cellular = Board::GetInstance().UsesCellularNetwork();
    const size_t payload_limit = Board::GetInstance().GetProtocolPayloadLimit();
    if (payload_limit != 0 && text.size() > payload_limit) {
        // A local size rejection is not evidence that the modem link failed.
        // Do not invalidate MQTT or schedule a PDP recheck here.
        ESP_LOGE(TAG, "Control message exceeds transport limit (%u > %u bytes)",
                 static_cast<unsigned>(text.size()), static_cast<unsigned>(payload_limit));
        return false;
    }
    [[maybe_unused]] const int64_t publish_started_us = cellular ? esp_timer_get_time() : 0;
    if (!mqtt_->Publish(publish_topic_, text)) {
        if (cellular) {
            mqtt_connected_.store(false);
            ESP_LOGW(TAG, "Cellular MQTT publish failed: %u bytes, %lld ms; rechecking network",
                     static_cast<unsigned>(text.size()),
                     static_cast<long long>((esp_timer_get_time() - publish_started_us) / 1000));
        }
        ESP_LOGE(TAG, "Failed to publish message (%u bytes)", static_cast<unsigned>(text.size()));
        SetError(Lang::Strings::SERVER_ERROR);
        if (cellular)
            ScheduleCellularRecheck();
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    constexpr size_t kAudioHeaderSize = 16;

    // Keep the critical section short: only snapshot the channel state here.
    // udp->Send() below may block waiting for the modem, and the UDP receive
    // callback needs channel_mutex_ on the modem's AT event task.
    std::shared_ptr<Udp> udp;
    std::string nonce;
    uint32_t sequence;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ == nullptr) {
            return false;
        }
        if (aes_nonce_.size() != kAudioHeaderSize || packet->payload.size() > UINT16_MAX) {
            ESP_LOGE(TAG, "Invalid AES nonce or audio payload length: %zu", packet->payload.size());
            return false;
        }
        udp = udp_;
        nonce = aes_nonce_;
        sequence = htonl(++local_sequence_);
    }

    const uint16_t payload_len = htons(static_cast<uint16_t>(packet->payload.size()));
    const uint32_t timestamp = htonl(packet->timestamp);
    memcpy(nonce.data() + 2, &payload_len, sizeof(payload_len));
    memcpy(nonce.data() + 8, &timestamp, sizeof(timestamp));
    memcpy(nonce.data() + 12, &sequence, sizeof(sequence));

    std::string encrypted;
    encrypted.resize(nonce.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    if (!CryptAesCtr(reinterpret_cast<const uint8_t*>(packet->payload.data()),
                     packet->payload.size(), reinterpret_cast<const uint8_t*>(nonce.data()),
                     reinterpret_cast<uint8_t*>(&encrypted[nonce.size()]))) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    // Send without holding channel_mutex_ (see above).
    return udp->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    std::shared_ptr<Udp> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
    }
    udp.reset();

    ESP_LOGI(TAG, "Closing audio channel, send_goodbye: %d", send_goodbye);

    // Only send goodbye when client initiates the close
    // Don't send if server already sent goodbye (to avoid ping-pong)
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id() + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void MqttProtocol::DiscardStaleAudioChannel() {
    std::shared_ptr<Udp> stale;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        stale = std::move(udp_);
    }
    if (stale) {
        ESP_LOGI(TAG, "Closing stale UDP audio channel before reconnect");
        stale.reset();  // May issue AT commands; never hold channel_mutex_ here.
    }
}

bool MqttProtocol::OpenAudioChannel() {
    auto& board = Board::GetInstance();
    if (!board.IsNetworkAvailable() || board.IsAudioMaintenanceActive()) {
        ESP_LOGI(TAG, "MQTT audio connection deferred: network unavailable or maintenance active");
        if (!board.IsAudioMaintenanceActive())
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    std::unique_lock<std::mutex> opening(channel_open_mutex_, std::try_to_lock);
    if (!opening.owns_lock()) {
        ESP_LOGW(TAG, "Audio channel connection already in progress");
        return false;
    }
    if (IsAudioChannelOpened())
        return true;
    // The old channel may be logically timed out while still owning the
    // modem socket. Release it before a new hello or a new UDP client.
    DiscardStaleAudioChannel();
    bool connected = false;
    {
        std::unique_lock<std::mutex> lock(mqtt_mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return false;
        connected = IsMqttConnectedLocked();
    }
    if (!connected) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    SetSessionId({});
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    auto network = board.GetNetwork();
    auto udp = network ? network->CreateUdp(2) : nullptr;
    if (!udp) {
        ESP_LOGE(TAG, "Failed to create UDP audio channel");
        return false;
    }
    udp->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        constexpr size_t kAudioHeaderSize = 16;
        if (data.size() < kAudioHeaderSize) {
            ESP_LOGE(TAG, "Invalid audio packet size: %zu", data.size());
            return;
        }
        if (static_cast<uint8_t>(data[0]) != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", static_cast<uint8_t>(data[0]));
            return;
        }
        uint16_t payload_len = 0;
        uint32_t timestamp = 0;
        uint32_t sequence = 0;
        memcpy(&payload_len, data.data() + 2, sizeof(payload_len));
        memcpy(&timestamp, data.data() + 8, sizeof(timestamp));
        memcpy(&sequence, data.data() + 12, sizeof(sequence));
        payload_len = ntohs(payload_len);
        timestamp = ntohl(timestamp);
        sequence = ntohl(sequence);
        if (data.size() != kAudioHeaderSize + payload_len) {
            ESP_LOGE(TAG, "Audio payload length mismatch: header=%u, datagram=%zu",
                     static_cast<unsigned>(payload_len), data.size() - kAudioHeaderSize);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (sequence <= remote_sequence_) {
                ESP_LOGW(TAG, "Received duplicate/old audio sequence: %lu, last: %lu", sequence,
                         remote_sequence_);
                return;
            }
            if (sequence != remote_sequence_ + 1) {
                // Record the sequence gap without synthesizing repeated PCM.
                // As upstream does, decode the received payload once.
                ESP_LOGI(TAG, "Downlink audio gap of %lu packet(s) before sequence %lu",
                         sequence - remote_sequence_ - 1, sequence);
            }
        }

        const size_t decrypted_size = payload_len;
        auto nonce = reinterpret_cast<const uint8_t*>(data.data());
        auto encrypted = reinterpret_cast<const uint8_t*>(data.data() + kAudioHeaderSize);
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        if (!CryptAesCtr(encrypted, decrypted_size, nonce,
                         reinterpret_cast<uint8_t*>(packet->payload.data()))) {
            ESP_LOGE(TAG, "Failed to decrypt audio data");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            // Re-check under the lock: a reordered packet can arrive while the
            // one that overtook it is still being decrypted above.
            if (sequence <= remote_sequence_) {
                return;
            }
            // Keep bounded gap metadata for diagnostics. A sequence gap is
            // not a reason to replay an earlier spoken syllable.
            const uint32_t gap = sequence - remote_sequence_ - 1;
            packet->missing_before = static_cast<uint8_t>(std::min<uint32_t>(gap, 255));
            remote_sequence_ = sequence;
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
    });

    if (auto connected = udp->Connect(udp_server_, udp_port_); !connected) {
        ESP_LOGE(TAG, "Failed to connect UDP audio channel: %s",
                 connected.error().ToString().c_str());
        if (board.UsesCellularNetwork()) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
            ScheduleCellularRecheck();
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_ = std::move(udp);
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
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

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported or missing transport");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        SetSessionId(session_id->valuestring);
        ESP_LOGI(TAG, "Session established");
    }

    // Get sample rate from hello message
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

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    auto server = cJSON_GetObjectItem(udp, "server");
    auto port = cJSON_GetObjectItem(udp, "port");
    auto key_item = cJSON_GetObjectItem(udp, "key");
    auto nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || !cJSON_IsNumber(port) || port->valueint <= 0 ||
        port->valueint > UINT16_MAX || !cJSON_IsString(key_item) || !cJSON_IsString(nonce_item)) {
        ESP_LOGE(TAG, "Invalid UDP server, port, key, or nonce");
        return;
    }
    const std::string udp_server = server->valuestring;
    const int udp_port = port->valueint;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_,
    // encryption);
    std::string aes_nonce;
    std::string aes_key;
    if (!DecodeHexString(nonce_item->valuestring, aes_nonce) ||
        !DecodeHexString(key_item->valuestring, aes_key) || aes_nonce.size() != 16 ||
        aes_key.size() != 16) {
        ESP_LOGE(TAG, "Invalid AES key or nonce length");
        return;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Crypto, status: %ld", static_cast<long>(status));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (aes_key_id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(aes_key_id_);
            aes_key_id_ = PSA_KEY_ID_NULL;
        }

        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        status = psa_import_key(&attributes, reinterpret_cast<const uint8_t*>(aes_key.data()),
                                aes_key.size(), &aes_key_id_);
        psa_reset_key_attributes(&attributes);
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to import AES key, status: %ld", static_cast<long>(status));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_server_ = udp_server;
        udp_port_ = udp_port;
        aes_nonce_ = std::move(aes_nonce);
        local_sequence_ = 0;
        remote_sequence_ = 0;
    }
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

bool MqttProtocol::CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                               uint8_t* output) {
    std::lock_guard<std::mutex> lock(crypto_mutex_);
    if (aes_key_id_ == PSA_KEY_ID_NULL || input == nullptr || nonce == nullptr ||
        output == nullptr) {
        return false;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = psa_cipher_encrypt_setup(&operation, aes_key_id_, PSA_ALG_CTR);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, nonce, 16);
    }

    size_t output_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, input, input_size, output, input_size, &output_len);
    }

    uint8_t finish_output[16];
    size_t finish_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, finish_output, sizeof(finish_output), &finish_len);
    }
    psa_cipher_abort(&operation);

    if (status != PSA_SUCCESS || output_len != input_size || finish_len != 0) {
        ESP_LOGE(TAG, "AES-CTR operation failed, status: %ld", static_cast<long>(status));
        return false;
    }
    return true;
}

static inline int CharToHex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool MqttProtocol::DecodeHexString(const std::string& hex_string, std::string& decoded) {
    decoded.clear();
    if ((hex_string.size() % 2) != 0) {
        return false;
    }
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        int high = CharToHex(hex_string[i]);
        int low = CharToHex(hex_string[i + 1]);
        if (high < 0 || low < 0) {
            decoded.clear();
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
