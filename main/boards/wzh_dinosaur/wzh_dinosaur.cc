#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>

#include <cstring>
#include <mutex>
#include <utility>

#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "dinosaur_audio.h"
#include "dinosaur_cellular.h"
#include "dinosaur_controller.h"
#include "dinosaur_power.h"
#include "display/display.h"
#include "settings.h"
#include "ssid_manager.h"
#include "wifi_board.h"
#include "wifi_manager.h"
#include "wzh_audio_hub.h"
#include "wzh_conversation_sounds.h"
#include "wzh_idle_power.h"
#include "wzh_mcp_tools.h"
#include "wzh_onboarding.h"
#include "wzh_power_telemetry.h"
#include "wzh_product.h"
#include "wzh_sensors.h"
#include "wzh_system_prompts.h"
#include "wzh_user_pairs_catalog.inc"
#include "wzh_voice_services.h"
#include "wzh_web_console.h"

#define TAG "WzhDinosaur"

// 这块板没有屏幕，但仍需要接收服务端下发的 llm.emotion。
// 通过继承 NoDisplay 拦截情绪事件，就能在不引入显示硬件的情况下驱动舵机。
class DinosaurDisplay : public NoDisplay {
public:
    explicit DinosaurDisplay(DinosaurController& controller) : controller_(controller) {}

    void SetEmotion(const char* emotion) override {
        Display::SetEmotion(emotion);
        controller_.HandleLlmEmotion(emotion);
    }

    // 句子文本提供当前回复的动作语气。用户转写提供兜底提示，
    // 真正播出语音时才由动作任务使用。
    void SetChatMessage(const char* role, const char* content) override {
        NoDisplay::SetChatMessage(role, content);
        if (role != nullptr && std::strcmp(role, "user") == 0 && content != nullptr) {
            controller_.HandleUserSpeechEmotion(content);
        } else if (role != nullptr && std::strcmp(role, "assistant") == 0 && content != nullptr) {
            controller_.HandleAssistantSpeech(content);
        }
    }

private:
    DinosaurController& controller_;
};

