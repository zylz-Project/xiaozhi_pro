#include "dinosaur_ml307_mqtt.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <esp_log.h>
#include <at_uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dinosaur_ml307_at.h"

namespace {
constexpr char kTag[] = "DinoMl307Mqtt";
constexpr int kConfigTimeoutMs = 5000;
constexpr int kProbeTimeoutMs = 1500;
constexpr int kConnectCommandTimeoutMs = 5000;
constexpr int kConnectWaitMs = 20000;
constexpr int kStateQueryTimeoutMs = 2000;
constexpr int kDisconnectTimeoutMs = 2000;
// ML307R accepts at most 1024 bytes for MQTTPUB, including data mode. The
// receive direction is a URC and is not subject to that publish limit, but it
// stays bounded so a malformed or hostile broker cannot exhaust RAM.
constexpr size_t kMaxPublishBytes = 1024;
constexpr size_t kMaxIncomingMessageBytes = 8192;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kDisconnectedBit = BIT1;
constexpr EventBits_t kStateBit = BIT2;
constexpr EventBits_t kCleanupBit = BIT3;
constexpr EventBits_t kPublishBit = BIT4;
// Documented MQTTPUB ordering is +MQTTPUB before OK, but older module
// firmware can emit the URC just after the final OK. Wait briefly for it after
// the command returns so a valid publish is not reported as a length failure.
constexpr int kPublishResultTimeoutMs = 500;

bool IsInt(const AtArgumentValue& value) { return value.type == AtArgumentValue::Type::Int; }

bool IsString(const AtArgumentValue& value) { return value.type == AtArgumentValue::Type::String; }

// The AT parser classifies an unquoted HEX payload made only of digits as an
// integer even though it preserves the original text. Accept both types and
// always use string_value for payload data.
bool IsHexValue(const AtArgumentValue& value) {
    return value.type == AtArgumentValue::Type::Int || value.type == AtArgumentValue::Type::String;
}

bool IsHexString(const std::string& value) {
    if ((value.size() & 1U) != 0)
        return false;
    for (unsigned char ch : value) {
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}

bool SafeAtString(const std::string& value, size_t max_length) {
    if (value.size() > max_length)
        return false;
    for (unsigned char ch : value) {
        // The component AT parser splits arguments on every comma, including
        // commas inside quotes, so none of these strings may contain one.
        if (ch == '"' || ch == '\\' || ch == ',' || ch == '\r' || ch == '\n' || ch < 0x20 ||
            ch == 0x7f)
            return false;
    }
    return true;
}

// The component AT parser splits arguments on every comma even when the comma
// is inside a quoted string. A topic containing one would therefore corrupt
// both the command and the publish URC grammar.
bool SafeAtTopic(const std::string& value, size_t max_length) {
    return SafeAtString(value, max_length) && value.find(',') == std::string::npos;
}

// +MQTTPUB reports the complete MQTT packet length, not the length of the
// payload we supplied. Reproduce the packet-size calculation so a truncated
// upload is still rejected without rejecting every valid publish.
size_t ExpectedPublishPacketLength(size_t topic_length, size_t payload_length, int qos) {
    size_t remaining = 2 + topic_length + payload_length;
    if (qos > 0)
        remaining += 2;
    size_t length_bytes = 1;
    for (size_t value = remaining; value >= 128; value /= 128)
        ++length_bytes;
    return 1 + length_bytes + remaining;
}

size_t TimeoutForBytes(const AtUart& uart, size_t bytes, size_t floor_ms) {
    int baud = uart.GetBaudRate();
    if (baud <= 0)
        baud = 115200;
    const uint64_t tx_ms =
        (static_cast<uint64_t>(bytes) * 10ULL * 1000ULL) / static_cast<unsigned>(baud);
    return std::max<size_t>(floor_ms, static_cast<size_t>(tx_ms) + 2000);
}
}  // namespace

DinosaurMl307Mqtt::DinosaurMl307Mqtt(std::shared_ptr<AtUart> at_uart, int mqtt_id)
    : at_uart_(std::move(at_uart)), mqtt_id_(mqtt_id) {
    event_group_handle_ = xEventGroupCreate();
    if (!event_group_handle_) {
        ESP_LOGE(kTag, "Cannot allocate MQTT event group");
        return;
    }
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            HandleUrc(command, arguments);
        });
}

