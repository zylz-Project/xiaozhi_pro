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
constexpr EventBits_t kModeChanged = BIT0;
constexpr EventBits_t kLinkChanged = BIT1;
constexpr EventBits_t kRecheckRequested = BIT2;
constexpr EventBits_t kRetryWakeEvents = kModeChanged | kRecheckRequested;
constexpr EventBits_t kWakeEvents = kRetryWakeEvents | kLinkChanged;

}  // namespace

bool DinosaurCellular::Start(NetworkEventCallback callback, DetectionCallback detection) {
    if (!DinosaurValidModemPins(kTx, kRx, kDtr)) {
        probe_state_.store(ProbeState::kUnavailable);
        ESP_LOGE(kTag, "ML307 UART pins missing, invalid, or conflict with dinosaur hardware");
        if (callback)
            callback(NetworkEvent::ModemErrorInitFailed, "ML307 UART pins not configured");
        return false;
    }
    NetworkEventCallback failure_callback;
    DetectionCallback replay;
    bool started = true;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        callback_ = std::move(callback);
        detection_callback_ = std::move(detection);
        if (started_) {
            if (exiting_) {
                // Let the worker finish releasing UART before re-probing.
                restart_requested_ = true;
                probe_state_.store(ProbeState::kProbing);
            } else if (HasDetectedModem()) {
                // The probe may have completed just as the caller requested it.
                replay = std::move(detection_callback_);
            }
        } else {
            probe_state_.store(ProbeState::kProbing);
            started_ = xTaskCreate(TaskEntry, "dino_ml307", 4096, this, 5, nullptr) == pdPASS;
            if (!started_) {
                probe_state_.store(ProbeState::kUnavailable);
                failure_callback = std::move(callback_);
                detection_callback_ = nullptr;
                started = false;
            }
        }
    }
    if (!started)
        ESP_LOGE(kTag, "Cannot start ML307 network task");
    if (failure_callback)
        failure_callback(NetworkEvent::ModemErrorInitFailed, "ML307 task creation failed");
    if (replay)
        replay(true);
    return started;
}

void DinosaurCellular::TaskEntry(void* arg) {
    auto* self = static_cast<DinosaurCellular*>(arg);
    for (;;) {
        self->Run();
        self->SetReady(false);
        self->network_.store(nullptr);
        self->network_adapter_.reset();
        self->modem_.reset();
        std::lock_guard<std::mutex> lock(self->lifecycle_mutex_);
        self->exiting_ = false;
        if (std::exchange(self->restart_requested_, false))
            continue;
        self->started_ = false;
        self->callback_ = nullptr;
        self->detection_callback_ = nullptr;
        break;
    }
    vTaskDelete(nullptr);
}

void DinosaurCellular::Notify(NetworkEvent event, const std::string& data) {
    NetworkEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        callback = callback_;
    }
    if (callback)
        callback(event, data);
}

void DinosaurCellular::NotifyDetection(ProbeState state) {
    const bool detected = state == ProbeState::kDetected;
    DetectionCallback callback;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        probe_state_.store(state);
        exiting_ = !detected;
        callback = std::move(detection_callback_);
    }
    if (callback)
        callback(detected);
}

void DinosaurCellular::SetReady(bool ready) {
    if (ready_.exchange(ready) == ready || (ready && !IsReady()))
        return;
    ESP_LOGI(kTag, "ML307 network %s", ready ? "ready" : "lost");
    Notify(ready ? NetworkEvent::Connected : NetworkEvent::Disconnected, ready ? "4G" : "");
}

void DinosaurCellular::MarkLinkLost(bool needs_dial) {
    if (needs_dial)
        needs_dial_.store(true);
    lost_generation_.fetch_add(1);
    if (auto* network = static_cast<DinosaurMl307Network*>(network_.load()))
        network->InterruptSockets();
    xEventGroupSetBits(wake_events_, kLinkChanged);
}