class WzhDinosaur : public WifiBoard {
public:
    WzhDinosaur()
        : boot_button_(BOOT_BUTTON_GPIO, false, 0, WZH_DOUBLE_CLICK_MS), display_(controller_) {
        // 配网热点和 DHCP 主机名使用产品前缀（"Dino-XXXX"），
        // 必须在 StartNetwork 之前设置。
        wifi_ssid_prefix_ = PANDA_AP_PREFIX;
        // 初始化顺序有依赖：ES8311 需要 I2C；动作控制器需要先拿到本地
        // 音频对象；MCP 工具最后注册，确保回调触发时控制器已经可用。
        InitializeI2c();
        if (!power_.Initialize()) {
            ESP_LOGE(TAG, "Power initialization failed; startup cancelled");
            gpio_set_level(POWER_CTRL_GPIO, 0);
            while (true)
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
        Application::GetInstance().GetAudioService().SetOutputAudioLevelListener(
            [this](float level, float attack) {
                local_audio_.FeedTtsLevel(level, attack);
                controller_.FeedOutputAudioLevel(level, attack);
            });
    }

    void OnAudioServiceReady() override {
        if (product_initialized_ || power_.IsShuttingDown())
            return;
        product_initialized_ = true;
        wzh::StartProductAudio();
        // Continue slower product initialization while the startup sound plays.
        wzh::RegisterUserPairs(wzh_user_assets::kPairs, std::size(wzh_user_assets::kPairs));
        local_audio_.Initialize();
        controller_.Initialize(&local_audio_);
        power_.RegisterServoOutput(&controller_.servo_output());
        power_.RegisterHoldFeedbackHook(
            [this]() { controller_.TriggerBodyAction(DinoBodyAction::kNodHead); });
        InitializeTouch();
        InitializeSensors();
        // 关机序列：断电前把舵机放回默认安全姿态。
        power_.RegisterShutdownHook([this]() {
            // 先停本地声音，再把舵机放回默认安全姿态。
            local_audio_.StopPlayback();
            controller_.SetDefaultPose();
        });
        controller_.RegisterMcpTools();
        // 音频管理工具（列表/试听/删除/擦除/状态），Web 控制台取消后的入口。
        wzh::RegisterAudioMcpTools("dinosaur", &local_audio_.audio_store(), &local_audio_);
        // 云端音频按需访问：不自动联网，管理页点击后才检查/更新。
        audio_hub_.ConfigureForFactory(&local_audio_.audio_store(), [this]() {
            return controller_.IsMaintenanceBusy() || local_audio_.LocalCommandEchoRisk();
        });
        // 本地管理页：设备热点 192.168.4.1:8080，STA 保持联网。
        wzh::WebConsole::Options console;
        console.store = &local_audio_.audio_store();
        console.player = &local_audio_;
        console.hub = &audio_hub_;
        console.product_name = "恐龙";
        console.build_name = "wzh-dinosaur";
        console.ap_prefix = PANDA_AP_PREFIX;
        console.maintenance = [this](bool enabled) {
            const auto state = Application::GetInstance().GetDeviceState();
            const bool system = state == kDeviceStateStarting || state == kDeviceStateUpgrading ||
                                state == kDeviceStateFatalError ||
                                state == kDeviceStateAudioTesting;
            controller_.SetMaintenance(enabled || system || web_console_.maintenance_active() ||
                                       Application::GetInstance().IsVoiceEnrollmentActive());
        };
        console.live_battery_level = [this](int& level) {
            const auto sample = power_.battery_sampling();
            if (!sample.valid || sample.live_mv <= 0)
                return false;
            level = wzh::BatteryLevelFromVoltage(sample.live_mv, BATTERY_EMPTY_VOLTAGE_MV,
                                                 BATTERY_FULL_VOLTAGE_MV);
            return true;
        };
        console.center = [this]() { return controller_.RequestCenter(); };
        wzh::ConfigureVoiceConsole(console);
        console.status_extra = [this](cJSON* root) {
            wzh::AddPowerTelemetry(root, power_.battery_voltage_mv(), power_.touch_code(),
                                   power_.touch_voltage_mv());
            wzh::AddBatterySamplingTelemetry(root, power_.battery_sampling());
            if (auto* sensors = cJSON_AddObjectToObject(root, "sensors"))
                sensors_.AddStatus(sensors);
            cJSON_AddBoolToObject(root, "conversation", controller_.IsConversationActive());
            cJSON_AddBoolToObject(root, "servo_powered", controller_.servo_output().powered());
            cJSON_AddNumberToObject(root, "servo_faults",
                                    static_cast<double>(controller_.servo_output().faults()));
        };
        web_console_.Configure(std::move(console));
        WifiManager::GetInstance().SetBeforeConfigApHook([this]() { web_console_.Stop(); });
        InitializeConversationMode();
        InitializeButton();
        wzh::StartIdlePower([this]() { power_.RequestShutdown(); });
    }

    AudioCodec* GetAudioCodec() override {
#if CONFIG_USE_VOICE_BARGE_IN
        constexpr bool kDacReference = true;
#else
        constexpr bool kDacReference = false;
#endif
        // 延迟构造 codec，保证 codec_i2c_bus_ 已由构造函数创建。
        // 在线录音、TTS 和离线声音都复用这个唯一实例。
        static Es8311AudioCodec codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, false, false,
            AUDIO_CODEC_PA_VOLTAGE, kDacReference, AUDIO_CODEC_PA_GAIN_DB,
            AUDIO_CODEC_MAX_VOLUME_DB, AUDIO_CODEC_SPEAKER_MAKEUP_DB, 100,
            AUDIO_CODEC_REFERENCE_MIC_GAIN_DB);
        return &codec;
    }

    bool SupportsLocalInteraction() const override { return true; }
    bool TriggerOnboardingFeedback(uint8_t step) override {
        return controller_.TriggerOnboardingFeedback(step);
    }
    bool IsOnboardingFeedbackPending() const override {
        return controller_.IsOnboardingFeedbackPending();
    }

    bool IsLocalPlaybackPending() override {
        return local_audio_.Playing() || !local_audio_.WorkerIdle();
    }

#if CONFIG_USE_VOICE_BARGE_IN && CONFIG_WZH_DINOSAUR_MOTION_BARGE_GUARD
    size_t GetBargeInMinimumSpeechSamples() const override {
        const int64_t last_command_us = controller_.LastServoCommandMovementUs();
        const int64_t now_us = esp_timer_get_time();
        if (controller_.ServoPoweredForBargeIn() && last_command_us > 0 &&
            now_us >= last_command_us && now_us - last_command_us <= 150000) {
            return 4800;  // 300 ms at 16 kHz; experimental, not speech confirmation.
        }
        return 0;
    }
#endif

    bool GetBargeInServoDiagnostics(bool& powered, int64_t& last_command_us) const override {
        powered = controller_.ServoPoweredForBargeIn();
        last_command_us = controller_.LastServoCommandMovementUs();
        return true;
    }

