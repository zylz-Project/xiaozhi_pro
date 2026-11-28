#include "dinosaur_cellular.h"

#if DINOSAUR_ENABLE_4G
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <utility>
#include "config.h"
#include "dinosaur_ml307_at.h"
#include "dinosaur_ml307_network.h"
#include "dinosaur_modem_pins.h"

namespace {
constexpr char kTag[] = "DinoCellular";
constexpr int kTx = DINOSAUR_ML307_TX_GPIO;
constexpr int kRx = DINOSAUR_ML307_RX_GPIO;
constexpr int kDtr = CONFIG_WZH_DINOSAUR_ML307_DTR_GPIO;
// Bound an absent-modem probe; the board also enforces an independent boot
// deadline covering registration and PDP, not just UART detection.
constexpr unsigned kDetectionAttempts = 2;
constexpr int kDetectTimeoutMs = 2000;
constexpr uint32_t kDetectRetryMs = 500;
// PDP activation can wait for the modem's own packet-data attach. Keep this
// bounded so a missing PDP never blocks the network task indefinitely.
constexpr size_t kPdpDialTimeoutMs = 15000;

}  // namespace

bool DinosaurCellular::Start(NetworkEventCallback callback, DetectionCallback detection) {
    if (!DinosaurValidModemPins(kTx, kRx, kDtr)) {
        ESP_LOGE(kTag, "ML307 UART pins missing, invalid, or conflict with dinosaur hardware");
        if (callback)
            callback(NetworkEvent::ModemErrorInitFailed, "ML307 UART pins not configured");
        return false;
    }
    NetworkEventCallback failure_callback;
    for (;;) {
        std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
        if (started_.load() && !exiting_.load()) {
            // A runtime switch may install a fresh callback while the probe or
            // registration task is already running. Keep the task, replace the
            // sink, and let it report the eventual detection result.
            std::lock_guard<std::mutex> callbacks(callback_mutex_);
            callback_ = std::move(callback);
            detection_callback_ = std::move(detection);
            return true;
        }
        if (exiting_.load()) {
            // Run() has returned but TaskEntry still owns the modem and the
            // callback slots. Wait for that cleanup instead of returning true
            // for a task that is about to disappear.
            lifecycle.unlock();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        {
            std::lock_guard<std::mutex> callbacks(callback_mutex_);
            callback_ = std::move(callback);
            detection_callback_ = std::move(detection);
        }
        started_.store(true);
        if (xTaskCreate(TaskEntry, "dino_ml307", 4096, this, 5, nullptr) != pdPASS) {
            started_.store(false);
            {
                std::lock_guard<std::mutex> callbacks(callback_mutex_);
                failure_callback = std::move(callback_);
                callback_ = nullptr;
                detection_callback_ = nullptr;
            }
            ESP_LOGE(kTag, "Cannot start ML307 network task");
        } else {
            return true;
        }
        break;
    }
    if (failure_callback)
        failure_callback(NetworkEvent::ModemErrorInitFailed, "ML307 task creation failed");
    return false;
}

void DinosaurCellular::TaskEntry(void* arg) {
    auto* self = static_cast<DinosaurCellular*>(arg);
    self->Run();
    // The probe/registration loop ended (absent modem, or a terminal failure
    // before registration). Allow a later Start() to bring it back: the user
    // may plug the modem in and switch to 4G from the portal.
    self->SetReady(false);
    {
        std::lock_guard<std::mutex> lifecycle(self->lifecycle_mutex_);
        self->started_.store(false);
        self->network_.store(nullptr);
        self->network_adapter_.reset();
        self->modem_.reset();
        std::lock_guard<std::mutex> callbacks(self->callback_mutex_);
        self->callback_ = nullptr;
        self->detection_callback_ = nullptr;
    }
    self->exiting_.store(false);
    vTaskDelete(nullptr);
}

void DinosaurCellular::Notify(NetworkEvent event, const std::string& data) {
    NetworkEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = callback_;
    }
    if (callback)
        callback(event, data);
}

void DinosaurCellular::NotifyDetection(bool detected) {
    DetectionCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = detection_callback_;
    }
    if (callback)
        callback(detected);
}