DinosaurMl307Mqtt::~DinosaurMl307Mqtt() {
    if (event_group_handle_) {
        // Unregister first: Disconnect() below must not call back into a
        // partially destroyed object from the AT event task.
        at_uart_->UnregisterUrcCallback(urc_callback_it_);
        Disconnect();
        vEventGroupDelete(event_group_handle_);
    }
}

bool DinosaurMl307Mqtt::Configure(const std::string& command) {
    if (AtCommandSucceeded(at_uart_->SendCommand(command, kConfigTimeoutMs)))
        return true;

    // A late URC can leave a response queued after a timeout. Probe the AT
    // parser before one retry so the next configuration command does not
    // consume the previous command's OK.
    ESP_LOGW(kTag, "MQTT configuration timed out; resynchronising AT parser");
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!AtCommandSucceeded(at_uart_->SendCommand("AT", kProbeTimeoutMs)))
        return false;
    return AtCommandSucceeded(at_uart_->SendCommand(command, kConfigTimeoutMs));
}

NetworkResult<> DinosaurMl307Mqtt::Connect(const std::string broker_address, int broker_port,
                                           const std::string client_id,
                                           const std::string username,
                                           const std::string password) {
    if (!event_group_handle_ || !at_uart_) {
        last_error_.store(-1);
        return Fail(NetworkError::NotInitialized());
    }
    if (broker_address.empty() || broker_port <= 0 || broker_port > 65535 ||
        !SafeAtString(broker_address, 128) || !SafeAtString(client_id, 128) ||
        !SafeAtString(username, 128) || !SafeAtString(password, 256)) {
        last_error_.store(-1);
        ESP_LOGE(kTag, "Invalid MQTT endpoint or credential string");
        return Fail(NetworkError::InvalidArgument());
    }

    std::lock_guard<std::mutex> connect_lock(connect_mutex_);
    SetConnected(false, false);
    {
        std::lock_guard<std::mutex> message_lock(message_mutex_);
        ResetIncomingLocked();
    }
    const uint32_t session_serial = session_serial_.fetch_add(1) + 1;
    pending_cleanup_serial_.store(0);
    last_error_.store(0);
    queried_state_.store(-1);

    // MQTTCFG parameters and the MQTT session survive an ESP32 reset until the
    // modem itself reboots. Probe with a best-effort disconnect even on a fresh
    // adapter instance: an inactive connection returns CME, which is harmless,
    // while a stale one would make MQTTCONN reject the duplicate id.
    session_possible_.store(true);
    xEventGroupClearBits(event_group_handle_, kCleanupBit);
    pending_cleanup_serial_.store(session_serial);
    const auto disconnect_result =
        at_uart_->SendCommand("AT+MQTTDISC=" + std::to_string(mqtt_id_), kDisconnectTimeoutMs);
    const bool disconnect_acked = AtCommandSucceeded(disconnect_result);
    bool session_cleared = disconnect_acked;
    if (disconnect_acked) {
        // Consume the documented "+MQTTURC: conn,<id>,2" while it still
        // belongs to the cleanup, not to the new MQTTCONN that follows.
        xEventGroupWaitBits(event_group_handle_, kCleanupBit, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(kDisconnectTimeoutMs));
    } else if (AtCommandCme(disconnect_result) != 0) {
        // A CME proves that this id had no session to release.
        session_cleared = true;
    } else {
        ESP_LOGW(kTag, "Stale MQTT session did not acknowledge disconnect");
    }
    // From this point on a late conn,2 belongs to the previous session and
    // must not tear down the MQTTCONN that follows.
    pending_cleanup_serial_.store(0);
    session_possible_.store(!session_cleared);
    if (!session_cleared && !RecoverMl307AtParser(*at_uart_, kProbeTimeoutMs)) {
        // The disconnect timed out, so its final response can still be queued
        // in the AT parser. Keep the session marked for cleanup the next time.
        last_error_.store(-1);
        ESP_LOGE(kTag, "Cannot resynchronise AT parser after stale MQTT cleanup");
        return Fail(NetworkError::Timeout());
    }
    xEventGroupClearBits(event_group_handle_,
                         kConnectedBit | kDisconnectedBit | kStateBit | kCleanupBit);

    const int keepalive = keep_alive_seconds_ == 0 ? 0 : std::clamp(keep_alive_seconds_, 60, 65535);
    const int ping_interval = keepalive >= 60 ? keepalive : 120;
    const std::string base = std::to_string(mqtt_id_);
    if (!Configure("AT+MQTTCFG=\"ssl\"," + base + "," + (broker_port == 8883 ? "1" : "0")) ||
        !Configure("AT+MQTTCFG=\"clean\"," + base + ",1") ||
        !Configure("AT+MQTTCFG=\"keepalive\"," + base + "," + std::to_string(keepalive)) ||
        !Configure("AT+MQTTCFG=\"pingreq\"," + base + "," + std::to_string(ping_interval)) ||
        !Configure("AT+MQTTCFG=\"encoding\"," + base + ",0,1")) {
        last_error_.store(-1);
        ESP_LOGE(kTag, "MQTT configuration failed");
        return Fail(NetworkError::AtCommandFailed());
    }

    xEventGroupClearBits(event_group_handle_, kConnectedBit | kDisconnectedBit);
    session_possible_.store(true);
    const std::string command = "AT+MQTTCONN=" + base + ",\"" + broker_address + "\"," +
                                std::to_string(broker_port) + ",\"" + client_id + "\",\"" +
                                username + "\",\"" + password + "\"";
    auto connect_result = at_uart_->SendCommand(command, kConnectCommandTimeoutMs);
    bool connect_accepted = AtCommandSucceeded(connect_result);
    if (!connect_accepted && AtCommandCme(connect_result) != 0) {
        // A CME is a definitive rejection. The most common recoverable case is
        // a duplicate connect id left by an earlier firmware run; clear it and
        // retry once. Credential/protocol rejections fail again and are final.
        ESP_LOGW(kTag, "MQTTCONN rejected (cme=%d); clearing session and retrying",
                 AtCommandCme(connect_result));
        xEventGroupClearBits(event_group_handle_, kCleanupBit);
        pending_cleanup_serial_.store(session_serial);
        if (at_uart_->SendCommand("AT+MQTTDISC=" + base, kDisconnectTimeoutMs)) {
            xEventGroupWaitBits(event_group_handle_, kCleanupBit, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(kDisconnectTimeoutMs));
            session_possible_.store(false);
        }
        pending_cleanup_serial_.store(0);
        vTaskDelay(pdMS_TO_TICKS(200));
        xEventGroupClearBits(event_group_handle_, kConnectedBit | kDisconnectedBit | kCleanupBit);
        session_possible_.store(true);
        connect_result = at_uart_->SendCommand(command, kConnectCommandTimeoutMs);
        connect_accepted = AtCommandSucceeded(connect_result);
        if (!connect_accepted && AtCommandCme(connect_result) != 0) {
            const int cme = AtCommandCme(connect_result);
            last_error_.store(cme);
            ESP_LOGE(kTag, "MQTTCONN rejected again (cme=%d)", cme);
            return Fail(NetworkError::CmeError(cme));
        }
    }
    if (!connect_accepted) {
        // The command may have timed out while the modem is still opening the
        // TLS/MQTT session. Wait for the connection URC instead of reporting a
        // failure immediately; Connect() below still has a bounded deadline.
        ESP_LOGW(kTag, "MQTTCONN command did not return OK; waiting for connection URC");
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, kConnectedBit | kDisconnectedBit, pdTRUE,
                                    pdFALSE, pdMS_TO_TICKS(kConnectWaitMs));
    if ((bits & kConnectedBit) && connected_.load()) {
        ESP_LOGI(kTag, "MQTT connected");
        return {};
    }
    if (bits & kDisconnectedBit) {
        ESP_LOGW(kTag, "MQTT connection rejected, code=%d", last_error_.load());
        const int code = last_error_.load();
        return Fail(code > 0 ? NetworkError::FromMl307Mqtt(code)
                             : NetworkError::ConnectFailed());
    }

    if (!connect_accepted) {
        // The command timed out, so its OK and conn URC can still be queued in
        // the AT parser. Drain them with two plain probes before querying
        // state; otherwise the first MQTTSTATE response could actually be the
        // delayed output of MQTTCONN. The second probe proves a fresh command
        // generated after that output can complete.
        if (!RecoverMl307AtParser(*at_uart_, kProbeTimeoutMs)) {
            last_error_.store(-1);
            ESP_LOGE(kTag, "Cannot resynchronise AT parser after MQTTCONN timeout");
            return Fail(NetworkError::Timeout());
        }
        bits = xEventGroupWaitBits(event_group_handle_, kConnectedBit | kDisconnectedBit,
                                   pdTRUE, pdFALSE, 0);
        if ((bits & kConnectedBit) && connected_.load()) {
            ESP_LOGI(kTag, "MQTT connected after delayed URC");
            return {};
        }
        if (bits & kDisconnectedBit) {
            const int code = last_error_.load();
            return Fail(code > 0 ? NetworkError::FromMl307Mqtt(code)
                                 : NetworkError::ConnectFailed());
        }
    }

    // Some modem firmware omits the conn URC after a delayed TLS handshake.
    // Query once, with a short timeout, and accept only the documented state 2.
    if (QueryState(kStateQueryTimeoutMs) && queried_state_.load() == 2 && connected_.load()) {
        ESP_LOGI(kTag, "MQTT connected (state query fallback)");
        return {};
    }
    if (queried_state_.load() == 3) {
        last_error_.store(-1);
        ESP_LOGE(kTag, "MQTT state query reports a disconnected session");
        return Fail(NetworkError::ServerDisconnected());
    }
    if (queried_state_.load() == 1) {
        bits = xEventGroupWaitBits(event_group_handle_, kConnectedBit | kDisconnectedBit, pdTRUE,
                                   pdFALSE, pdMS_TO_TICKS(5000));
        if ((bits & kConnectedBit) && connected_.load()) {
            ESP_LOGI(kTag, "MQTT connected after delayed reconnect URC");
            return {};
        }
    }

    last_error_.store(-1);
    ESP_LOGE(kTag, "MQTT connection timed out");
    return Fail(NetworkError::Timeout());
}

