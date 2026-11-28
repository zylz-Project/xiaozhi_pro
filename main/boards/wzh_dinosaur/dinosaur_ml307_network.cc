#include "dinosaur_ml307_network.h"

#include <sdkconfig.h>
#include "config.h"

#if DINOSAUR_ENABLE_4G

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <esp_log.h>
#include <at_uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "dinosaur_ml307_at.h"
#include "dinosaur_ml307_http.h"
#include "dinosaur_ml307_mqtt.h"

namespace {
constexpr char kUdpTag[] = "DinoMl307Udp";
constexpr char kTcpTag[] = "DinoMl307Tcp";
// TCP MIPOPEN carries its own modem-side connect deadline. Keep it shorter than
// the application wait so the modem reports a definitive +MIPOPEN failure
// before we abandon the command. Otherwise the default 60-second modem timeout
// can complete a socket after Connect() has already returned, leaving an
// unowned session behind that later audio traffic cannot use.
constexpr int kUdpConnectTimeoutMs = 10000;
constexpr int kTcpModemOpenTimeoutSeconds = 15;
constexpr int kTcpConnectTimeoutMs = 18000;
constexpr int kConfigTimeoutMs = 3000;
constexpr int kCloseTimeoutMs = 3000;
constexpr int kCloseUrcTimeoutMs = 1000;
constexpr int kOpenTimeoutMs = 5000;
constexpr size_t kSendTimeoutFloorMs = 2000;
constexpr size_t kSendCompleteTimeoutMs = 3000;
constexpr int kMinSocketId = 0;
constexpr int kMaxSocketId = 5;
// The previous upstream UDP implementation capped inline HEX at 1460/2 bytes.
// Opus audio frames fit comfortably; a larger datagram fails the channel
// rather than desynchronizing the AT stream with the binary prompt flow.
constexpr size_t kMaxInlinePayloadBytes = 1460 / 2;

constexpr EventBits_t kOpenResolved = BIT1;
constexpr EventBits_t kOpenFailed = BIT2;
constexpr EventBits_t kCloseResolved = BIT3;
constexpr EventBits_t kSendResolved = BIT4;
constexpr EventBits_t kSendFailed = BIT5;

bool IsInt(const AtArgumentValue& value) { return value.type == AtArgumentValue::Type::Int; }

bool IsString(const AtArgumentValue& value) { return value.type == AtArgumentValue::Type::String; }

bool IsValidSocketId(int id) { return id >= kMinSocketId && id <= kMaxSocketId; }

// The address is placed inside a quoted AT argument. The local parser also
// splits URC arguments on every comma, even inside quotes, so reject anything
// that could break either the modem command or the response grammar.
bool IsSafeEndpoint(const std::string& host, int port) {
    if (host.empty() || host.size() > 255 || port <= 0 || port > 65535)
        return false;
    for (unsigned char ch : host) {
        if (ch <= 0x20 || ch == 0x7f || ch == '"' || ch == '\\' || ch == ',' || ch == '\r' ||
            ch == '\n') {
            return false;
        }
    }
    return true;
}

// Unquoted HEX payloads made only of digits are reported as integers by the
// AT parser while retaining string_value. Accept both representations.
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

size_t TimeoutForBytes(const AtUart& uart, size_t bytes, size_t floor_ms) {
    int baud = uart.GetBaudRate();
    if (baud <= 0)
        baud = 115200;
    const uint64_t tx_ms =
        (static_cast<uint64_t>(bytes) * 10ULL * 1000ULL) / static_cast<unsigned>(baud);
    return std::max<size_t>(floor_ms, static_cast<size_t>(tx_ms) + 1500);
}

class DinosaurMl307HexUdp final : public Udp {
public:
    DinosaurMl307HexUdp(std::shared_ptr<AtUart> at_uart, int udp_id)
        : at_uart_(std::move(at_uart)), udp_id_(udp_id) {
        if (!at_uart_ || !IsValidSocketId(udp_id_)) {
            ESP_LOGE(kUdpTag, "Invalid ML307 UDP socket id %d", udp_id_);
            return;
        }
        event_group_handle_ = xEventGroupCreate();
        if (!event_group_handle_) {
            ESP_LOGE(kUdpTag, "Cannot allocate UDP event group");
            return;
        }
        urc_callback_it_ = at_uart_->RegisterUrcCallback(
            [this](const std::string& command, const std::vector<AtArgumentValue>& args) {
                HandleUrc(command, args);
            });
        urc_callback_registered_ = true;
    }