void DinosaurCellular::SetReady(bool ready) {
    if (ready_.exchange(ready) == ready)
        return;
    ESP_LOGI(kTag, "ML307 network %s", ready ? "ready" : "lost");
    Notify(ready ? NetworkEvent::Connected : NetworkEvent::Disconnected, ready ? "4G" : "");
}

namespace {
bool ValidPdpAddress(const std::string& address) {
    // Validate without depending on the ESP32's optional IPv6 socket stack:
    // the AT modem supplies its own data transport.
    if (address.find(':') == std::string::npos) {
        unsigned parts = 0, value = 0, digits = 0;
        bool nonzero = false;
        for (char ch : address) {
            if (ch >= '0' && ch <= '9') {
                if (++digits > 3)
                    return false;
                value = value * 10 + static_cast<unsigned>(ch - '0');
                if (value > 255)
                    return false;
            } else if (ch == '.') {
                if (!digits || ++parts >= 4)
                    return false;
                nonzero = nonzero || value != 0;
                value = digits = 0;
            } else {
                return false;
            }
        }
        return digits && parts == 3 && (nonzero || value != 0);
    }
    const char* p = address.c_str();
    bool compressed = false, nonzero = false;
    unsigned groups = 0;
    if (*p == ':') {
        if (p[1] != ':')
            return false;
        compressed = true;
        p += 2;
    }
    while (*p) {
        unsigned digits = 0;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            if (++digits > 4)
                return false;
            nonzero = nonzero || *p != '0';
            ++p;
        }
        if (!digits || ++groups > 8)
            return false;
        if (!*p)
            break;
        if (*p++ != ':')
            return false;
        if (*p == ':') {
            if (compressed)
                return false;
            compressed = true;
            ++p;
            if (!*p)
                break;
        } else if (!*p) {
            return false;
        }
    }
    return nonzero && (compressed ? groups < 8 : groups == 8);
}
}  // namespace