    ExternalTts* GetExternalTts() override { return wzh::ProductExternalTts(); }

    VoiceEnrollment* GetVoiceEnrollment() override { return wzh::ProductVoiceEnrollment(); }

    void SetVoiceEnrollmentRecording(bool recording) override {
        const auto state = Application::GetInstance().GetDeviceState();
        const bool system = state == kDeviceStateStarting || state == kDeviceStateUpgrading ||
                            state == kDeviceStateFatalError || state == kDeviceStateAudioTesting;
        controller_.SetMaintenance(recording || system || web_console_.maintenance_active());
    }

    bool KeepAudioOutputEnabled() const override {
        return Application::GetInstance().IsVoiceEnrollmentActive();
    }

    std::string_view GetConversationExitSound() const override {
        return wzh::ConversationExitSound();
    }

    bool IsAudioMaintenanceActive() override {
        return power_.IsShuttingDown() || web_console_.maintenance_active() ||
               local_audio_.audio_store().IsWriting() || wzh::VoiceManagementBusy();
    }

    // Local commands remain available during provisioning and cloud initialization.
    bool HandleLocalVoiceCommand(const std::string& command) override {
        constexpr const char* prefix = "local:";
        if (command.compare(0, std::strlen(prefix), prefix) != 0) {
            if (wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kWake))
                return true;
            // An ordinary wake phrase is an explicit invitation to resume companionship.
            local_audio_.StopPlayback();
            controller_.HandlePetControl(wzh::PetControl::kWake, false);
            wzh::NotifyOnboarding(wzh::OnboardingEvent::kWake);
            // Talking to it offline is interaction, even without a conversation:
            // otherwise the countdown says goodbye to a child who is playing.
            wzh::NotifyIdleInteraction();
            return false;
        }
        const std::string name = command.substr(std::strlen(prefix));
        // Any recognized offline command is a real interaction for the countdown.
        wzh::NotifyIdleInteraction();
        const auto control = wzh::ParsePetControl(name);
        const bool stop = control == wzh::PetControl::kStop || control == wzh::PetControl::kRest ||
                          control == wzh::PetControl::kQuiet;
        if (!stop && local_audio_.LocalCommandEchoRisk()) {
            ESP_LOGD(TAG, "Local command ignored during speaker output: %s", name.c_str());
            return true;
        }
        if (name == "chat_wake") {
            // Offline chapter: no saved network, so opening a channel fails
            // silently. Stay quiet instead of looking broken, but count it as
            // proof that the wake phrase is understood.
            if (wzh::OnboardingBlocksConversation()) {
                if (wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kWake))
                    return true;
                wzh::NotifyOnboarding(wzh::OnboardingEvent::kWake);
                return true;
            }
            local_audio_.StopPlayback();
            controller_.HandlePetControl(wzh::PetControl::kWake, false);
            Application::GetInstance().WakeWordInvoke(wzh::Product().wake_phrase);
            return true;
        }
        ESP_LOGI(TAG, "Input=local_voice %s", name.c_str());
        if (controller_.HandlePetControl(control)) {
            return true;
        }
        if (controller_.IsConversationActive()) {
            ESP_LOGD(TAG, "Cloud conversation owns joint commands via MCP");
            return true;
        }
        if (name == "shutdown") {
            power_.RequestShutdown();
            return true;
        }
        // Deliver the spoken command even while the cloud conversation owns
        // joint control: the tutorial deliberately waits for a real "摇摇头".
        if (name == "shake_head") {
            if (wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kCommand))
                return true;
            wzh::NotifyOnboarding(wzh::OnboardingEvent::kCommand);
            if (controller_.HandleBodyCommand(name))
                return true;
        }
        if (controller_.HandleBodyCommand(name)) {
            return true;
        } else if (name == "happy" || name == "affection" || name == "eat") {
            const DinoAction action = name == "happy"       ? DinoAction::kHappy
                                      : name == "affection" ? DinoAction::kAffection
                                                            : DinoAction::kEat;
            controller_.TriggerAction(action, !controller_.IsConversationActive());
        }
        return true;
    }

    Display* GetDisplay() override { return &display_; }

    bool GetTemperature(float& celsius) override { return power_.GetTemperature(celsius); }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        level = power_.battery_level();
        // 当前硬件没有接入充电状态检测脚，只能报告电池正在供电。
        charging = false;
        discharging = true;
        return level >= 0;
    }

