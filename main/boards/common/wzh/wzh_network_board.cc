#include "wzh_network_board.h"

#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC
#include "application.h"
#include "../../wzh_dinosaur/dinosaur_cellular.h"
#include "wifi_manager.h"

#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <nvs.h>
#include <utility>

namespace wzh {
namespace {
constexpr char TAG[] = "WzhNetwork";
}

NetworkBoard::NetworkBoard() : cellular_(std::make_unique<DinosaurCellular>()) {}
NetworkBoard::~NetworkBoard() = default;

void NetworkBoard::StartNetwork() {
    const bool saved = LoadCellularPreference();
    cellular_saved_.store(saved);
    network_.store(saved ? NetworkChoice::kCellular : NetworkChoice::kWifi);
    // Always detect the hardware. Only an explicitly saved choice allows
    // the modem task to register and use the data connection.
    cellular_->SetPaused(!saved);
    WifiBoard::StartNetwork();
    if (!cellular_->Start(
            [this](NetworkEvent event, const std::string& data) {
                Application::GetInstance().Schedule([this, event, data]() {
                    if (UsesCellularNetwork() && !transport_trial_.load() &&
                        !IsNetworkShuttingDown() && event != NetworkEvent::ModemErrorInitFailed &&
                        !(event == NetworkEvent::Disconnected && cellular_->IsReady()))
                        ForwardNetworkEvent(event, data);
                });
            },
            [this](bool detected) {
                if (!detected)
                    Application::GetInstance().Schedule([this]() { FallbackToWifi(); });
            })) {
        FallbackToWifi();
    }
}

bool NetworkBoard::DeferWifiStationStart() const { return UsesCellularNetwork(); }
bool NetworkBoard::UsesCellularNetwork() const { return network_.load() == NetworkChoice::kCellular; }
bool NetworkBoard::HasCellularModem() const { return !cellular_->Absent(); }
bool NetworkBoard::HasSavedCellularNetwork() const { return cellular_saved_.load(); }
bool NetworkBoard::ClearNetworkPreference() { return SaveCellularPreference(false); }
size_t NetworkBoard::GetProtocolPayloadLimit() const { return UsesCellularNetwork() ? 1024 : 0; }
bool NetworkBoard::IsNetworkAvailable() const {
    if (IsNetworkShuttingDown() || transport_trial_.load())
        return false;
    return UsesCellularNetwork() ? cellular_->IsReady() : WifiBoard::IsNetworkAvailable();
}
NetworkInterface* NetworkBoard::GetNetwork() {
    return UsesCellularNetwork() ? cellular_->GetNetwork() : WifiBoard::GetNetwork();
}

const char* NetworkBoard::ConnectCellularNetwork() {
    if (!HasCellularModem())
        return "设备未检测到 4G 模块";
    if (UsesCellularNetwork() && cellular_->IsReady() && HasSavedCellularNetwork())
        return nullptr;
    if (!BeginTransportTrial())
        return "设备正在启动或处理任务，请稍后重试";

    auto& wifi = WifiManager::GetInstance();
    wifi.StopStation();
    cellular_->SetPaused(false);
    const auto deadline = esp_timer_get_time() + 30'000'000;
    while (!cellular_->IsReady() && !IsNetworkShuttingDown() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const bool ready = cellular_->IsReady() && !IsNetworkShuttingDown();
    const bool saved = ready && SaveCellularPreference(true);
    if (!saved && !UsesCellularNetwork()) {
        cellular_->SetPaused(true);
        wifi.StartStation();
    }
    CompleteTransportTrial(saved ? NetworkChoice::kCellular : network_.load());
    if (saved)
        return nullptr;
    return ready ? "4G 已连接，保存设置失败" : "4G 连接超时，请检查 SIM 卡和信号";
}

bool NetworkBoard::BeginWifiNetworkTrial() {
    if (!UsesCellularNetwork())
        return true;
    if (!BeginTransportTrial())
        return false;
    cellular_->SetPaused(true);
    return true;
}

bool NetworkBoard::FinishWifiNetworkTrial(bool connected) {
    const bool saved = connected && (!HasSavedCellularNetwork() || SaveCellularPreference(false));
    if (UsesCellularNetwork()) {
        if (!saved) {
            WifiManager::GetInstance().StopStation();
            cellular_->SetPaused(false);
        }
        CompleteTransportTrial(saved ? NetworkChoice::kWifi : NetworkChoice::kCellular);
    }
    return !connected || saved;
}

const char* NetworkBoard::GetNetworkStateIcon() {
    if (!UsesCellularNetwork())
        return WifiBoard::GetNetworkStateIcon();
    return cellular_->IsReady() ? MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT
                               : MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF;
}

bool NetworkBoard::LoadCellularPreference() {
    nvs_handle_t handle;
    if (nvs_open("dino_net", NVS_READONLY, &handle) != ESP_OK)
        return false;
    char value[16]{};
    size_t size = sizeof(value);
    const auto result = nvs_get_str(handle, "network", value, &size);
    nvs_close(handle);
    return result == ESP_OK && std::strcmp(value, "cellular") == 0;
}

bool NetworkBoard::SaveCellularPreference(bool cellular) {
    nvs_handle_t handle;
    auto result = nvs_open("dino_net", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = cellular ? nvs_set_str(handle, "network", "cellular")
                          : nvs_erase_key(handle, "network");
        if (!cellular && result == ESP_ERR_NVS_NOT_FOUND)
            result = ESP_OK;
        if (result == ESP_OK)
            result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK)
        cellular_saved_.store(cellular);
    else
        ESP_LOGE(TAG, "Cannot save network preference: %s", esp_err_to_name(result));
    return result == ESP_OK;
}

bool NetworkBoard::BeginTransportTrial() {
    StaticSemaphore_t completion_storage;
    auto done = xSemaphoreCreateBinaryStatic(&completion_storage);
    bool prepared = false;
    Application::GetInstance().PrepareNetworkTransportSwitch([this, done, &prepared](bool ok) {
        if (ok && !IsNetworkShuttingDown()) {
            transport_trial_.store(true);
            ForwardNetworkEvent(NetworkEvent::Disconnected);
            prepared = true;
        }
        xSemaphoreGive(done);
    });
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return prepared;
}

void NetworkBoard::CompleteTransportTrial(NetworkChoice choice) {
    network_.store(choice);
    transport_trial_.store(false);
    Application::GetInstance().Schedule([this, choice]() {
        if (network_.load() != choice || !IsNetworkAvailable())
            return;
        const auto name = choice == NetworkChoice::kCellular ? "4G"
                                                            : WifiManager::GetInstance().GetSsid();
        ForwardNetworkEvent(NetworkEvent::Connected, name);
    });
}

void NetworkBoard::ForwardNetworkEvent(NetworkEvent event, const std::string& data) {
    if (UsesCellularNetwork() && event == NetworkEvent::Connected) {
        if (!cellular_->IsReady() || std::exchange(cellular_connected_reported_, true))
            return;
    } else if (event == NetworkEvent::Disconnected) {
        cellular_connected_reported_ = false;
    }
    if (cloud_network_callback_)
        cloud_network_callback_(event, data);
}

void NetworkBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    cloud_network_callback_ = std::move(callback);
    WifiBoard::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        if (!UsesCellularNetwork() && !transport_trial_.load() && !IsNetworkShuttingDown()) {
            if (cloud_network_callback_)
                cloud_network_callback_(event, data);
        }
    });
}

void NetworkBoard::FallbackToWifi() {
    if (IsNetworkShuttingDown() || !UsesCellularNetwork() || transport_trial_.load())
        return;
    network_.store(NetworkChoice::kWifi);
    cellular_->SetPaused(true);
    WifiManager::GetInstance().StartStation();
}

}  // namespace wzh
#endif
