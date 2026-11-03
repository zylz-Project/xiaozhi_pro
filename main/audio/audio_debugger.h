#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

class AudioDebugger {
public:
    AudioDebugger();
    ~AudioDebugger();

    void Feed(const std::vector<int16_t>& data, int sample_rate = 16000, int channels = 1);
    void FeedProcessed(const std::vector<int16_t>& data, bool vad);

private:
    bool EnsureSocket();
    void Send(const std::vector<int16_t>& data, int sample_rate, int channels, int stream,
              bool vad);
    std::atomic<int> udp_sockfd_{-1};
    struct sockaddr_in udp_server_addr_{};
    bool address_valid_ = false;
    std::mutex socket_mutex_;
    std::atomic<uint32_t> sequences_[2]{};
    std::atomic<uint32_t> positions_[2]{};
    std::atomic<int64_t> started_us_{-1};
    std::atomic<bool> finished_{false};
};

#endif