    ~DinosaurMl307HexUdp() override {
        if (urc_callback_registered_) {
            // Unregister before closing: no callback can observe this object
            // after the call returns.
            at_uart_->UnregisterUrcCallback(urc_callback_it_);
            urc_callback_registered_ = false;
        }
        connected_ = false;
        connected_state_.store(false);
        if (socket_may_exist_.exchange(false)) {
            const auto close_result =
                at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(udp_id_), kCloseTimeoutMs);
            if (!AtCommandSucceeded(close_result) && AtCommandCme(close_result) == 0) {
                ESP_LOGW(kUdpTag, "Failed to close UDP socket %d during teardown", udp_id_);
            }
        }
        if (event_group_handle_) {
            vEventGroupDelete(event_group_handle_);
        }
    }

    NetworkResult<> Connect(const std::string& host, int port) override {
        if (!event_group_handle_ || !at_uart_ || !IsSafeEndpoint(host, port)) {
            ESP_LOGE(kUdpTag, "Invalid UDP endpoint or socket state");
            return Fail(NetworkError::InvalidArgument());
        }

        std::lock_guard<std::mutex> lock(io_mutex_);
        connected_ = false;
        connected_state_.store(false);
        instance_active_.store(false);
        pending_send_size_.store(0);
        xEventGroupClearBits(event_group_handle_, kOpenResolved | kOpenFailed | kCloseResolved |
                                                      kSendResolved | kSendFailed);

        // Best-effort cleanup. The modem may still own this socket id from a
        // previous firmware run even when this adapter has no local state. An
        // inactive id answers CME (harmless); a timeout is re-synchronised
        // with a plain AT probe before a new socket is opened.
        socket_may_exist_.store(true);
        expecting_close_.store(true);
        const auto close_result =
            at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(udp_id_), kCloseTimeoutMs);
        const bool close_acked = AtCommandSucceeded(close_result);
        bool close_confirmed = close_acked;
        if (close_acked) {
            xEventGroupWaitBits(event_group_handle_, kCloseResolved, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(kCloseUrcTimeoutMs));
        } else if (AtCommandCme(close_result) == 0) {
            close_confirmed = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            auto probe_result = at_uart_->SendCommand("AT", 1000);
            if (!probe_result) {
                expecting_close_.store(false);
                ESP_LOGE(kUdpTag, "Cannot resynchronise AT parser after UDP cleanup");
                return Fail(probe_result.error().ToNetworkError());
            }
        } else {
            // A CME proves that the modem answered and no socket exists.
            close_confirmed = true;
        }
        expecting_close_.store(false);
        socket_may_exist_.store(!close_confirmed);
        xEventGroupClearBits(event_group_handle_, kCloseResolved | kOpenResolved | kOpenFailed);
        connected_state_.store(false);
        instance_active_.store(false);

        // 1,1 selects inline HEX for both directions. The receive path still
        // decodes MIPURC "rudp" payloads exactly as before.
        if (!at_uart_->SendCommand("AT+MIPCFG=\"encoding\"," + std::to_string(udp_id_) + ",1,1",
                                   kConfigTimeoutMs)) {
            ESP_LOGE(kUdpTag, "Failed to enable inline HEX for UDP socket %d", udp_id_);
            return Fail(NetworkError::AtCommandFailed());
        }
        if (!at_uart_->SendCommand("AT+MIPCFG=\"ssl\"," + std::to_string(udp_id_) + ",0,0",
                                   kConfigTimeoutMs)) {
            ESP_LOGE(kUdpTag, "Failed to configure UDP socket %d security", udp_id_);
            return Fail(NetworkError::AtCommandFailed());
        }

        xEventGroupClearBits(event_group_handle_, kOpenResolved | kOpenFailed);
        // For UDP this field is the local port, not TCP's connect timeout.
        // Match upstream's automatic port allocation across new sessions.
        const std::string open = "AT+MIPOPEN=" + std::to_string(udp_id_) + ",\"UDP\",\"" + host +
                                 "\"," + std::to_string(port) + ",,0";
        awaiting_open_.store(true);
        socket_may_exist_.store(true);
        auto open_result = at_uart_->SendCommand(open, kOpenTimeoutMs);
        if (!open_result) {
            awaiting_open_.store(false);
            if (AtCommandCme(open_result) != 0)
                socket_may_exist_.store(false);
            ESP_LOGE(kUdpTag, "Failed to request UDP socket %d open", udp_id_);
            return Fail(open_result.error().ToNetworkError());
        }
        auto bits = xEventGroupWaitBits(event_group_handle_, kOpenResolved | kOpenFailed, pdTRUE,
                                        pdFALSE, pdMS_TO_TICKS(kUdpConnectTimeoutMs));
        awaiting_open_.store(false);
        if (bits & kOpenFailed) {
            ESP_LOGE(kUdpTag, "ML307 rejected UDP socket %d open, error=%d", udp_id_,
                     last_error_.load());
            return Fail(NetworkError::FromMl307Socket(last_error_.load()));
        }
        if (!(bits & kOpenResolved)) {
            ESP_LOGE(kUdpTag, "Timed out opening UDP socket %d", udp_id_);
            return Fail(NetworkError::Timeout());
        }