void DinosaurMl307Mqtt::Disconnect() {
    std::lock_guard<std::mutex> connect_lock(connect_mutex_);
    const uint32_t session_serial = session_serial_.fetch_add(1) + 1;
    pending_cleanup_serial_.store(0);
    const bool was_connected = connected_.exchange(false);
    if (was_connected)
        session_possible_.store(true);
    {
        std::lock_guard<std::mutex> message_lock(message_mutex_);
        ResetIncomingLocked();
    }
    if (!session_possible_.load())
        return;
    pending_cleanup_serial_.store(session_serial);
    const auto disconnect_result =
        at_uart_->SendCommand("AT+MQTTDISC=" + std::to_string(mqtt_id_), kDisconnectTimeoutMs);
    if (AtCommandSucceeded(disconnect_result)) {
        session_possible_.store(false);
    } else if (AtCommandCme(disconnect_result) != 0) {
        // CME proves that this id had no session to release.
        session_possible_.store(false);
        pending_cleanup_serial_.store(0);
    } else {
        // A timeout leaves the modem state unknown. Keep the flag so the next
        // Connect() performs another best-effort cleanup. Keep the pending
        // serial as well so a delayed conn,2 can complete this cleanup.
        session_possible_.store(true);
        ESP_LOGW(kTag, "MQTT disconnect was not acknowledged");
    }
}

