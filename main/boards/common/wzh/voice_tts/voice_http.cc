#include "voice_http.h"
#include "voice_at_buffer.h"

#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC && CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "network_interface.h"

namespace voice_tts {
namespace {
constexpr char kTag[] = "VoiceAtHttps";
// Bound the URC callback's memory use. Never block the modem's receive task on playback.
constexpr size_t kReceiveCapacity = 1024 * 1024;
constexpr int kSocketId = 3;
std::atomic<bool> socket_in_use{false};

class AtTls {
public:
    AtTls() {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&config_);
    }
    ~AtTls() {
        // Unregister modem callbacks before releasing their context.
        tcp_.reset();
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        free(buffer_);
        if (leased_)
            socket_in_use.store(false);
    }
    int Connect(const char* host, int port, int timeout_ms) {
        bool expected = false;
        if (!socket_in_use.compare_exchange_strong(expected, true)) {
            ESP_LOGW(kTag, "Voice connection already in use");
            return -1;
        }
        leased_ = true;
        buffer_ = static_cast<unsigned char*>(
            heap_caps_malloc(kReceiveCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buffer_) {
            ESP_LOGE(kTag, "No memory for bounded AT receive buffer");
            return -1;
        }
        receive_.Init(buffer_, kReceiveCapacity);
        auto* network = Board::GetInstance().GetNetwork();
        tcp_ = network ? network->CreateTcp(kSocketId) : nullptr;
        if (!tcp_)
            return -1;
        tcp_->OnStream([this](const std::string& data) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failed_ || !receive_.Push(data.data(), data.size())) {
                failed_ = true;
                cv_.notify_all();
                return;
            }
            cv_.notify_all();
        });
        tcp_->OnDisconnected([this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            cv_.notify_all();
        });
        const int64_t tcp_start = esp_timer_get_time();
        if (!tcp_->Connect(host, port)) {
            ESP_LOGW(kTag, "AT TCP connect failed");
            return -1;
        }
        ESP_LOGI(kTag, "TCP connected: elapsed_ms=%u",
                 static_cast<unsigned>((esp_timer_get_time() - tcp_start) / 1000));
        if (mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
            esp_crt_bundle_attach(&config_) != ESP_OK) {
            ESP_LOGE(kTag, "TLS configuration failed");
            return -1;
        }
        mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ssl_conf_rng(
            &config_,
            [](void*, unsigned char* out, size_t size) {
                esp_fill_random(out, size);
                return 0;
            },
            nullptr);
#endif
        if (mbedtls_ssl_setup(&ssl_, &config_) != 0 || mbedtls_ssl_set_hostname(&ssl_, host) != 0)
            return -1;
        mbedtls_ssl_set_bio(&ssl_, this, Send, Receive, nullptr);
        SetDeadline(timeout_ms);
        const int64_t tls_start = esp_timer_get_time();
        int rc;
        do {
            rc = mbedtls_ssl_handshake(&ssl_);
        } while ((rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                 Remaining() > 0);
        const uint32_t verify_flags = mbedtls_ssl_get_verify_result(&ssl_);
        if (rc != 0 || verify_flags != 0) {
            ESP_LOGW(kTag, "TLS handshake failed: result=%d verify_flags=0x%lx", rc,
                     static_cast<unsigned long>(verify_flags));
            return -1;
        }
        ESP_LOGI(kTag, "TLS verified: elapsed_ms=%u",
                 static_cast<unsigned>((esp_timer_get_time() - tls_start) / 1000));
        return 0;
    }
    int Read(char* out, int len, int timeout_ms) {
        SetDeadline(timeout_ms);
        int rc;
        do {
            rc = mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char*>(out), len);
        } while ((rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                 Remaining() > 0);
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (rc < 0) {
            ESP_LOGW(kTag, "TLS read failed: -0x%x", -rc);
            errno = ETIMEDOUT;
            return -1;
        }
        return rc;
    }
    int Write(const char* data, int len, int timeout_ms) {
        SetDeadline(timeout_ms);
        int rc;
        do {
            rc = mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char*>(data), len);
        } while ((rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                 Remaining() > 0);
        if (rc < 0) {
            ESP_LOGW(kTag, "TLS write failed: -0x%x", -rc);
            return -1;
        }
        return rc;
    }
    int PollRead(int timeout_ms) {
        if (mbedtls_ssl_get_bytes_avail(&ssl_) > 0)
            return 1;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
                     [this] { return receive_.Size() || closed_ || failed_; });
        return failed_ ? -1 : (receive_.Size() || closed_ ? 1 : 0);
    }
    int Close() {
        tcp_.reset();
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
        return 0;
    }

private:
    void SetDeadline(int timeout_ms) {
        deadline_ = esp_timer_get_time() + static_cast<int64_t>(std::max(0, timeout_ms)) * 1000;
    }
    int64_t Remaining() const {
        return std::max<int64_t>(0, (deadline_ - esp_timer_get_time() + 999) / 1000);
    }
    static int Send(void* ctx, const unsigned char* data, size_t len) {
        auto* self = static_cast<AtTls*>(ctx);
        if (!self->tcp_ || self->Remaining() <= 0)
            return MBEDTLS_ERR_NET_SEND_FAILED;
        const size_t chunk = std::min<size_t>(len, 730);
        int sent = self->tcp_->Send(std::string(reinterpret_cast<const char*>(data), chunk));
        return sent > 0 ? sent : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int Receive(void* ctx, unsigned char* out, size_t len) {
        auto* self = static_cast<AtTls*>(ctx);
        std::unique_lock<std::mutex> lock(self->mutex_);
        self->cv_.wait_for(lock, std::chrono::milliseconds(self->Remaining()), [self] {
            return self->receive_.Size() || self->closed_ || self->failed_;
        });
        if (self->failed_) {
            ESP_LOGE(kTag, "AT receive buffer overflow; aborting response");
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        if (!self->receive_.Size())
            return self->closed_ ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
        return static_cast<int>(self->receive_.Pop(out, len));
    }
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config config_;
    std::unique_ptr<Tcp> tcp_;
    unsigned char* buffer_ = nullptr;
    VoiceAtBuffer receive_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int64_t deadline_ = 0;
    bool failed_ = false, closed_ = false, leased_ = false;
};
AtTls* Context(esp_transport_handle_t t) {
    return static_cast<AtTls*>(esp_transport_get_context_data(t));
}
}  // namespace

void VoiceHttpDelete::operator()(esp_http_client* client) const {
    if (client)
        esp_http_client_cleanup(client);
    if (transport)
        esp_transport_destroy(static_cast<esp_transport_handle_t>(transport));
}

VoiceHttpClient CreateVoiceHttpClient(const esp_http_client_config_t& original) {
    auto& board = Board::GetInstance();
    if (!board.UsesCellularNetwork())
        return VoiceHttpClient(esp_http_client_init(&original), VoiceHttpDelete{});
    if (!board.IsNetworkAvailable() || !original.url || strncmp(original.url, "https://", 8) != 0)
        return {};
    auto transport = esp_transport_init();
    if (!transport)
        return {};
    auto* context = new (std::nothrow) AtTls();
    if (!context) {
        esp_transport_destroy(transport);
        return {};
    }
    esp_transport_set_context_data(transport, context);
    esp_transport_set_func(
        transport,
        [](esp_transport_handle_t t, const char* host, int port, int timeout) {
            return Context(t)->Connect(host, port, timeout);
        },
        [](esp_transport_handle_t t, char* data, int len, int timeout) {
            return Context(t)->Read(data, len, timeout);
        },
        [](esp_transport_handle_t t, const char* data, int len, int timeout) {
            return Context(t)->Write(data, len, timeout);
        },
        [](esp_transport_handle_t t) { return Context(t)->Close(); },
        [](esp_transport_handle_t t, int timeout) { return Context(t)->PollRead(timeout); },
        [](esp_transport_handle_t, int) { return 1; },
        [](esp_transport_handle_t t) {
            delete Context(t);
            return 0;
        });
    auto config = original;
    config.transport = transport;
    auto* client = esp_http_client_init(&config);
    if (!client) {
        esp_transport_destroy(transport);
        return {};
    }
    return VoiceHttpClient(client, VoiceHttpDelete{transport});
}
bool VoiceHttpNetworkAvailable() {
    auto& board = Board::GetInstance();
    return board.UsesCellularNetwork() ? board.IsNetworkAvailable() : board.HasSystemNetwork();
}
}  // namespace voice_tts
#endif