        connected_state_.store(true);
        instance_active_.store(true);
        connected_ = true;
        ESP_LOGI(kUdpTag, "UDP socket %d connected with inline HEX send", udp_id_);
        return {};
    }

    void Disconnect() override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        connected_ = false;
        connected_state_.store(false);
        pending_send_size_.store(0);
        if (!socket_may_exist_.exchange(false)) {
            return;
        }
        xEventGroupClearBits(event_group_handle_, kCloseResolved);
        expecting_close_.store(true);
        const auto result =
            at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(udp_id_), kCloseTimeoutMs);
        if (!AtCommandSucceeded(result) && AtCommandCme(result) == 0) {
            // A timeout leaves the modem state unknown. Let the destructor or
            // the next Connect() retry instead of forgetting the socket.
            socket_may_exist_.store(true);
            ESP_LOGW(kUdpTag, "Failed to close UDP socket %d", udp_id_);
        }
        expecting_close_.store(false);
    }

    int Send(const std::string& data) override {
        if (data.empty()) {
            return 0;
        }
        if (data.size() > kMaxInlinePayloadBytes) {
            ESP_LOGE(kUdpTag, "UDP datagram too large for inline HEX: %u bytes (max %u)",
                     static_cast<unsigned>(data.size()),
                     static_cast<unsigned>(kMaxInlinePayloadBytes));
            return -1;
        }

        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!connected_state_.load() || !instance_active_.load()) {
            ESP_LOGE(kUdpTag, "UDP socket %d is not connected", udp_id_);
            return -1;
        }

        std::string command =
            "AT+MIPSEND=" + std::to_string(udp_id_) + "," + std::to_string(data.size()) + ",";
        command += "\"";
        command.reserve(command.size() + data.size() * 2 + 4);
        at_uart_->EncodeHexAppend(command, data.data(), data.size());
        command += "\"\r\n";

        pending_send_size_.store(data.size());
        xEventGroupClearBits(event_group_handle_, kSendResolved | kSendFailed);
        const size_t send_timeout =
            TimeoutForBytes(*at_uart_, command.size() + 4, kSendTimeoutFloorMs);
        // One command, one final OK, followed by MIPSEND. There is no binary
        // prompt to miss, so a timeout cannot leave the modem waiting for
        // more bytes; it still invalidates the socket for the next Connect().
        const auto send_result = at_uart_->SendCommand(command, send_timeout, false);
        const bool command_ok = AtCommandSucceeded(send_result);
        EventBits_t bits = 0;
        if (command_ok) {
            bits = xEventGroupWaitBits(event_group_handle_, kSendResolved | kSendFailed, pdTRUE,
                                       pdFALSE, pdMS_TO_TICKS(kSendCompleteTimeoutMs));
        } else {
            bits = xEventGroupWaitBits(event_group_handle_, kSendResolved | kSendFailed, pdFALSE,
                                       pdFALSE, 0) &
                   (kSendResolved | kSendFailed);
        }
        pending_send_size_.store(0);
        if (!command_ok || !(bits & kSendResolved)) {
            connected_state_.store(false);
            ESP_LOGE(kUdpTag, "Inline HEX send failed on UDP socket %d (%u bytes)", udp_id_,
                     static_cast<unsigned>(data.size()));
            if (!command_ok) {
                const auto& error = send_result.error();
                ESP_LOGE(kUdpTag, "AT send result: code=%d cme=%d esp=%d timeout=%u ms",
                         static_cast<int>(error.code), error.cme, static_cast<int>(error.esp),
                         static_cast<unsigned>(send_timeout));
            } else {
                ESP_LOGE(kUdpTag, "AT send acknowledged but completion missing (events=0x%x)",
                         static_cast<unsigned>(bits));
            }
            return -1;
        }
        return static_cast<int>(data.size());
    }