#if DINOSAUR_ENABLE_4G
    void StartNetwork() override {
        const bool prefer_wifi =
            LoadTransportPreference() == "wifi" && SsidManager::GetInstance().HasSavedNetworks();
        // Reserve the modem task's stack before the portal starts allocating.
        // A Wi-Fi preference still allows detection, without registration.
        if (prefer_wifi)
            cellular_.SetPaused(true);
        const bool started = StartCellularDetection(0);
        // Always bring up the local portal, even before we know which radio
        // can provide the cloud connection.
        WifiBoard::StartNetwork();
        if (prefer_wifi || !started || !ArmModemProbeDeadline(0))
            SelectBootNetwork(NetworkChoice::kWifi);
    }

    NetworkInterface* GetNetwork() override {
        if (network_choice_.load() == NetworkChoice::kProbing)
            return nullptr;
        return UsesCellularNetwork() ? cellular_.GetNetwork() : WifiBoard::GetNetwork();
    }
    bool IsNetworkAvailable() const override {
        if (power_.IsShuttingDown() || network_choice_.load() == NetworkChoice::kProbing)
            return false;
        return UsesCellularNetwork() ? cellular_.IsReady() : WifiBoard::IsNetworkAvailable();
    }
    bool UsesCellularNetwork() const override {
        return network_choice_.load() == NetworkChoice::kCellular;
    }
    // ML307 MQTTPUB accepts at most 1024 payload bytes.
    size_t GetProtocolPayloadLimit() const override { return UsesCellularNetwork() ? 1024 : 0; }
    bool IsNetworkProbing() const override {
        return network_choice_.load() == NetworkChoice::kProbing;
    }
    bool CanSwitchNetworkTransport() const override { return true; }
    bool HasModemTransport() const override { return cellular_.HasDetectedModem(); }
    NetworkTransportSwitchStatus GetNetworkTransportSwitchStatus() const override {
        std::lock_guard<std::mutex> lock(network_switch_mutex_);
        return network_switch_;
    }
    bool SelectNetworkTransport(bool cellular) override {
        if (!CanStartNetworkSwitch())
            return false;
        NetworkTransportSwitchStatus request;
        {
            std::lock_guard<std::mutex> lock(network_switch_mutex_);
            if (network_switch_.state == SwitchState::kPending)
                return network_switch_.cellular == cellular;
            if (IsNetworkProbing())
                return false;
            uint32_t id = network_switch_.id + 1;
            if (id == 0)
                ++id;
            network_switch_ = request = {SwitchState::kPending, cellular, id};
        }
        Application::GetInstance().Schedule([this, request]() { StartRequestedSwitch(request); });
        return true;
    }
    void RequestNetworkRecheck() override {
        if (UsesCellularNetwork())
            cellular_.RequestRecheck();
    }
    bool HasSystemNetwork() const override { return WifiManager::GetInstance().IsConnected(); }
    const char* GetNetworkStateIcon() override {
        if (!UsesCellularNetwork())
            return WifiBoard::GetNetworkStateIcon();
        return cellular_.IsReady() ? MATERIAL_SYMBOLS_SIGNAL_CELLULAR_ALT
                                   : MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF;
    }

    std::string GetBoardJson() override {
        if (!UsesCellularNetwork())
            return WifiBoard::GetBoardJson();
        return std::string("{\"type\":\"") + BOARD_TYPE + "\",\"name\":\"" + BOARD_NAME +
               "\",\"manufacturer\":\"" + BOARD_MANUFACTURER + "\",\"network\":\"cellular\"}";
    }

    std::string GetDeviceStatusJson() override {
        if (!UsesCellularNetwork())
            return WifiBoard::GetDeviceStatusJson();
        auto* root = cJSON_Parse(WifiBoard::GetDeviceStatusJson().c_str());
        if (!root)
            return "{}";
        cJSON_DeleteItemFromObject(root, "network");
        auto* network = cJSON_AddObjectToObject(root, "network");
        if (network) {
            cJSON_AddStringToObject(network, "type", "cellular");
            cJSON_AddBoolToObject(network, "connected", cellular_.IsReady());
        }
        char* text = cJSON_PrintUnformatted(root);
        std::string result = text ? text : "{}";
        cJSON_free(text);
        cJSON_Delete(root);
        return result;
    }
#endif

    void OnLocalPortalReady() override { web_console_.Start(); }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
