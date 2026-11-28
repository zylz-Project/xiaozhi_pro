#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>
#include <atomic>
#include <mutex>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT (1 << 1)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    void OnNetworkLost() override { CloseAudioChannel(false); }
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    mutable std::mutex channel_mutex_;
    std::mutex open_mutex_;
    std::unique_ptr<WebSocket> websocket_;
    bool closing_ = false;
    std::atomic<uint32_t> channel_generation_{0};
    std::atomic<bool> channel_ready_{false};
    int version_ = 1;

    void HandleBinaryAudio(const char* data, size_t len);
    bool ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