private:
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& args) {
        if (command == "MIPOPEN" && args.size() >= 2 && IsInt(args[0]) &&
            args[0].int_value == udp_id_ && IsInt(args[1])) {
            if (!awaiting_open_.exchange(false)) {
                if (args[1].int_value == 0) {
                    // The caller timed out but the modem completed the open.
                    // Remember it so teardown closes this socket.
                    instance_active_.store(socket_may_exist_.load());
                    ESP_LOGW(kUdpTag, "Late MIPOPEN success for UDP socket %d; teardown required",
                             udp_id_);
                } else {
                    socket_may_exist_.store(false);
                    ESP_LOGW(kUdpTag, "Ignoring stale MIPOPEN failure for UDP socket %d", udp_id_);
                }
                return;
            }
            if (args[1].int_value == 0) {
                connected_state_.store(true);
                instance_active_.store(true);
                socket_may_exist_.store(true);
                xEventGroupSetBits(event_group_handle_, kOpenResolved);
            } else {
                last_error_.store(args[1].int_value);
                connected_state_.store(false);
                instance_active_.store(false);
                socket_may_exist_.store(false);
                xEventGroupSetBits(event_group_handle_, kOpenFailed);
            }
            return;
        }
        if (command == "MIPCLOSE" && !args.empty() && IsInt(args[0]) &&
            args[0].int_value == udp_id_) {
            if (!expecting_close_.exchange(false)) {
                ESP_LOGW(kUdpTag, "Ignoring unsolicited MIPCLOSE for UDP socket %d", udp_id_);
                return;
            }
            connected_state_.store(false);
            instance_active_.store(false);
            socket_may_exist_.store(false);
            pending_send_size_.store(0);
            xEventGroupSetBits(event_group_handle_, kCloseResolved);
            return;
        }
        if (command == "MIPSEND" && args.size() >= 2 && IsInt(args[0]) &&
            args[0].int_value == udp_id_) {
            // Per the ML307 TCP/IP manual, +MIPSEND: <id>,<length> reports the
            // number of bytes accepted by the modem. It is not an error code.
            // A zero length for a non-empty datagram is the only failure the
            // URC itself can express; CME errors are handled by SendCommand().
            const size_t expected = pending_send_size_.exchange(0);
            if (expected == 0) {
                ESP_LOGW(kUdpTag, "Ignoring stale MIPSEND for UDP socket %d", udp_id_);
                return;
            }
            if (!IsInt(args[1]) || args[1].int_value <= 0 ||
                static_cast<size_t>(args[1].int_value) != expected) {
                last_error_.store(-1);
                ESP_LOGW(kUdpTag, "MIPSEND length mismatch on UDP socket %d: %d/%u", udp_id_,
                         IsInt(args[1]) ? args[1].int_value : -1, static_cast<unsigned>(expected));
                connected_state_.store(false);
                xEventGroupSetBits(event_group_handle_, kSendFailed);
            } else {
                xEventGroupSetBits(event_group_handle_, kSendResolved);
            }
            return;
        }
        if (command == "MIPURC" && args.size() >= 2 && IsString(args[0]) && IsInt(args[1]) &&
            args[1].int_value == udp_id_) {
            if (args[0].string_value == "rudp") {
                if (args.size() < 4 || !IsInt(args[2]) || !IsHexValue(args[3]) ||
                    !IsHexString(args[3].string_value) ||
                    args[3].string_value.size() != static_cast<size_t>(args[2].int_value) * 2) {
                    ESP_LOGE(kUdpTag, "Malformed MIPURC rudp for UDP socket %d", udp_id_);
                    return;
                }
                if (connected_state_.load() && message_callback_) {
                    message_callback_(at_uart_->DecodeHex(args[3].string_value));
                }
            } else if (args[0].string_value == "disconn") {
                connected_state_.store(false);
                instance_active_.store(false);
                socket_may_exist_.store(false);
                pending_send_size_.store(0);
                if (args.size() >= 3 && IsInt(args[2])) {
                    last_error_.store(args[2].int_value);
                }
                xEventGroupSetBits(event_group_handle_, kCloseResolved | kOpenFailed | kSendFailed);
            } else {
                ESP_LOGW(kUdpTag, "Unhandled MIPURC %s for UDP socket %d",
                         args[0].string_value.c_str(), udp_id_);
            }
            return;
        }
        if (command == "FIFO_OVERFLOW") {
            // This runs on the AT event task. Never issue MIPCLOSE here: it
            // would wait for that same task and turn a recoverable desync into
            // a deadlock. The next Connect()/destructor closes the socket.
            connected_state_.store(false);
            // Keep instance_active_/socket_may_exist_ set: callback context
            // cannot issue MIPCLOSE, so teardown or the next Connect() must.
            pending_send_size_.store(0);
            xEventGroupSetBits(event_group_handle_, kOpenFailed | kSendFailed | kCloseResolved);
            ESP_LOGW(kUdpTag, "AT FIFO overflow invalidated UDP socket %d", udp_id_);
        }
    }

    std::shared_ptr<AtUart> at_uart_;
    int udp_id_;
    EventGroupHandle_t event_group_handle_ = nullptr;
    std::list<UrcCallback>::iterator urc_callback_it_;
    bool urc_callback_registered_ = false;
    std::mutex io_mutex_;
    std::atomic<bool> connected_state_{false};
    std::atomic<bool> instance_active_{false};
    std::atomic<bool> socket_may_exist_{false};
    std::atomic<bool> expecting_close_{false};
    std::atomic<bool> awaiting_open_{false};
    std::atomic<int> last_error_{0};
    std::atomic<size_t> pending_send_size_{0};
};