bool DinosaurMl307Mqtt::Publish(const std::string topic, const std::string payload, int qos) {
    std::lock_guard<std::mutex> publish_lock(publish_mutex_);
    if (!connected_.load())
        return false;
    if (topic.empty() || !SafeAtTopic(topic, 256) || payload.empty() ||
        payload.size() > kMaxPublishBytes)
        return false;
    if (qos < 0 || qos > 2)
        return false;

    const size_t expected_packet_length =
        ExpectedPublishPacketLength(topic.size(), payload.size(), qos);
    std::string command = "AT+MQTTPUB=" + std::to_string(mqtt_id_) + ",\"" + topic + "\"," +
                          std::to_string(qos) + ",0,0," + std::to_string(payload.size());
    pending_publish_size_.store(payload.size());
    publish_accepted_size_.store(-1);
    xEventGroupClearBits(event_group_handle_, kPublishBit);
    const size_t timeout_ms = TimeoutForBytes(*at_uart_, command.size() + payload.size() + 8, 3000);
    if (!at_uart_->SendCommandWithData(command, timeout_ms, true,
                                       payload.empty() ? nullptr : payload.data(),
                                       payload.size())) {
        pending_publish_size_.store(0);
        SetConnected(false, false);
        session_possible_.store(true);
        last_error_.store(-1);
        ESP_LOGW(kTag, "MQTT publish failed (%u bytes)", static_cast<unsigned>(payload.size()));
        return false;
    }
    int accepted = publish_accepted_size_.load();
    if (accepted < 0) {
        xEventGroupWaitBits(event_group_handle_, kPublishBit, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(kPublishResultTimeoutMs));
        accepted = publish_accepted_size_.load();
    }
    pending_publish_size_.store(0);
    if (accepted != static_cast<int>(expected_packet_length)) {
        SetConnected(false, false);
        session_possible_.store(true);
        last_error_.store(-1);
        ESP_LOGW(kTag, "MQTT publish length mismatch: %d/%u (payload=%u)", accepted,
                 static_cast<unsigned>(expected_packet_length),
                 static_cast<unsigned>(payload.size()));
        return false;
    }
    return true;
}