#if DINOSAUR_ENABLE_4G
        cloud_network_callback_ = std::move(callback);
        WifiBoard::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::Connected)
                web_console_.Start();
            else if (event == NetworkEvent::WifiConfigModeEnter &&
                     !WifiManager::GetInstance().IsPersistentPortalActive())
                web_console_.Stop();
            if (network_choice_.load() != NetworkChoice::kWifi || power_.IsShuttingDown())
                return;
            if (event == NetworkEvent::Connected && wifi_connected_reported_.exchange(true))
                return;
            if (event == NetworkEvent::Disconnected || event == NetworkEvent::WifiConfigModeEnter) {
                wifi_connected_reported_.store(false);
                wzh::NotifyOnboarding(wzh::OnboardingEvent::kNetworkLost);
                controller_.SetConversationActive(false);
            }
            if (cloud_network_callback_)
                cloud_network_callback_(event, data);
        });
#else
        // 在线/离线主要由对话状态决定；断网事件仍应立即强制返回离线，
        // 防止语音通道关闭回调尚未到达时继续保留在线状态。
        WifiBoard::SetNetworkEventCallback([this, callback = std::move(callback)](
                                               NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::Connected) {
                web_console_.Start();
            } else if (event == NetworkEvent::WifiConfigModeEnter) {
                web_console_.Stop();
            }
            if (event == NetworkEvent::Disconnected || event == NetworkEvent::WifiConfigModeEnter) {
                wzh::NotifyOnboarding(wzh::OnboardingEvent::kNetworkLost);
                controller_.SetConversationActive(false);
            }
            if (callback) {
                callback(event, data);
            }
        });
#endif
    }

