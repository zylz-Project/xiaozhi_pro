#ifndef WZH_DINOSAUR_CELLULAR_H_
#define WZH_DINOSAUR_CELLULAR_H_

#include <sdkconfig.h>
#include "config.h"
#if DINOSAUR_ENABLE_4G
#include <at_modem.h>
#include <atomic>
#include <memory>
#include "board.h"

// The modem outlives its network clients and the board. All blocking AT work
// stays on the network task; receive callbacks only signal a change.
class DinosaurCellular {
public:
    bool Start(NetworkEventCallback callback);
    bool IsReady() const { return ready_.load(); }
    NetworkInterface* GetNetwork() const { return network_.load(); }

private:
    static void TaskEntry(void* arg);
    void Run();
    void SetReady(bool ready);
    NetworkEventCallback callback_;
    std::unique_ptr<AtModem> modem_;
    std::atomic<AtModem*> network_{nullptr};
    std::atomic<bool> ready_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint32_t> lost_generation_{0};
};
#endif
#endif
