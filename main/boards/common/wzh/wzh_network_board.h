#pragma once

#include "wifi_board.h"

#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC
#include <atomic>
#include <memory>
class DinosaurCellular;
#endif

namespace wzh {

// The three products use the same JP1 ML307 connector on the V4-2.2 PCB.
class NetworkBoard : public WifiBoard {
protected:
    virtual bool IsNetworkShuttingDown() const = 0;

#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC
public:
    NetworkBoard();
    ~NetworkBoard() override;
    void StartNetwork() override;
    void SetNetworkEventCallback(NetworkEventCallback callback) override;
    bool DeferWifiStationStart() const override;
    bool UsesCellularNetwork() const override;
    bool HasCellularModem() const override;
    bool HasSavedCellularNetwork() const override;
    bool ClearNetworkPreference() override;
    size_t GetProtocolPayloadLimit() const override;
    bool IsNetworkAvailable() const override;
    NetworkInterface* GetNetwork() override;
    const char* GetNetworkStateIcon() override;
    const char* ConnectCellularNetwork() override;
    bool BeginWifiNetworkTrial() override;
    bool FinishWifiNetworkTrial(bool connected) override;

private:
    enum class NetworkChoice : uint8_t { kWifi, kCellular };
    static bool LoadCellularPreference();
    bool SaveCellularPreference(bool cellular);
    bool BeginTransportTrial();
    void CompleteTransportTrial(NetworkChoice choice);
    void ForwardNetworkEvent(NetworkEvent event, const std::string& data = {});
    void FallbackToWifi();

    std::atomic<NetworkChoice> network_{NetworkChoice::kWifi};
    std::atomic<bool> transport_trial_{false};
    std::atomic<bool> cellular_saved_{false};
    std::unique_ptr<DinosaurCellular> cellular_;
    NetworkEventCallback cloud_network_callback_;
    bool cellular_connected_reported_ = false;  // Application task only.
#endif
};

}  // namespace wzh