private:
#if DINOSAUR_ENABLE_4G
    enum class NetworkChoice : uint8_t { kProbing, kWifi, kCellular };
    using SwitchState = NetworkTransportSwitchStatus::State;
    static constexpr int64_t kModemProbeUs = 8000000;
    static constexpr int64_t kCellularRecoveryUs = 15000000;

    bool DeferWifiSetup() const override { return wifi_setup_deferred_.load(); }

    static constexpr const char* kTransportNvs = "network";
    static constexpr const char* kTransportNamespace = "dino_net";
    void SaveTransportPreference(const char* value) {
        Settings settings(kTransportNamespace, true);
        settings.SetString(kTransportNvs, value);
    }
    std::string LoadTransportPreference() {
        Settings settings(kTransportNamespace, false);
        return settings.GetString(kTransportNvs, "");
    }
    bool CanStartNetworkSwitch() const {
        const auto state = Application::GetInstance().GetDeviceState();
        return !power_.IsShuttingDown() && state != kDeviceStateActivating &&
               state != kDeviceStateUpgrading && state != kDeviceStateFatalError;
    }
    bool StartDeadline(esp_timer_handle_t& timer, const char* name, void (*callback)(void*),
                       int64_t delay_us) {
        if (timer == nullptr) {
            esp_timer_create_args_t args{};
            args.arg = this;
            args.name = name;
            args.callback = callback;
            if (esp_timer_create(&args, &timer) != ESP_OK) {
                ESP_LOGE(TAG, "Unable to create network deadline %s", name);
                return false;
            }
        }
        if (esp_timer_start_once(timer, delay_us) == ESP_OK)
            return true;
        ESP_LOGE(TAG, "Unable to start network deadline %s", name);
        return false;
    }
    bool ArmModemProbeDeadline(uint32_t request_id) {
        const auto deadline = esp_timer_get_time() + kModemProbeUs;
        if (!StartDeadline(
                modem_probe_timer_, "dino_probe",
                [](void* arg) {
                    auto* self = static_cast<WzhDinosaur*>(arg);
                    Application::GetInstance().Schedule(
                        [self]() { self->CheckModemProbeDeadline(); });
                },
                kModemProbeUs)) {
            return false;
        }
        modem_probe_deadline_us_ = deadline;
        modem_probe_request_id_ = request_id;
        return true;
    }
    void CancelModemProbeDeadline() {
        if (modem_probe_deadline_us_ != 0) {
            modem_probe_deadline_us_ = 0;
            esp_timer_stop(modem_probe_timer_);
        }
    }
    void CheckModemProbeDeadline() {
        if (modem_probe_deadline_us_ == 0 || esp_timer_get_time() < modem_probe_deadline_us_)
            return;
        ESP_LOGW(TAG, "ML307 hardware probe deadline expired");
        ReportModemDetection(modem_probe_request_id_, cellular_.HasDetectedModem());
    }

    bool StartCellularDetection(uint32_t request_id) {
        return cellular_.Start(
            [this](NetworkEvent event, const std::string& data) {
                Application::GetInstance().Schedule(
                    [this, event, data]() { HandleCellularEvent(event, data); });
            },
            [this, request_id](bool detected) {
                Application::GetInstance().Schedule(
                    [this, request_id, detected]() { ReportModemDetection(request_id, detected); });
            });
    }
    void ReportModemDetection(uint32_t request_id, bool detected) {
        if (power_.IsShuttingDown())
            return;
        if (request_id == 0) {
            if (wifi_setup_deferred_.load())
                SelectBootNetwork(detected ? NetworkChoice::kCellular : NetworkChoice::kWifi);
            return;
        }
        const auto active = GetNetworkTransportSwitchStatus();
        if (active.state != SwitchState::kPending || active.id != request_id)
            return;
        CancelModemProbeDeadline();
        if (!detected || !CanStartNetworkSwitch() ||
            !BeginNetworkSwitch(NetworkChoice::kCellular, request_id)) {
            CompleteTransportRequest(request_id, false);
        }
    }

    void SelectBootNetwork(NetworkChoice choice) {
        if (power_.IsShuttingDown() || !wifi_setup_deferred_.exchange(false))
            return;
        CancelModemProbeDeadline();
        ESP_LOGI(TAG, "Boot transport selected: %s",
                 choice == NetworkChoice::kCellular ? "4G" : "Wi-Fi");
        ApplyNetworkChoice(choice, true);
    }
    void StartRequestedSwitch(NetworkTransportSwitchStatus request) {
        if (!CanStartNetworkSwitch() || IsNetworkProbing()) {
            CompleteTransportRequest(request.id, false);
            return;
        }
        const auto target = request.cellular ? NetworkChoice::kCellular : NetworkChoice::kWifi;
        if (network_choice_.load() == target) {
            // Persist the explicit choice even if this boot already selected it.
            SaveTransportPreference(request.cellular ? "cellular" : "wifi");
            if (request.cellular && !cellular_.IsReady()) {
                cellular_.RequestRecheck();
                ArmCellularFallbackDeadline();
            }
            CompleteTransportRequest(request.id, true);
            return;
        }
        if (request.cellular && !cellular_.HasDetectedModem()) {
            // Probe while the current Wi-Fi connection and conversation remain
            // available. A negative result never tears down the old route.
            if (ArmModemProbeDeadline(request.id) && StartCellularDetection(request.id))
                return;
            CancelModemProbeDeadline();
            CompleteTransportRequest(request.id, false);
            return;
        }
        if (!BeginNetworkSwitch(target, request.id))
            CompleteTransportRequest(request.id, false);
    }
    bool BeginNetworkSwitch(NetworkChoice target, uint32_t request_id) {
        {
            std::lock_guard<std::mutex> lock(network_switch_mutex_);
            if (IsNetworkProbing() ||
                (request_id == 0 && network_switch_.state == SwitchState::kPending))
                return false;
            network_choice_.store(NetworkChoice::kProbing);
        }
        CancelCellularFallbackDeadline();
        if (target == NetworkChoice::kWifi)
            cellular_.SetPaused(true);
        auto& app = Application::GetInstance();
        app.PrepareConversationNetworkSwitch();
        app.ResetProtocol([this, target, request_id]() {
            if (power_.IsShuttingDown()) {
                CompleteTransportRequest(request_id, false);
                return;
            }
            if (request_id == 0 && !SsidManager::GetInstance().HasSavedNetworks()) {
                // The saved configuration can be cleared while the old
                // protocol is closing. Leave the modem selected in that case.
                ApplyNetworkChoice(NetworkChoice::kCellular, false);
                CancelCellularFallbackDeadline();
                AnnounceCellularFailure(false);
                return;
            }
            if (request_id != 0)
                SaveTransportPreference(target == NetworkChoice::kCellular ? "cellular" : "wifi");
            if (request_id == 0)
                AnnounceCellularFailure(true);
            ApplyNetworkChoice(target, false);
            CompleteTransportRequest(request_id, true);
            ESP_LOGI(TAG, "Transport switched to %s",
                     target == NetworkChoice::kCellular ? "4G" : "Wi-Fi");
        });
        return true;
    }
    void CompleteTransportRequest(uint32_t request_id, bool success) {
        if (request_id == 0)
            return;
        std::lock_guard<std::mutex> lock(network_switch_mutex_);
        if (network_switch_.id == request_id && network_switch_.state == SwitchState::kPending)
            network_switch_.state = success ? SwitchState::kSucceeded : SwitchState::kFailed;
    }

    void ApplyNetworkChoice(NetworkChoice choice, bool boot) {
        auto& wifi = WifiManager::GetInstance();
        const bool cellular = choice == NetworkChoice::kCellular;
        // Keep the portal AP up. Reclaim STA memory before registration or
        // transport use because the ML307 AT ring needs that internal SRAM.
        if (cellular) {
            wifi.StopStation();
            wifi_connected_reported_.store(false);
        } else {
            cellular_.SetPaused(true);
            cellular_connected_reported_.store(false);
        }
        network_choice_.store(choice);
        if (cellular)
            cellular_.SetPaused(false);
        if (boot)
            CompleteDeferredWifiSetup();
        else if (!cellular)
            wifi.StartStation();
        if (cellular) {
            if (cellular_.IsReady())
                HandleCellularEvent(NetworkEvent::Connected, "4G");
            else
                ArmCellularFallbackDeadline();
        } else if (wifi.IsConnected() && !wifi_connected_reported_.exchange(true) &&
                   cloud_network_callback_) {
            cloud_network_callback_(NetworkEvent::Connected, wifi.GetSsid());
        }
    }
    void HandleCellularEvent(NetworkEvent event, const std::string& data) {
        if (power_.IsShuttingDown() || !UsesCellularNetwork())
            return;
        if (event == NetworkEvent::Connected) {
            if (!cellular_.IsReady() || cellular_connected_reported_.exchange(true))
                return;
            CancelCellularFallbackDeadline();
        } else if (event == NetworkEvent::Disconnected) {
            if (cellular_.IsReady())
                return;
            cellular_connected_reported_.store(false);
            wzh::NotifyOnboarding(wzh::OnboardingEvent::kNetworkLost);
            controller_.SetConversationActive(false);
            ArmCellularFallbackDeadline();
        } else if (event != NetworkEvent::Connecting) {
            // The deadline owns the offline announcement. A registration
            // error is distinct from failure to detect the modem over UART.
            return;
        }
        if (cloud_network_callback_)
            cloud_network_callback_(event, data);
    }

    void ArmCellularFallbackDeadline() {
        if (cellular_fallback_deadline_us_ != 0)
            return;
        const auto deadline = esp_timer_get_time() + kCellularRecoveryUs;
        if (StartDeadline(
                cellular_fallback_timer_, "dino_fallback",
                [](void* arg) {
                    auto* self = static_cast<WzhDinosaur*>(arg);
                    Application::GetInstance().Schedule(
                        [self]() { self->CheckCellularFallback(); });
                },
                kCellularRecoveryUs)) {
            cellular_fallback_deadline_us_ = deadline;
        }
    }
    void CancelCellularFallbackDeadline() {
        if (cellular_fallback_deadline_us_ != 0) {
            cellular_fallback_deadline_us_ = 0;
            esp_timer_stop(cellular_fallback_timer_);
        }
    }
    void CheckCellularFallback() {
        if (cellular_fallback_deadline_us_ == 0 ||
            esp_timer_get_time() < cellular_fallback_deadline_us_)
            return;
        cellular_fallback_deadline_us_ = 0;
        if (power_.IsShuttingDown() || !UsesCellularNetwork() || cellular_.IsReady())
            return;
        if (!CanStartNetworkSwitch()) {
            ArmCellularFallbackDeadline();
            return;
        }
        if (!SsidManager::GetInstance().HasSavedNetworks()) {
            // Retain the selected cellular route and keep retrying registration.
            // The hotspot can be used to save Wi-Fi credentials.
            AnnounceCellularFailure(false);
        } else if (!BeginNetworkSwitch(NetworkChoice::kWifi, 0)) {
            ArmCellularFallbackDeadline();
        }
    }
    void AnnounceCellularFailure(bool falling_back) {
        extern const char wifi_start[] asm("_binary_cellular_to_wifi_ogg_start");
        extern const char wifi_end[] asm("_binary_cellular_to_wifi_ogg_end");
        extern const char offline_start[] asm("_binary_cellular_unavailable_ogg_start");
        extern const char offline_end[] asm("_binary_cellular_unavailable_ogg_end");
        const char* message = falling_back ? "4G 暂不可用，正在切换到 Wi-Fi"
                                           : "4G 暂不可用，请连接设备热点配置 Wi-Fi";
        const std::string_view sound =
            falling_back ? std::string_view(wifi_start, wifi_end - wifi_start)
                         : std::string_view(offline_start, offline_end - offline_start);
        Application::GetInstance().Alert("4G 暂不可用", message, "warning", sound);
    }

    std::atomic<NetworkChoice> network_choice_{NetworkChoice::kProbing};
    std::atomic<bool> wifi_setup_deferred_{true};
    std::atomic<bool> wifi_connected_reported_{false};
    std::atomic<bool> cellular_connected_reported_{false};
    mutable std::mutex network_switch_mutex_;
    NetworkTransportSwitchStatus network_switch_;
    esp_timer_handle_t modem_probe_timer_ = nullptr;
    uint32_t modem_probe_request_id_ = 0;
    int64_t modem_probe_deadline_us_ = 0;
    esp_timer_handle_t cellular_fallback_timer_ = nullptr;
    int64_t cellular_fallback_deadline_us_ = 0;
    DinosaurCellular cellular_;
    NetworkEventCallback cloud_network_callback_;
