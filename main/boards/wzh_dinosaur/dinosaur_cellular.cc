#include "dinosaur_cellular.h"

#if DINOSAUR_ENABLE_4G
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <utility>
#include "config.h"
#include "dinosaur_modem_pins.h"

namespace {
constexpr char kTag[] = "DinoCellular";
constexpr int kTx = DINOSAUR_ML307_TX_GPIO;
constexpr int kRx = DINOSAUR_ML307_RX_GPIO;
constexpr int kDtr = CONFIG_WZH_DINOSAUR_ML307_DTR_GPIO;

}  // namespace

bool DinosaurCellular::Start(NetworkEventCallback callback) {
    if (!DinosaurValidModemPins(kTx, kRx, kDtr)) {
        ESP_LOGE(kTag, "ML307 UART pins missing, invalid, or conflict with dinosaur hardware");
        if (callback)
            callback(NetworkEvent::ModemErrorInitFailed, "ML307 UART pins not configured");
        return false;
    }
    if (started_.exchange(true))
        return true;
    callback_ = std::move(callback);
    if (xTaskCreate(TaskEntry, "dino_ml307", 4096, this, 5, nullptr) != pdPASS) {
        started_.store(false);
        ESP_LOGE(kTag, "Cannot start ML307 network task");
        if (callback_)
            callback_(NetworkEvent::ModemErrorInitFailed, "ML307 task creation failed");
        return false;
    }
    return true;
}

void DinosaurCellular::TaskEntry(void* arg) {
    static_cast<DinosaurCellular*>(arg)->Run();
    vTaskDelete(nullptr);
}

void DinosaurCellular::SetReady(bool ready) {
    if (ready_.exchange(ready) == ready)
        return;
    ESP_LOGI(kTag, "ML307 network %s", ready ? "ready" : "lost");
    if (callback_)
        callback_(ready ? NetworkEvent::Connected : NetworkEvent::Disconnected, ready ? "4G" : "");
}

void DinosaurCellular::Run() {
    if (callback_)
        callback_(NetworkEvent::ModemDetecting, "");
    ESP_LOGI(kTag, "XiaoZhi ML307 AT mode: ESP32 TX=%d RX=%d, target baud=921600", kTx, kRx);
    unsigned detection_failures = 0;
    while (!modem_) {
        // A bounded detect timeout permits retries without blocking the app.
        modem_ = AtModem::Detect(static_cast<gpio_num_t>(kTx), static_cast<gpio_num_t>(kRx),
                                 static_cast<gpio_num_t>(kDtr), 921600, 5000);
        if (!modem_) {
            ++detection_failures;
            if (detection_failures == 3 && callback_)
                callback_(NetworkEvent::ModemErrorInitFailed, "ML307 not detected");
            // The first probe can fail just after the modem changes baud rate.
            // Retry promptly like XiaoZhi, then back off on a persistent fault.
            const uint32_t retry_ms = detection_failures < 3 ? 1000 : 10000;
            ESP_LOGW(kTag, "ML307 not detected; retrying in %lu ms",
                     static_cast<unsigned long>(retry_ms));
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
        }
    }
    ESP_LOGI(kTag, "ML307 detected; waiting for SIM and network registration");
    network_.store(modem_.get());
    modem_->OnNetworkStateChanged([this](bool ready) {
        if (!ready)
            lost_generation_.fetch_add(1);
    });
    uint32_t seen_loss = lost_generation_.load();
    auto last_failure = NetworkStatus::Ready;
    for (;;) {
        if (!IsReady()) {
            if (callback_)
                callback_(NetworkEvent::Connecting, "");
            const uint32_t generation = lost_generation_.load();
            const auto result = modem_->WaitForNetworkReady(15000);
            seen_loss = lost_generation_.load();
            if (result == NetworkStatus::Ready && generation == seen_loss) {
                last_failure = NetworkStatus::Ready;
                SetReady(true);
            } else {
                if (result != last_failure) {
                    if (result == NetworkStatus::ErrorInsertPin && callback_)
                        callback_(NetworkEvent::ModemErrorNoSim, "");
                    else if (result == NetworkStatus::ErrorRegistrationDenied && callback_)
                        callback_(NetworkEvent::ModemErrorRegDenied, "");
                    else
                        ESP_LOGW(kTag, "ML307 registration not ready; retrying");
                    last_failure = result;
                }
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (lost_generation_.load() != seen_loss) {
                SetReady(false);
                seen_loss = lost_generation_.load();
            }
        }
    }
}
#endif