bool DinosaurMl307Mqtt::Subscribe(const std::string topic, int qos) {
    if (!connected_.load() || topic.empty() || !SafeAtTopic(topic, 256) || qos < 0 || qos > 2)
        return false;
    return AtCommandSucceeded(at_uart_->SendCommand(
        "AT+MQTTSUB=" + std::to_string(mqtt_id_) + ",\"" + topic + "\"," + std::to_string(qos),
        kConfigTimeoutMs));
}

bool DinosaurMl307Mqtt::Unsubscribe(const std::string topic) {
    if (!connected_.load() || topic.empty() || !SafeAtTopic(topic, 256))
        return false;
    return AtCommandSucceeded(at_uart_->SendCommand(
        "AT+MQTTUNSUB=" + std::to_string(mqtt_id_) + ",\"" + topic + "\"", kConfigTimeoutMs));
}

bool DinosaurMl307Mqtt::IsConnected() { return connected_.load(); }

bool DinosaurMl307Mqtt::QueryState(int timeout_ms) {
    if (!event_group_handle_)
        return false;
    xEventGroupClearBits(event_group_handle_, kStateBit);
    queried_state_.store(-1);
    state_query_pending_.store(true);
    const bool command_ok =
        AtCommandSucceeded(at_uart_->SendCommand("AT+MQTTSTATE=" + std::to_string(mqtt_id_),
                                                 timeout_ms));
    EventBits_t bits = 0;
    if (command_ok) {
        bits = xEventGroupWaitBits(event_group_handle_, kStateBit, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(timeout_ms));
    }
    state_query_pending_.store(false);
    if (command_ok && (bits & kStateBit))
        return true;
    // A late +MQTTSTATE must not be claimed by the next health query. Two
    // plain probes drain any final response and prove the parser is sane.
    for (int pass = 0; pass < 2; ++pass) {
        if (!AtCommandSucceeded(at_uart_->SendCommand("AT", kProbeTimeoutMs)))
            break;
    }
    return false;
}