#endif
    bool product_initialized_ = false;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;
    DinosaurPower power_;
    DinosaurAudio local_audio_;
    wzh::AudioHub audio_hub_;
    wzh::WebConsole web_console_;
    wzh::Sensors sensors_;
    DinosaurController controller_;
    DinosaurDisplay display_;

    void InitializeSensors() {
        sensors_.Initialize(
            HUMAN_SENSOR_GPIO, POSTURE_I2C_SDA_GPIO, POSTURE_I2C_SCL_GPIO,
            &controller_.servo_output(), [this](wzh::Sensors::Event event) {
                const bool tutorial_motion = event == wzh::Sensors::Event::kPickedUp ||
                                             event == wzh::Sensors::Event::kShaken;
                if (tutorial_motion &&
                    wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kMotion)) {
                    wzh::NotifyIdleInteraction();
                    return true;
                }
                const bool accepted = controller_.HandleSensorEvent(static_cast<int>(event));
                if (accepted && tutorial_motion)
                    wzh::NotifyOnboarding(wzh::OnboardingEvent::kMotion);
                if (accepted || event == wzh::Sensors::Event::kHuman)
                    wzh::NotifyIdleInteraction();
                return accepted;
            });
        sensors_.RegisterMcpTools("dinosaur");
    }

    void InitializeTouch() {
        power_.RegisterTouchHook([this](uint8_t code, bool starts_contact) {
            const bool captured =
                starts_contact && wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch);
            if (starts_contact && !captured)
                wzh::NotifyOnboarding(wzh::OnboardingEvent::kTouch);
            controller_.HandleTouch(code, starts_contact && !captured);
        });
    }

    void InitializeConversationMode() {
        // Wi-Fi 连着但没有说话时也属于本地互动等待。只有建立会话、收音
        // 或播放服务端回复期间，才暂停本地声音并进入在线情绪模式。
        // speaking 状态单独上报，让 TTS 播放期间恐龙能随语音摆尾。
        Application::GetInstance().AddDeviceStateChangeListener(
            [this](DeviceState old_state, DeviceState new_state) {
                if (old_state == kDeviceStateIdle && new_state == kDeviceStateConnecting) {
                    controller_.HandlePetControl(wzh::PetControl::kWake, false);
                }
                const bool maintenance =
                    new_state == kDeviceStateUpgrading || new_state == kDeviceStateStarting ||
                    new_state == kDeviceStateFatalError || new_state == kDeviceStateAudioTesting;
                controller_.SetMaintenance(maintenance || web_console_.maintenance_active());
                const bool active =
                    new_state == kDeviceStateConnecting || new_state == kDeviceStateListening ||
                    new_state == kDeviceStateSpeaking || new_state == kDeviceStateAudioTesting;
                wzh::NotifyIdleInteraction();
                controller_.SetConversationActive(active);
                controller_.SetSpeaking(new_state == kDeviceStateSpeaking);
            });
    }

    void InitializeI2c() {
        // ES8311 独占本板的 I2C0 总线。开启内部上拉仅用于辅助，量产硬件
        // 仍应保留合适的外部上拉电阻。
        i2c_master_bus_config_t config = {};
        config.i2c_port = I2C_NUM_0;
        config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
    }

    void InitializeButton() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // 单击仅在启动/配网阶段打开门户；正常对话统一使用双击。
            if (app.GetDeviceState() == kDeviceStateStarting ||
                app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                EnterWifiConfigMode();
                return;
            }
        });
        boot_button_.OnDoubleClick([]() {
            if (!wzh::OnboardingBlocksConversation()) {
                Application::GetInstance().ToggleConversation();
            }
        });
        ESP_LOGI(TAG,
                 "Boot/power key: double click enters/exits conversation; single click only "
                 "configures Wi-Fi");
    }
};

DECLARE_BOARD(WzhDinosaur);