bool DinosaurCellular::ConfirmPdpAddress(uint32_t generation) {
    auto uart = modem_->GetAtUart();
    if (!uart)
        return false;
    std::atomic<bool> valid{false};
    std::atomic<int> csq{-1};
    auto listener = uart->RegisterUrcCallback(
        [&valid, &csq](const std::string& command, const std::vector<AtArgumentValue>& args) {
            if (command == "MIPCALL" && args.size() >= 2 &&
                args[0].type == AtArgumentValue::Type::Int &&
                args[1].type == AtArgumentValue::Type::Int && args[0].int_value == 1) {
                const bool active = args[1].int_value == 1 && args.size() >= 3 &&
                                    ValidPdpAddress(args[2].string_value);
                valid.store(active);
            } else if (command == "CSQ" && !args.empty() &&
                       args[0].type == AtArgumentValue::Type::Int) {
                csq.store(args[0].int_value);
            }
        });
    bool confirmed = false;
    for (int attempt = 0; attempt < 3 && lost_generation_.load() == generation; ++attempt) {
        valid.store(false);
        const auto pdp_query = uart->SendCommand("AT+MIPCALL?", 1500);
        if (AtCommandSucceeded(pdp_query) && valid.load() &&
            lost_generation_.load() == generation) {
            confirmed = true;
            break;
        }
        if (attempt < 2 && lost_generation_.load() == generation)
            vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (confirmed) {
        // One bounded signal sample before announcing readiness; no polling
        // or extra AT commands while conversation traffic is active.
        static_cast<void>(uart->SendCommand("AT+CSQ", 500));
        confirmed = lost_generation_.load() == generation;
        if (confirmed)
            ESP_LOGI(kTag, "PDP address confirmed; CSQ=%d", csq.load());
    }
    if (!confirmed)
        ESP_LOGW(kTag, "PDP address unavailable or network changed; registration will be retried");
    uart->UnregisterUrcCallback(listener);
    return confirmed;
}

bool DinosaurCellular::DialPdp(uint32_t generation) {
    auto uart = modem_->GetAtUart();
    if (!uart || lost_generation_.load() != generation)
        return false;

    ESP_LOGI(kTag, "Activating PDP context 1 after loss");
    const auto result = uart->SendCommand("AT+MIPCALL=1,1", kPdpDialTimeoutMs);
    if (AtCommandSucceeded(result)) {
        return lost_generation_.load() == generation;
    }

    if (AtCommandCme(result) != 0) {
        // A CME is a definitive modem answer (for example "already active").
        // Let the following MIPCALL? check decide whether an address exists.
        ESP_LOGW(kTag, "PDP activation rejected (cme=%d); checking PDP state",
                 AtCommandCme(result));
        return lost_generation_.load() == generation;
    }

    // A timeout leaves a late OK/URC in the parser. Two plain probes ensure a
    // subsequent MIPCALL? query cannot consume the previous command's output.
    for (int pass = 0; pass < 2; ++pass) {
        if (!AtCommandSucceeded(uart->SendCommand("AT", 1000))) {
            ESP_LOGE(kTag, "Cannot resynchronise AT parser after PDP activation timeout");
            return false;
        }
    }
    return lost_generation_.load() == generation;
}

void DinosaurCellular::SetPaused(bool paused) {
    if (paused_.exchange(paused) == paused)
        return;
    if (paused) {
        // Demote to not-ready immediately so status pages and conversation
        // gating stop trusting the cellular link while it is deselected.
        lost_generation_.fetch_add(1);
        SetReady(false);
        ESP_LOGI(kTag, "ML307 deselected; cellular reporting offline");
    } else {
        // Resume registration on the next loop iteration. Do not bump the
        // generation again: the pause already invalidated any in-flight work,
        // and a fresh bump here would force an unnecessary modem restart.
        ESP_LOGI(kTag, "ML307 reselected; resuming registration");
    }
}

void DinosaurCellular::Run() {
    Notify(NetworkEvent::ModemDetecting);
    ESP_LOGI(kTag, "XiaoZhi ML307 AT mode: ESP32 TX=%d RX=%d, target baud=921600", kTx, kRx);
    unsigned detection_failures = 0;
    while (!modem_) {
        ESP_LOGI(kTag, "Probing ML307 over UART (attempt %u/%u, %d ms window)",
                 detection_failures + 1, kDetectionAttempts, kDetectTimeoutMs);
        // A bounded detect timeout permits retries without blocking the app.
        auto detected = AtModem::Detect(static_cast<gpio_num_t>(kTx), static_cast<gpio_num_t>(kRx),
                                        static_cast<gpio_num_t>(kDtr), 921600, kDetectTimeoutMs);
        if (!detected) {
            ++detection_failures;
            if (detection_failures == kDetectionAttempts) {
                ESP_LOGW(kTag, "ML307 not detected after %u probes; selecting Wi-Fi",
                         kDetectionAttempts);
                exiting_.store(true);
                NotifyDetection(false);
                // Hardware is absent: the board stays on Wi-Fi and this task
                // ends. No modem-error alert — an unplugged module is a valid
                // Wi-Fi configuration, not a fault. Selecting 4G from the
                // portal calls Start() again for a fresh probe.
                return;
            }
            // The first probe can fail just after the modem changes baud rate.
            // Retry promptly like XiaoZhi, then back off on a persistent fault.
            ESP_LOGW(kTag,
                     "ML307 did not answer AT; check JP1 +5V/GND and UART 11/12, "
                     "retrying in %lu ms",
                     static_cast<unsigned long>(kDetectRetryMs));
            vTaskDelay(pdMS_TO_TICKS(kDetectRetryMs));
        } else {
            modem_ = std::move(*detected);
        }
    }
    auto uart = modem_->GetAtUart();
    if (!uart) {
        network_.store(nullptr);
        ESP_LOGE(kTag, "ML307 UART unavailable after detection");
        exiting_.store(true);
        Notify(NetworkEvent::ModemErrorInitFailed, "ML307 UART unavailable");
        NotifyDetection(false);
        return;
    }
    ESP_LOGI(kTag, "ML307 detected");
    // Replace UDP, TCP/SSL, HTTP, MQTT, and WebSocket with the board adapters.
    // UDP/TCP/WebSocket use the inline HEX form that has no binary-payload
    // prompt to time out on; HTTP streams and validates its URCs; MQTT keeps a
    // local connection cache instead of issuing MQTTSTATE on every health
    // check. All adapters share this one AtUart.
    network_adapter_ = std::make_unique<DinosaurMl307Network>(modem_.get(), uart);
    network_.store(network_adapter_.get());
    // First probe round: announce detection once. A late modem (found after
    // Wi-Fi fallback) still announces itself so an unpreference-based board
    // can re-evaluate; SelectNetwork ignores repeated calls.
    NotifyDetection(true);
    modem_->OnNetworkStateChanged([this](bool ready) {
        if (!ready) {
            lost_generation_.fetch_add(1);
            needs_dial_.store(true);
        }
    });
    // Observe explicit PDP deactivation URCs without polling or issuing AT
    // commands while the modem is carrying conversation and TTS traffic.
    auto pdp_listener = uart->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& args) {
            // The AT UART DMA ring overflowed: response bytes were dropped, so
            // any in-flight command will time out. Ml307Mqtt has no overflow
            // handler of its own (only TCP/SSL/HTTP do), which is why MQTT
            // silence used to last until the app's own retry. Report the link
            // as lost so the reconnect path starts from a known state.
            if (command == "FIFO_OVERFLOW") {
                const unsigned overflow = overflow_count_.fetch_add(1) + 1;
                ESP_LOGW(kTag, "AT FIFO overflow #%u; forcing link recheck", overflow);
                lost_generation_.fetch_add(1);
                return;
            }
            if (command == "MQTTSTATE") {
                const bool numeric = !args.empty() && args[0].type == AtArgumentValue::Type::Int;
                ESP_LOGI(kTag, "ML307 MQTT status response: fields=%u numeric=%d",
                         static_cast<unsigned>(args.size()), numeric);
            }
            if (command == "MIPCALL" && args.size() >= 2 &&
                args[0].type == AtArgumentValue::Type::Int &&
                args[1].type == AtArgumentValue::Type::Int && args[0].int_value == 1 &&
                args[1].int_value == 0) {
                lost_generation_.fetch_add(1);
                needs_dial_.store(true);
            }
        });
    struct ListenerGuard {
        std::shared_ptr<AtUart> uart;
        decltype(pdp_listener) listener;
        ~ListenerGuard() { uart->UnregisterUrcCallback(listener); }
    } listener_guard{uart, pdp_listener};
    uint32_t seen_loss = lost_generation_.load();
    uint32_t seen_recheck = recheck_generation_.load();
    auto last_failure = NetworkStatus::Ready;
    bool connecting_reported = false;
    bool flight_mode = false;
    bool radio_enabled = false;
    for (;;) {
        if (paused_.load()) {
            SetReady(false);
            radio_enabled = false;
            if (!flight_mode) {
                flight_mode = AtCommandSucceeded(uart->SendCommand("AT+CFUN=4", 3000));
                if (!flight_mode)
                    ESP_LOGW(kTag, "ML307 did not confirm flight mode");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!radio_enabled) {
            if (!AtCommandSucceeded(uart->SendCommand("AT+CFUN=1", 3000))) {
                ESP_LOGW(kTag, "ML307 radio did not confirm enable");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            radio_enabled = true;
            flight_mode = false;
            if (paused_.load())
                continue;
        }
        if (!IsReady()) {
            if (!connecting_reported) {
                // One registration notification per outage. Repeated failures
                // retry silently instead of repainting the offline status.
                Notify(NetworkEvent::Connecting);
                connecting_reported = true;
            }
            if (kDtr == GPIO_NUM_NC) {
                // Without DTR there is no dedicated modem wake signal. Keep
                // the modem awake, including after a modem reboot or PDP loss.
                if (modem_->SetSleepMode(false))
                    ESP_LOGI(kTag, "ML307 sleep disabled (DTR not connected)");
                else
                    ESP_LOGW(kTag, "ML307 did not confirm sleep disable; continuing registration");
            }
            const uint32_t generation = lost_generation_.load();
            const auto result = modem_->WaitForNetworkReady(15000);
            if (paused_.load()) {
                // A pause can arrive while the modem task is blocked in
                // registration. Do not let that stale result bring 4G back.
                seen_loss = lost_generation_.load();
                seen_recheck = recheck_generation_.load();
                SetReady(false);
                continue;
            }
            seen_loss = lost_generation_.load();
            bool transport_ok = true;
            if (result == NetworkStatus::Ready && generation == seen_loss) {
                // ML307 does not automatically dial after MIPCALL=0,1. The
                // module can also lose PDP while CEREG remains registered, so
                // recovery needs an explicit MIPCALL=1,1 before confirming the
                // address. Do it only when a loss/failed confirmation asked for
                // it; a healthy boot already used the modem's auto-dial.
                if (needs_dial_.load()) {
                    transport_ok = DialPdp(generation);
                    if (paused_.load()) {
                        seen_loss = lost_generation_.load();
                        seen_recheck = recheck_generation_.load();
                        SetReady(false);
                        continue;
                    }
                    seen_loss = lost_generation_.load();
                }
            }
            if (result == NetworkStatus::Ready && transport_ok && generation == seen_loss &&
                ConfirmPdpAddress(generation) && !paused_.load() &&
                lost_generation_.load() == generation) {
                needs_dial_.store(false);
                connecting_reported = false;
                last_failure = NetworkStatus::Ready;
                ESP_LOGI(kTag, "Registration and PDP confirmed; generation=%lu",
                         static_cast<unsigned long>(generation));
                SetReady(true);
                // A recheck queued while registration was in progress is
                // satisfied by the PDP confirmation that just completed.
                seen_recheck = recheck_generation_.load();
                if (lost_generation_.load() != generation)
                    SetReady(false);
            } else {
                if (result == NetworkStatus::Ready)
                    needs_dial_.store(true);
                if (result == NetworkStatus::Error) {
                    // WaitForNetworkReady reports a generic error when an AT
                    // registration command fails. Probe the parser once to
                    // distinguish a live AT channel from a dead UART.
                    const auto at_probe = uart->SendCommand("AT", 1000);
                    ESP_LOGW(kTag, "4G registration diagnostics: AT_response=%s cme=%d",
                             AtCommandSucceeded(at_probe) ? "ok" : "failed",
                             AtCommandCme(at_probe));
                }
                if (result != last_failure) {
                    if (result == NetworkStatus::ErrorInsertPin)
                        Notify(NetworkEvent::ModemErrorNoSim);
                    else if (result == NetworkStatus::ErrorRegistrationDenied)
                        Notify(NetworkEvent::ModemErrorRegDenied);
                    else
                        ESP_LOGW(kTag, "ML307 registration not ready; retrying");
                    last_failure = result;
                }
                ESP_LOGW(kTag, "4G not ready: registration=%d generation=%lu; retry in 10s",
                         static_cast<int>(result),
                         static_cast<unsigned long>(lost_generation_.load()));
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
            const uint32_t loss = lost_generation_.load();
            if (loss != seen_loss) {
                ESP_LOGW(kTag, "4G registration or PDP lost; restarting registration");
                SetReady(false);
                seen_loss = loss;
                seen_recheck = recheck_generation_.load();
                continue;
            }

            const uint32_t recheck = recheck_generation_.load();
            if (recheck != seen_recheck) {
                // Coalesce repeated write failures into one bounded probe.
                // ConfirmPdpAddress only treats lost_generation_ as a hard
                // invalidation, so this request no longer tears down the link.
                seen_recheck = recheck;
                ESP_LOGW(kTag, "Cellular write failed; confirming PDP before dropping 4G");
                if (ConfirmPdpAddress(loss)) {
                    ESP_LOGI(kTag, "PDP recheck confirmed; keeping 4G ready");
                    seen_recheck = recheck_generation_.load();
                } else {
                    ESP_LOGW(kTag, "PDP recheck failed; restarting registration");
                    needs_dial_.store(true);
                    SetReady(false);
                    seen_loss = lost_generation_.load();
                    seen_recheck = recheck_generation_.load();
                }
            }
        }
    }
}
#endif