// The upstream ML307 TCP/SSL driver also emits an unquoted HEX payload and
// gives the modem only the UART transmission time plus 100 ms. The manual's
// inline form requires a quoted string, and the modem may acknowledge after
// that deadline. This adapter keeps TCP/SSL data on the same bounded command
// path as UDP; TLS itself is still performed by the caller (voice_http.cc).
class DinosaurMl307HexTcp final : public Tcp {
public:
    DinosaurMl307HexTcp(std::shared_ptr<AtUart> at_uart, int tcp_id, bool ssl)
        : at_uart_(std::move(at_uart)), tcp_id_(tcp_id), ssl_(ssl) {
        if (!at_uart_ || !IsValidSocketId(tcp_id_)) {
            ESP_LOGE(kTcpTag, "Invalid ML307 TCP socket id %d", tcp_id_);
            return;
        }
        event_group_handle_ = xEventGroupCreate();
        if (!event_group_handle_) {
            ESP_LOGE(kTcpTag, "Cannot allocate TCP event group");
            return;
        }
        urc_callback_it_ = at_uart_->RegisterUrcCallback(
            [this](const std::string& command, const std::vector<AtArgumentValue>& args) {
                HandleUrc(command, args);
            });
        urc_callback_registered_ = true;
    }

    ~DinosaurMl307HexTcp() override {
        if (urc_callback_registered_) {
            at_uart_->UnregisterUrcCallback(urc_callback_it_);
            urc_callback_registered_ = false;
        }
        connected_ = false;
        connected_state_.store(false);
        if (socket_may_exist_.exchange(false)) {
            const auto close_result =
                at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(tcp_id_), kCloseTimeoutMs);
            if (!AtCommandSucceeded(close_result)) {
                ESP_LOGW(kTcpTag, "Failed to close TCP socket %d during teardown", tcp_id_);
            }
        }
        if (event_group_handle_) {
            vEventGroupDelete(event_group_handle_);
        }
    }

    NetworkResult<> Connect(const std::string& host, int port) override {
        if (!event_group_handle_ || !at_uart_ || !IsSafeEndpoint(host, port)) {
            ESP_LOGE(kTcpTag, "Invalid TCP endpoint or socket state");
            return Fail(NetworkError::InvalidArgument());
        }

        std::lock_guard<std::mutex> lock(io_mutex_);
        connected_ = false;
        connected_state_.store(false);
        instance_active_.store(false);
        pending_send_size_.store(0);
        xEventGroupClearBits(event_group_handle_, kOpenResolved | kOpenFailed | kCloseResolved |
                                                      kSendResolved | kSendFailed);

        // Best-effort cleanup. An inactive id answers CME (harmless); a
        // timeout is resynchronised with a plain AT probe before a new socket
        // is opened. Track an uncertain modem-side socket so a later
        // destructor can still reclaim it.
        socket_may_exist_.store(true);
        expecting_close_.store(true);
        const auto close_result =
            at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(tcp_id_), kCloseTimeoutMs);
        const bool close_acked = AtCommandSucceeded(close_result);
        bool close_confirmed = close_acked;
        if (close_acked) {
            xEventGroupWaitBits(event_group_handle_, kCloseResolved, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(kCloseUrcTimeoutMs));
        } else if (AtCommandCme(close_result) == 0) {
            close_confirmed = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            auto probe_result = at_uart_->SendCommand("AT", 1000);
            if (!probe_result) {
                expecting_close_.store(false);
                ESP_LOGE(kTcpTag, "Cannot resynchronise AT parser after TCP cleanup");
                return Fail(probe_result.error().ToNetworkError());
            }
        } else {
            // A CME proves that the modem answered and no socket exists.
            close_confirmed = true;
        }
        expecting_close_.store(false);
        socket_may_exist_.store(!close_confirmed);
        xEventGroupClearBits(event_group_handle_, kCloseResolved | kOpenResolved | kOpenFailed);
        connected_state_.store(false);
        instance_active_.store(false);

        if (ssl_ && !at_uart_->SendCommand("AT+MSSLCFG=\"auth\",0,0", kConfigTimeoutMs)) {
            ESP_LOGE(kTcpTag, "Failed to configure modem TLS authentication");
            return Fail(NetworkError::AtCommandFailed());
        }
        if (!at_uart_->SendCommand("AT+MIPCFG=\"encoding\"," + std::to_string(tcp_id_) + ",1,1",
                                   kConfigTimeoutMs) ||
            !at_uart_->SendCommand(
                "AT+MIPCFG=\"ssl\"," + std::to_string(tcp_id_) + "," + (ssl_ ? "1" : "0") + ",0",
                kConfigTimeoutMs)) {
            ESP_LOGE(kTcpTag, "Failed to configure TCP socket %d", tcp_id_);
            return Fail(NetworkError::AtCommandFailed());
        }

        xEventGroupClearBits(event_group_handle_, kOpenResolved | kOpenFailed);
        const std::string open = "AT+MIPOPEN=" + std::to_string(tcp_id_) + ",\"TCP\",\"" + host +
                                 "\"," + std::to_string(port) + "," +
                                 std::to_string(kTcpModemOpenTimeoutSeconds) + ",0";
        awaiting_open_.store(true);
        socket_may_exist_.store(true);
        auto open_result = at_uart_->SendCommand(open, kOpenTimeoutMs);
        if (!open_result) {
            awaiting_open_.store(false);
            if (AtCommandCme(open_result) != 0)
                socket_may_exist_.store(false);
            ESP_LOGE(kTcpTag, "Failed to request TCP socket %d open", tcp_id_);
            return Fail(open_result.error().ToNetworkError());
        }
        auto bits = xEventGroupWaitBits(event_group_handle_, kOpenResolved | kOpenFailed, pdTRUE,
                                        pdFALSE, pdMS_TO_TICKS(kTcpConnectTimeoutMs));
        awaiting_open_.store(false);
        if (bits & kOpenFailed) {
            ESP_LOGE(kTcpTag, "ML307 rejected TCP socket %d open, error=%d", tcp_id_,
                     last_error_.load());
            return Fail(NetworkError::FromMl307Socket(last_error_.load()));
        }
        if (!(bits & kOpenResolved)) {
            ESP_LOGE(kTcpTag, "Timed out opening TCP socket %d", tcp_id_);
            return Fail(NetworkError::Timeout());
        }

        connected_ = true;
        connected_state_.store(true);
        instance_active_.store(true);
        ESP_LOGI(kTcpTag, "TCP socket %d connected with inline HEX send", tcp_id_);
        return {};
    }

    void Disconnect() override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        connected_ = false;
        connected_state_.store(false);
        pending_send_size_.store(0);
        if (!socket_may_exist_.exchange(false)) {
            return;
        }
        xEventGroupClearBits(event_group_handle_, kCloseResolved);
        expecting_close_.store(true);
        const auto result =
            at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(tcp_id_), kCloseTimeoutMs);
        if (!AtCommandSucceeded(result) && AtCommandCme(result) == 0) {
            // A timeout leaves the modem state unknown. Let the destructor or
            // the next Connect() retry instead of forgetting the socket.
            socket_may_exist_.store(true);
            ESP_LOGW(kTcpTag, "Failed to close TCP socket %d", tcp_id_);
        }
        expecting_close_.store(false);
    }

    int Send(const std::string& data) override {
        if (data.empty())
            return 0;
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!connected_state_.load() || !instance_active_.load()) {
            ESP_LOGE(kTcpTag, "TCP socket %d is not connected", tcp_id_);
            return -1;
        }

        size_t total_sent = 0;
        while (total_sent < data.size()) {
            const size_t chunk_size = std::min(data.size() - total_sent, kMaxInlinePayloadBytes);
            std::string command =
                "AT+MIPSEND=" + std::to_string(tcp_id_) + "," + std::to_string(chunk_size) + ",\"";
            command.reserve(command.size() + chunk_size * 2 + 4);
            at_uart_->EncodeHexAppend(command, data.data() + total_sent, chunk_size);
            command += "\"\r\n";

            pending_send_size_.store(chunk_size);
            xEventGroupClearBits(event_group_handle_, kSendResolved | kSendFailed);
            const size_t send_timeout =
                TimeoutForBytes(*at_uart_, command.size() + 4, kSendTimeoutFloorMs);
            const bool command_ok =
                AtCommandSucceeded(at_uart_->SendCommand(command, send_timeout, false));
            EventBits_t bits = 0;
            if (command_ok) {
                bits = xEventGroupWaitBits(event_group_handle_, kSendResolved | kSendFailed, pdTRUE,
                                           pdFALSE, pdMS_TO_TICKS(kSendCompleteTimeoutMs));
            } else {
                bits = xEventGroupWaitBits(event_group_handle_, kSendResolved | kSendFailed,
                                           pdFALSE, pdFALSE, 0) &
                       (kSendResolved | kSendFailed);
            }
            pending_send_size_.store(0);
            if (!command_ok || !(bits & kSendResolved)) {
                connected_state_.store(false);
                ESP_LOGE(kTcpTag, "Inline HEX send failed on TCP socket %d (%u bytes)", tcp_id_,
                         static_cast<unsigned>(chunk_size));
                return -1;
            }
            total_sent += chunk_size;
        }
        return static_cast<int>(total_sent);
    }

