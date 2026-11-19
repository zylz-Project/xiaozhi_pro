#ifndef WZH_DINOSAUR_CELLULAR_H_
#define WZH_DINOSAUR_CELLULAR_H_

#include <sdkconfig.h>
#include "config.h"
#if DINOSAUR_ENABLE_4G
#include <at_modem.h>
#include <atomic>
#include <memory>
#include <mutex>
#include "board.h"

// The modem outlives its network clients and the board. All blocking AT work
// stays on the network task; receive callbacks only signal a change.
class DinosaurCellular {
public:
    using DetectionCallback = std::function<void(bool)>;
    bool Start(NetworkEventCallback callback, DetectionCallback detection = {});
    // Pause/resume registration without destroying the modem instance. A
    // paused cellular task idles in one-second ticks and reports not-ready.
    void SetPaused(bool paused);
    bool IsReady() const { return ready_.load(); }
    // True when the modem was never detected (or probing continues in the
    // background). Distinguishes "absent hardware" from "registered but not
    // ready" for the transport switch confirmation.
    bool Absent() const { return !started_.load() || network_.load() == nullptr; }
    void RequestRecheck() {
        // A failed cloud/audio write is only a hint. Confirm the PDP from the
        // modem task and keep the link ready when the registration is healthy.
        // Hard losses use lost_generation_ and still force a full restart.
        recheck_generation_.fetch_add(1);
    }
    NetworkInterface* GetNetwork() const { return network_.load(); }

private:
    static void TaskEntry(void* arg);
    void Run();
    void SetReady(bool ready);
    void Notify(NetworkEvent event, const std::string& data = {});
    void NotifyDetection(bool detected);
    bool ConfirmPdpAddress(uint32_t generation);
    bool DialPdp(uint32_t generation);
    std::mutex lifecycle_mutex_;
    std::mutex callback_mutex_;
    NetworkEventCallback callback_;
    DetectionCallback detection_callback_;
    std::unique_ptr<AtModem> modem_;
    std::unique_ptr<NetworkInterface> network_adapter_;
    std::atomic<NetworkInterface*> network_{nullptr};
    std::atomic<bool> ready_{false};
    std::atomic<bool> started_{false};
    // Set before Run() returns so a concurrent Start() waits for the old task
    // to release the modem instead of installing callbacks on a dying task.
    std::atomic<bool> exiting_{false};
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