void DinosaurMl307Mqtt::SetConnected(bool connected, bool notify) {
    const bool was_connected = connected_.exchange(connected);
    if (connected == was_connected)
        return;
    if (connected) {
        last_error_.store(0);
        if (notify && on_connected_callback_)
            on_connected_callback_();
    } else if (notify && on_disconnected_callback_) {
        on_disconnected_callback_();
    }
}

void DinosaurMl307Mqtt::ResetIncomingLocked() {
    message_payload_.clear();
    message_id_ = -1;
    message_topic_.clear();
    message_expected_ = 0;
}

void DinosaurMl307Mqtt::HandleUrc(const std::string& command,
                                  const std::vector<AtArgumentValue>& arguments) {
    if (IsMl307TransportLost(command, arguments)) {
        const bool was_connected = connected_.exchange(false);
        session_possible_.store(true);
        pending_cleanup_serial_.store(0);
        {
            std::lock_guard<std::mutex> message_lock(message_mutex_);
            ResetIncomingLocked();
        }
        last_error_.store(-1);
        xEventGroupSetBits(event_group_handle_, kDisconnectedBit);
        if (was_connected && on_disconnected_callback_)
            on_disconnected_callback_();
        return;
    }

    if (command == "MQTTSTATE") {
        if (!state_query_pending_.load()) {
            ESP_LOGW(kTag, "Ignoring unsolicited MQTTSTATE response");
            return;
        }
        if (arguments.size() < 1 || !IsInt(arguments[0])) {
            ESP_LOGW(kTag, "Malformed MQTTSTATE response");
            return;
        }
        const int state = arguments[0].int_value;
        queried_state_.store(state);
        if (state == 2) {
            session_possible_.store(true);
            pending_cleanup_serial_.store(0);
            SetConnected(true, true);
        } else if (state == 3) {
            session_possible_.store(false);
            pending_cleanup_serial_.store(0);
            SetConnected(false, true);
            xEventGroupSetBits(event_group_handle_, kDisconnectedBit);
        } else if (state == 1) {
            session_possible_.store(true);
        } else {
            ESP_LOGW(kTag, "Unexpected MQTT state %d", state);
        }
        xEventGroupSetBits(event_group_handle_, kStateBit);
        return;
    }

    if (command == "MQTTPUB") {
        if (arguments.size() < 3 || !IsInt(arguments[0]) || !IsInt(arguments[1]) ||
            !IsInt(arguments[2]) || arguments[0].int_value != mqtt_id_) {
            ESP_LOGW(kTag, "Malformed MQTTPUB response");
            return;
        }
        if (pending_publish_size_.load() != 0) {
            publish_accepted_size_.store(arguments[2].int_value);
            xEventGroupSetBits(event_group_handle_, kPublishBit);
        }
        return;
    }

    if (command != "MQTTURC")
        return;
    if (arguments.size() < 2 || !IsString(arguments[0]) || !IsInt(arguments[1]) ||
        arguments[1].int_value != mqtt_id_)
        return;

    const std::string& type = arguments[0].string_value;
    if (type == "conn") {
        if (arguments.size() < 3 || !IsInt(arguments[2])) {
            ESP_LOGW(kTag, "Malformed MQTT conn URC");
            return;
        }
        const int code = arguments[2].int_value;
        if (code == 2) {
            // MQTTDISC reports conn,2 asynchronously. Only the disconnect
            // issued by the current session serial may tear down the current
            // connection; a delayed copy from an older session is ignored.
            const uint32_t current_serial = session_serial_.load();
            const uint32_t expected_serial = pending_cleanup_serial_.load();
            if (expected_serial == 0 || expected_serial != current_serial) {
                ESP_LOGW(kTag, "Ignoring stale MQTT conn,2 (expected=%lu current=%lu)",
                         static_cast<unsigned long>(expected_serial),
                         static_cast<unsigned long>(current_serial));
                return;
            }
            pending_cleanup_serial_.store(0);
            xEventGroupSetBits(event_group_handle_, kCleanupBit);
            session_possible_.store(false);
            if (connected_.exchange(false)) {
                last_error_.store(code);
                xEventGroupSetBits(event_group_handle_, kDisconnectedBit);
                if (on_disconnected_callback_)
                    on_disconnected_callback_();
            }
            return;
        }
        last_error_.store(code);
        if (code == 0) {
            session_possible_.store(true);
            SetConnected(true, true);
            xEventGroupSetBits(event_group_handle_, kConnectedBit);
        } else if (code == 1) {
            session_possible_.store(true);
            // The modem is reconnecting, so an established session is no
            // longer usable. During the initial Connect() no connected bit is
            // set yet; wait for the eventual 0/terminal state in that case.
            if (connected_.exchange(false)) {
                xEventGroupSetBits(event_group_handle_, kDisconnectedBit);
                if (on_disconnected_callback_)
                    on_disconnected_callback_();
            }
        } else {
            session_possible_.store(code == 4 || code == 5 || code == 6);
            SetConnected(false, true);
            xEventGroupSetBits(event_group_handle_, kDisconnectedBit);
            if (code == 5 || code == 6) {
                const std::string message = ErrorToString(code);
                if (on_error_callback_)
                    on_error_callback_(message);
            }
        }
        return;
    }

    if (type == "publish") {
        if (arguments.size() != 7 || !IsInt(arguments[2]) || !IsString(arguments[3]) ||
            !IsInt(arguments[4]) || !IsInt(arguments[5]) || !IsHexValue(arguments[6])) {
            ESP_LOGW(kTag, "Malformed MQTT publish URC");
            return;
        }
        const int total = arguments[4].int_value;
        const int part = arguments[5].int_value;
        const int message_id = arguments[2].int_value;
        const std::string& topic = arguments[3].string_value;
        if (message_id < 0 || topic.empty() || !SafeAtTopic(topic, 256) || total < 0 ||
            total > static_cast<int>(kMaxIncomingMessageBytes) || part < 0 || part > total ||
            !IsHexString(arguments[6].string_value) ||
            arguments[6].string_value.size() != static_cast<size_t>(part) * 2) {
            ESP_LOGE(kTag, "Invalid MQTT publish lengths total=%d part=%d", total, part);
            std::lock_guard<std::mutex> message_lock(message_mutex_);
            ResetIncomingLocked();
            return;
        }
        std::string completed_topic;
        std::string completed_payload;
        bool completed = false;
        {
            std::lock_guard<std::mutex> message_lock(message_mutex_);
            if (message_id_ != message_id || message_topic_ != topic ||
                message_expected_ != static_cast<size_t>(total)) {
                if (!message_payload_.empty()) {
                    ESP_LOGW(kTag, "Discarding incomplete MQTT message %d on %s", message_id_,
                             message_topic_.c_str());
                }
                ResetIncomingLocked();
                message_id_ = message_id;
                message_topic_ = topic;
                message_expected_ = static_cast<size_t>(total);
            }
            const std::string chunk =
                part > 0 ? at_uart_->DecodeHex(arguments[6].string_value) : std::string();
            if (message_payload_.size() + chunk.size() > message_expected_) {
                ESP_LOGE(kTag, "MQTT publish payload overflow for message %d", message_id_);
                ResetIncomingLocked();
                return;
            }
            message_payload_.append(chunk);
            if (message_payload_.size() == message_expected_) {
                completed_topic = message_topic_;
                completed_payload = std::move(message_payload_);
                completed = true;
                ResetIncomingLocked();
            }
        }
        if (completed && on_message_callback_) {
            on_message_callback_(completed_topic, completed_payload);
        }
        return;
    }

    if (type == "timeout") {
        last_error_.store(-1);
        ESP_LOGW(kTag, "MQTT operation timed out");
    }
}

std::string DinosaurMl307Mqtt::ErrorToString(int code) {
    switch (code) {
        case 0:
            return "Connected";
        case 1:
            return "Reconnecting";
        case 2:
            return "Disconnected: user initiated";
        case 3:
            return "Disconnected: connection rejected";
        case 4:
            return "Disconnected: server closed";
        case 5:
            return "Disconnected: ping timeout";
        case 6:
            return "Disconnected: network error";
        case 255:
            return "Disconnected: unknown error";
        default:
            return "Unknown error";
    }
}