private:
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& args) {
        if (command == "MIPOPEN" && args.size() >= 2 && IsInt(args[0]) &&
            args[0].int_value == tcp_id_ && IsInt(args[1])) {
            if (!awaiting_open_.exchange(false)) {
                if (args[1].int_value == 0) {
                    // The caller timed out but the modem completed the open.
                    // Remember it so teardown closes this socket.
                    instance_active_.store(socket_may_exist_.load());
                    ESP_LOGW(kTcpTag, "Late MIPOPEN success for TCP socket %d; teardown required",
                             tcp_id_);
                } else {
                    socket_may_exist_.store(false);
                    ESP_LOGW(kTcpTag, "Ignoring stale MIPOPEN failure for TCP socket %d", tcp_id_);
                }
                return;
            }
            if (args[1].int_value == 0) {
                connected_state_.store(true);
                instance_active_.store(true);
                socket_may_exist_.store(true);
                xEventGroupSetBits(event_group_handle_, kOpenResolved);
            } else {
                last_error_.store(args[1].int_value);
                connected_state_.store(false);
                instance_active_.store(false);
                socket_may_exist_.store(false);
                xEventGroupSetBits(event_group_handle_, kOpenFailed);
            }
            return;
        }
        if (command == "MIPCLOSE" && !args.empty() && IsInt(args[0]) &&
            args[0].int_value == tcp_id_) {
            if (!expecting_close_.exchange(false)) {
                ESP_LOGW(kTcpTag, "Ignoring unsolicited MIPCLOSE for TCP socket %d", tcp_id_);
                return;
            }
            connected_state_.store(false);
            instance_active_.store(false);
            socket_may_exist_.store(false);
            pending_send_size_.store(0);
            xEventGroupSetBits(event_group_handle_, kCloseResolved);
            return;
        }
        if (command == "MIPSEND" && args.size() >= 2 && IsInt(args[0]) &&
            args[0].int_value == tcp_id_) {
            const size_t expected = pending_send_size_.exchange(0);
            if (expected == 0) {
                ESP_LOGW(kTcpTag, "Ignoring stale MIPSEND for TCP socket %d", tcp_id_);
                return;
            }
            if (!IsInt(args[1]) || args[1].int_value <= 0 ||
                static_cast<size_t>(args[1].int_value) != expected) {
                last_error_.store(-1);
                ESP_LOGW(kTcpTag, "MIPSEND length mismatch on TCP socket %d: %d/%u", tcp_id_,
                         IsInt(args[1]) ? args[1].int_value : -1, static_cast<unsigned>(expected));
                connected_state_.store(false);
                xEventGroupSetBits(event_group_handle_, kSendFailed);
            } else {
                xEventGroupSetBits(event_group_handle_, kSendResolved);
            }
            return;
        }
        if (command == "MIPURC" && args.size() >= 2 && IsString(args[0]) && IsInt(args[1]) &&
            args[1].int_value == tcp_id_) {
            if (args[0].string_value == "rtcp") {
                if (args.size() < 4 || !IsInt(args[2]) || !IsHexValue(args[3]) ||
                    !IsHexString(args[3].string_value) ||
                    args[3].string_value.size() != static_cast<size_t>(args[2].int_value) * 2) {
                    ESP_LOGE(kTcpTag, "Malformed MIPURC rtcp for TCP socket %d", tcp_id_);
                    return;
                }
                if (connected_state_.load() && stream_callback_)
                    stream_callback_(at_uart_->DecodeHex(args[3].string_value));
            } else if (args[0].string_value == "disconn") {
                const bool was_connected = connected_state_.exchange(false);
                instance_active_.store(false);
                socket_may_exist_.store(false);
                pending_send_size_.store(0);
                if (args.size() >= 3 && IsInt(args[2]))
                    last_error_.store(args[2].int_value);
                xEventGroupSetBits(event_group_handle_, kCloseResolved | kOpenFailed | kSendFailed);
                if (was_connected && disconnect_callback_)
                    disconnect_callback_();
            } else {
                ESP_LOGW(kTcpTag, "Unhandled MIPURC %s for TCP socket %d",
                         args[0].string_value.c_str(), tcp_id_);
            }
            return;
        }
        if (command == "FIFO_OVERFLOW") {
            // Never issue an AT command from this callback: it runs on the AT
            // receive task. Keep the socket tracked so the next
            // Connect()/destructor closes it.
            connected_state_.store(false);
            pending_send_size_.store(0);
            xEventGroupSetBits(event_group_handle_, kOpenFailed | kSendFailed | kCloseResolved);
            ESP_LOGW(kTcpTag, "AT FIFO overflow invalidated TCP socket %d", tcp_id_);
        }
    }

    std::shared_ptr<AtUart> at_uart_;
    int tcp_id_;
    bool ssl_;
    EventGroupHandle_t event_group_handle_ = nullptr;
    std::list<UrcCallback>::iterator urc_callback_it_;
    bool urc_callback_registered_ = false;
    std::mutex io_mutex_;
    std::atomic<bool> connected_state_{false};
    std::atomic<bool> instance_active_{false};
    std::atomic<bool> socket_may_exist_{false};
    std::atomic<bool> expecting_close_{false};
    std::atomic<bool> awaiting_open_{false};
    std::atomic<int> last_error_{0};
    std::atomic<size_t> pending_send_size_{0};
};