void DinosaurCellular::RequestRecheck() {
    recheck_generation_.fetch_add(1);
    xEventGroupSetBits(wake_events_, kRecheckRequested);
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
    if (!uart || !IsCurrentLink(generation))
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
    for (int attempt = 0; attempt < 3 && IsCurrentLink(generation); ++attempt) {
        valid.store(false);
        const auto pdp_query = uart->SendCommand("AT+MIPCALL?", 1500);
        if (AtCommandSucceeded(pdp_query) && valid.load() && IsCurrentLink(generation)) {
            confirmed = true;
            break;
        }
        if (attempt < 2 && IsCurrentLink(generation))
            xEventGroupWaitBits(wake_events_, kModeChanged, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
    }
    if (confirmed && IsCurrentLink(generation)) {
        // One bounded signal sample before announcing readiness; no polling
        // or extra AT commands while conversation traffic is active.
        static_cast<void>(uart->SendCommand("AT+CSQ", 500));
        confirmed = IsCurrentLink(generation);
        if (confirmed)
            ESP_LOGI(kTag, "PDP address confirmed; CSQ=%d", csq.load());
    }
    if (!confirmed)
        ESP_LOGW(kTag, "PDP address unavailable or network changed; registration will be retried");
    uart->UnregisterUrcCallback(listener);
    return confirmed && IsCurrentLink(generation);
}

bool DinosaurCellular::DialPdp(uint32_t generation) {
    auto uart = modem_->GetAtUart();
    if (!uart || !IsCurrentLink(generation))
        return false;

    ESP_LOGI(kTag, "Activating PDP context 1 after loss");
    const auto result = uart->SendCommand("AT+MIPCALL=1,1", kPdpDialTimeoutMs);
    if (AtCommandSucceeded(result)) {
        return IsCurrentLink(generation);
    }

    if (AtCommandCme(result) != 0) {
        // A CME is a definitive modem answer (for example "already active").
        // Let the following MIPCALL? check decide whether an address exists.
        ESP_LOGW(kTag, "PDP activation rejected (cme=%d); checking PDP state",
                 AtCommandCme(result));
        return IsCurrentLink(generation);
    }

    // A timeout leaves a late OK/URC in the parser. Two plain probes ensure a
    // subsequent MIPCALL? query cannot consume the previous command's output.
    for (int pass = 0; pass < 2; ++pass) {
        if (!IsCurrentLink(generation))
            return false;
        if (!AtCommandSucceeded(uart->SendCommand("AT", 1000))) {
            ESP_LOGE(kTag, "Cannot resynchronise AT parser after PDP activation timeout");
            return false;
        }
    }
    return IsCurrentLink(generation);
}

void DinosaurCellular::SetPaused(bool paused) {
    if (paused_.exchange(paused) == paused)
        return;
    if (paused) {
        // Demote to not-ready immediately so status pages and conversation
        // gating stop trusting the cellular link while it is deselected.
        MarkLinkLost(false);
        ESP_LOGI(kTag, "ML307 deselected; cellular reporting offline");
    } else {
        ESP_LOGI(kTag, "ML307 reselected; waking registration task");
    }
    xEventGroupSetBits(wake_events_, kModeChanged);
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
                NotifyDetection(detected.error().code == AtErrc::Timeout
                                    ? ProbeState::kNoResponse
                                    : ProbeState::kUnavailable);
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
        Notify(NetworkEvent::ModemErrorInitFailed, "ML307 UART unavailable");
        NotifyDetection(ProbeState::kUnavailable);
        return;
    }
    ESP_LOGI(kTag, "ML307 detected; waiting for SIM and network registration");
    // Replace UDP, TCP/SSL, HTTP, MQTT, and WebSocket with the board adapters.
    // UDP/TCP/WebSocket use the inline HEX form that has no binary-payload
    // prompt to time out on; HTTP streams and validates its URCs; MQTT keeps a
    // local connection cache instead of issuing MQTTSTATE on every health
    // check. All adapters share this one AtUart.
    network_adapter_ = std::make_unique<DinosaurMl307Network>(*this, uart);
    network_.store(network_adapter_.get());
    modem_->OnNetworkStateChanged([this](bool ready) {
        if (!ready)
            MarkLinkLost(true);
    });
    // Observe explicit PDP deactivation URCs without polling or issuing AT
    // commands while the modem is carrying conversation and TTS traffic.
    auto pdp_listener = uart->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& args) {
            // Lost UART data invalidates modem commands already in flight.
            if (command == "FIFO_OVERFLOW") {
                const unsigned overflow = overflow_count_.fetch_add(1) + 1;
                ESP_LOGW(kTag, "AT FIFO overflow #%u; forcing link recheck", overflow);
                MarkLinkLost(false);
                return;
            }
            if (command == "MQTTSTATE") {
                const bool numeric = !args.empty() && args[0].type == AtArgumentValue::Type::Int;
                ESP_LOGI(kTag, "ML307 MQTT status response: fields=%u numeric=%d",
                         static_cast<unsigned>(args.size()), numeric);
            }
            if (command == "MATREADY" || IsMl307PdpDeactivated(command, args)) {
                MarkLinkLost(true);
            }
        });
    struct ListenerGuard {
        std::shared_ptr<AtUart> uart;
        decltype(pdp_listener) listener;
        ~ListenerGuard() { uart->UnregisterUrcCallback(listener); }
    } listener_guard{uart, pdp_listener};
    NotifyDetection(ProbeState::kDetected);
    uint32_t seen_loss = lost_generation_.load();
    uint32_t seen_recheck = recheck_generation_.load();
    auto last_failure = NetworkStatus::Ready;
    bool connecting_reported = false;
    for (;;) {
        if (paused_.load()) {
            SetReady(false);
            connecting_reported = false;
            xEventGroupWaitBits(wake_events_, kModeChanged, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        const uint32_t loss = lost_generation_.load();
        if (loss != seen_loss) {
            ESP_LOGW(kTag, "4G registration or PDP lost; restarting registration");
            SetReady(false);
            seen_loss = loss;
            seen_recheck = recheck_generation_.load();
        }
        if (!IsReady()) {
            if (!connecting_reported) {
                // One registration notification per outage. Repeated failures
                // retry silently instead of repainting the offline status.
                Notify(NetworkEvent::Connecting);
                connecting_reported = true;
            }
            const uint32_t generation = lost_generation_.load();
            if (!IsCurrentLink(generation))
                continue;
            // The SDK retries CPIN up to ten times before waiting for an AT
            // registration event. A silent UART after a modem reset should
            // reach our bounded retry path instead of blocking there.
            const auto pin = uart->SendCommand("AT+CPIN?", 1500);
            if (!IsCurrentLink(generation))
                continue;
            if (AtCommandSucceeded(pin) && kDtr == GPIO_NUM_NC) {
                // Without DTR there is no dedicated modem wake signal. Keep
                // the modem awake, including after a modem reboot or PDP loss.
                if (modem_->SetSleepMode(false))
                    ESP_LOGI(kTag, "ML307 sleep disabled (DTR not connected)");
                else
                    ESP_LOGW(kTag, "ML307 did not confirm sleep disable; continuing registration");
            }
            if (!IsCurrentLink(generation))
                continue;
            const auto result = AtCommandSucceeded(pin)
                                    ? modem_->WaitForNetworkReady(15000)
                                    : (AtCommandCme(pin) == 10 ? NetworkStatus::ErrorInsertPin
                                                               : NetworkStatus::Error);
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
                if (needs_dial_.exchange(false)) {
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
                ConfirmPdpAddress(generation) && IsCurrentLink(generation)) {
                connecting_reported = false;
                last_failure = NetworkStatus::Ready;
                ESP_LOGI(kTag, "Registration and PDP confirmed; generation=%lu",
                         static_cast<unsigned long>(generation));
                ready_generation_.store(generation);
                SetReady(true);
                // A recheck queued while registration was in progress is
                // satisfied by the PDP confirmation that just completed.
                seen_recheck = recheck_generation_.load();
                if (!IsCurrentLink(generation))
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
                // Explicit requests can retry now. Repeated modem loss URCs
                // do not interrupt a failed registration's backoff.
                xEventGroupWaitBits(wake_events_, kRetryWakeEvents, pdTRUE, pdFALSE,
                                    pdMS_TO_TICKS(10000));
            }
        } else {
            const uint32_t recheck = recheck_generation_.load();
            if (recheck == seen_recheck) {
                xEventGroupWaitBits(wake_events_, kWakeEvents, pdTRUE, pdFALSE, portMAX_DELAY);
            } else {
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
