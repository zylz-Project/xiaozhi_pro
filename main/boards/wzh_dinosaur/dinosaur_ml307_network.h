#ifndef WZH_DINOSAUR_ML307_NETWORK_H_
#define WZH_DINOSAUR_ML307_NETWORK_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <network_interface.h>
#include <array>
#include <memory>
#include <mutex>

class AtUart;
class DinosaurCellular;

// The upstream ML307 UDP driver sends through the two-stage
// AT+MIPSEND -> '>' -> binary flow. Its first stage has a one-second timeout,
// and a late response can leave the AT parser out of sync. ML307 also supports
// a single inline HEX command, so the UDP audio socket is replaced here. TCP
// and SSL use the same quoted form because the upstream driver omits the
// quotes and gives the modem only a 100 ms processing margin. MQTT is also
// replaced by the board adapter because the upstream driver queries MQTTSTATE
// on every connection check and uses a fixed three-second timeout. HTTP and
// WebSocket also stay in this adapter: HTTP needs bounded streaming and strict
// URC validation, while WebSocket must build on the reliable inline-HEX TCP
// transport instead of the upstream binary-prompt path.
class DinosaurMl307Network : public NetworkInterface {
public:
    DinosaurMl307Network(const DinosaurCellular& cellular, std::shared_ptr<AtUart> at_uart);
    ~DinosaurMl307Network() override = default;

    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateTcp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateSsl(int connect_id = -1) override;
    std::unique_ptr<Udp> CreateUdp(int connect_id = -1) override;
    std::unique_ptr<Mqtt> CreateMqtt(int connect_id = -1) override;
    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id = -1) override;

    static constexpr int kSocketCount = 6;
    static constexpr EventBits_t kSocketInterrupted = BIT6;
    bool ClaimSocket(int id, EventGroupHandle_t events);
    void ReleaseSocket(int id);
    void InterruptSockets();

private:
    const DinosaurCellular& cellular_;
    std::shared_ptr<AtUart> at_uart_;
    std::mutex socket_mutex_;
    std::array<EventGroupHandle_t, kSocketCount> socket_events_{};
};

#endif
