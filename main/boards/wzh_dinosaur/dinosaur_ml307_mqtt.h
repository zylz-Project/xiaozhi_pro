#ifndef WZH_DINOSAUR_ML307_MQTT_H_
#define WZH_DINOSAUR_ML307_MQTT_H_

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <at_uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt.h>

// ML307 MQTT transport with bounded resynchronisation and strict URC parsing.
// The upstream driver queries AT+MQTTSTATE on every Connect()/IsConnected()
// call and treats every state except "disconnected" as connected. That both
// misclassifies the "connecting" state and turns a transient AT timeout into a
// 10 second recursive wait. This adapter trusts connection URCs, uses cached
// state for IsConnected(), and only queries MQTTSTATE once as a fallback.
class DinosaurMl307Mqtt final : public Mqtt {
public:
    DinosaurMl307Mqtt(std::shared_ptr<AtUart> at_uart, int mqtt_id);
    ~DinosaurMl307Mqtt() override;

    NetworkResult<> Connect(const std::string broker_address, int broker_port,
                            const std::string client_id, const std::string username,
                            const std::string password) override;
    void Disconnect() override;
    bool Publish(const std::string topic, const std::string payload, int qos = 0) override;
    bool Subscribe(const std::string topic, int qos = 0) override;
    bool Unsubscribe(const std::string topic) override;
    bool IsConnected() override;

private:
    bool Configure(const std::string& command);
    bool QueryState(int timeout_ms);
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments);
    void SetConnected(bool connected, bool notify);
    void ResetIncomingLocked();
    static std::string ErrorToString(int code);

    std::shared_ptr<AtUart> at_uart_;
    int mqtt_id_;
    EventGroupHandle_t event_group_handle_ = nullptr;
    std::list<UrcCallback>::iterator urc_callback_it_;
    std::mutex connect_mutex_;
    std::mutex publish_mutex_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> session_possible_{false};
    std::atomic<bool> state_query_pending_{false};
    std::atomic<int> last_error_{0};
    std::atomic<int> queried_state_{-1};
    std::atomic<size_t> pending_publish_size_{0};
    std::atomic<int> publish_accepted_size_{-1};
    // Each explicit Connect()/Disconnect() starts a new session serial. A
    // conn,2 URC is only allowed to tear down the session when it matches the
    // serial of the disconnect that produced it; late cleanup URCs from an
    // older session must not kill a newer connection.
    std::atomic<uint32_t> session_serial_{0};
    std::atomic<uint32_t> pending_cleanup_serial_{0};
    // Reassembly state is shared with Connect()/Disconnect() on other tasks.
    std::mutex message_mutex_;
    std::string message_payload_;
    int message_id_ = -1;
    std::string message_topic_;
    size_t message_expected_ = 0;
};

#endif  // WZH_DINOSAUR_ML307_MQTT_H_
