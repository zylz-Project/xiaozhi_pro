#include "audio_debugger.h"

#include "sdkconfig.h"

#if CONFIG_USE_AUDIO_DEBUGGER
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <string_view>
#endif

#define TAG "AudioDebugger"

AudioDebugger::AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    // Parse without exceptions. Audio initialization precedes Wi-Fi startup,
    // so socket creation waits until the network stack has an interface.
    const std::string_view address(CONFIG_AUDIO_DEBUG_UDP_SERVER);
    const auto colon = address.find(':');
    unsigned port = 0;
    if (colon == std::string_view::npos || colon == 0 || colon >= INET_ADDRSTRLEN) {
        return;
    }
    const auto result =
        std::from_chars(address.data() + colon + 1, address.data() + address.size(), port);
    if (result.ec != std::errc{} || result.ptr != address.data() + address.size() || port == 0 ||
        port > 65535) {
        return;
    }
    std::array<char, INET_ADDRSTRLEN> ip{};
    std::memcpy(ip.data(), address.data(), colon);
    udp_server_addr_.sin_family = AF_INET;
    udp_server_addr_.sin_port = htons(static_cast<uint16_t>(port));
    address_valid_ = inet_pton(AF_INET, ip.data(), &udp_server_addr_.sin_addr) == 1;
#endif
}

AudioDebugger::~AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    const int fd = udp_sockfd_.load();
    if (fd >= 0) {
        close(fd);
    }
#endif
}

bool AudioDebugger::EnsureSocket() {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (!address_valid_ || esp_netif_get_default_netif() == nullptr) {
        return false;
    }
    if (udp_sockfd_.load() >= 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (udp_sockfd_.load() < 0) {
        udp_sockfd_.store(socket(AF_INET, SOCK_DGRAM, 0));
    }
    return udp_sockfd_.load() >= 0;
#else
    return false;
#endif
}

void AudioDebugger::Feed(const std::vector<int16_t>& data, int sample_rate, int channels) {
    Send(data, sample_rate, channels, 0, false);
}

void AudioDebugger::FeedProcessed(const std::vector<int16_t>& data, bool vad) {
#if CONFIG_AUDIO_DEBUG_PACKETIZED
    Send(data, 16000, 1, 1, vad);
#else
    (void)data;
    (void)vad;
#endif
}

void AudioDebugger::Send(const std::vector<int16_t>& data, int sample_rate, int channels,
                         int stream, bool vad) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (data.empty() || channels < 1 || channels > 2 || sample_rate < 1 ||
        data.size() % channels != 0 || !EnsureSocket()) {
        return;
    }
    const int fd = udp_sockfd_.load();
#if CONFIG_AUDIO_DEBUG_PACKETIZED
    const int64_t now = esp_timer_get_time();
    const int64_t started = started_us_.load();
    if (started >= 0 && now - started >= int64_t(CONFIG_AUDIO_DEBUG_DURATION_SECONDS) * 1000000) {
        if (!finished_.exchange(true)) {
            ESP_LOGI(TAG, "Timed audio capture complete");
        }
        return;
    }
    constexpr size_t kHeaderBytes = 32, kPayloadBytes = 640;
    std::array<uint8_t, kHeaderBytes + kPayloadBytes> packet;
    auto put = [&packet](size_t offset, uint64_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            packet[offset + i] = static_cast<uint8_t>(value >> (i * 8));
        }
    };
    std::memcpy(packet.data(), "XDBG", 4);
    packet[4] = 1;
    packet[5] = stream;
    packet[6] = channels;
    packet[7] = vad ? 1 : 0;
    for (size_t offset = 0; offset < data.size();) {
        const size_t count = std::min(data.size() - offset, kPayloadBytes / sizeof(int16_t));
        put(8, sequences_[stream].fetch_add(1), 4);
        put(12, sample_rate, 4);
        put(16, positions_[stream].fetch_add(count / channels), 4);
        put(20, now, 8);
        put(28, count / channels, 2);
        put(30, count * sizeof(int16_t), 2);
        std::memcpy(packet.data() + kHeaderBytes, data.data() + offset, count * sizeof(int16_t));
        const auto sent =
            sendto(fd, packet.data(), kHeaderBytes + count * sizeof(int16_t), MSG_DONTWAIT,
                   reinterpret_cast<const sockaddr*>(&udp_server_addr_), sizeof(udp_server_addr_));
        if (sent >= 0) {
            int64_t expected = -1;
            if (started_us_.compare_exchange_strong(expected, now)) {
                ESP_LOGI(TAG, "Timed audio capture started (%d seconds)",
                         CONFIG_AUDIO_DEBUG_DURATION_SECONDS);
            }
        }
        offset += count;
    }
#else
    (void)stream;
    (void)vad;
    sendto(fd, data.data(), data.size() * sizeof(int16_t), MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&udp_server_addr_), sizeof(udp_server_addr_));
#endif
#else
    (void)data;
    (void)sample_rate;
    (void)channels;
    (void)stream;
    (void)vad;
#endif
}