std::unique_ptr<Udp> CreateInlineHexUdp(NetworkInterface* delegate,
                                        const std::shared_ptr<AtUart>& at_uart, int connect_id) {
    if (!delegate || !at_uart || connect_id < 0) {
        return nullptr;
    }
    return std::make_unique<DinosaurMl307HexUdp>(at_uart, connect_id);
}

std::unique_ptr<Tcp> CreateInlineHexTcp(NetworkInterface* delegate,
                                        const std::shared_ptr<AtUart>& at_uart, int connect_id,
                                        bool ssl) {
    if (!delegate || !at_uart || connect_id < 0)
        return nullptr;
    return std::make_unique<DinosaurMl307HexTcp>(at_uart, connect_id, ssl);
}
}  // namespace

DinosaurMl307Network::DinosaurMl307Network(NetworkInterface* delegate,
                                           std::shared_ptr<AtUart> at_uart)
    : delegate_(delegate), at_uart_(std::move(at_uart)) {}

std::unique_ptr<Http> DinosaurMl307Network::CreateHttp(int connect_id) {
    (void)connect_id;
    if (!at_uart_)
        return nullptr;
    return std::make_unique<DinosaurMl307Http>(at_uart_);
}

std::unique_ptr<Tcp> DinosaurMl307Network::CreateTcp(int connect_id) {
    return CreateInlineHexTcp(delegate_, at_uart_, connect_id, false);
}

std::unique_ptr<Tcp> DinosaurMl307Network::CreateSsl(int connect_id) {
    return CreateInlineHexTcp(delegate_, at_uart_, connect_id, true);
}

std::unique_ptr<Udp> DinosaurMl307Network::CreateUdp(int connect_id) {
    return CreateInlineHexUdp(delegate_, at_uart_, connect_id);
}

std::unique_ptr<Mqtt> DinosaurMl307Network::CreateMqtt(int connect_id) {
    if (!at_uart_ || connect_id < 0)
        return nullptr;
    return std::make_unique<DinosaurMl307Mqtt>(at_uart_, connect_id);
}

std::unique_ptr<WebSocket> DinosaurMl307Network::CreateWebSocket(int connect_id) {
    if (!at_uart_ || connect_id < 0)
        return nullptr;
    // Construct against this adapter so the WebSocket client uses the
    // inline-HEX TCP/TLS implementation above rather than the component
    // modem's binary-prompt transport.
    return std::make_unique<WebSocket>(this, connect_id);
}

#endif
