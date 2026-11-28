#ifndef WZH_DINOSAUR_CELLULAR_H_
#define WZH_DINOSAUR_CELLULAR_H_

#include <sdkconfig.h>
#include "config.h"
#if DINOSAUR_ENABLE_4G
#include <at_modem.h>
#include <freertos/event_groups.h>
#include <atomic>
#include <memory>
#include <mutex>
#include "board.h"

// The modem outlives its network clients and the board. All blocking AT work
// stays on the network task; receive callbacks only signal a change.
class DinosaurCellular {
public:
    enum class ProbeState { kNotChecked, kProbing, kDetected, kNoResponse, kUnavailable };
    using DetectionCallback = std::function<void(bool)>;
    bool Start(NetworkEventCallback callback, DetectionCallback detection = {});
    // Pause/resume registration without destroying the detected modem.
    void SetPaused(bool paused);
    bool IsReady() const {
        return ready_.load() && !paused_.load() &&
               ready_generation_.load() == lost_generation_.load();
    }
    ProbeState GetProbeState() const { return probe_state_.load(); }
    bool HasDetectedModem() const { return GetProbeState() == ProbeState::kDetected; }
    bool ProbeInProgress() const { return GetProbeState() == ProbeState::kProbing; }
    bool Absent() const { return !HasDetectedModem(); }
    void RequestRecheck();
    NetworkInterface* GetNetwork() const { return network_.load(); }

private:
    static void TaskEntry(void* arg);
    void Run();
    void SetReady(bool ready);
    void MarkLinkLost(bool needs_dial);
    bool IsCurrentLink(uint32_t generation) const {
        return !paused_.load() && generation == lost_generation_.load();
    }
    void Notify(NetworkEvent event, const std::string& data = {});
    void NotifyDetection(ProbeState state);
    bool ConfirmPdpAddress(uint32_t generation);
    bool DialPdp(uint32_t generation);
    std::mutex lifecycle_mutex_;
    NetworkEventCallback callback_;
    DetectionCallback detection_callback_;
    bool started_ = false;
    bool exiting_ = false;
    bool restart_requested_ = false;
    StaticEventGroup_t wake_storage_;
    const EventGroupHandle_t wake_events_ = xEventGroupCreateStatic(&wake_storage_);
    std::unique_ptr<AtModem> modem_;
    std::unique_ptr<NetworkInterface> network_adapter_;
    std::atomic<NetworkInterface*> network_{nullptr};
    std::atomic<ProbeState> probe_state_{ProbeState::kNotChecked};
    std::atomic<bool> ready_{false};
    std::atomic<uint32_t> ready_generation_{0};
    std::atomic<bool> paused_{false};
    std::atomic<uint32_t> lost_generation_{0};
    std::atomic<uint32_t> recheck_generation_{0};
    std::atomic<unsigned> overflow_count_{0};
    // Set after an explicit PDP deactivation or a failed PDP confirmation.
    // The next registration-ready pass must issue AT+MIPCALL=1,1, because
    // ML307 does not automatically dial again after MIPCALL=0,1.
    std::atomic<bool> needs_dial_{false};
};
#endif
#endif
