#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <utility>

#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "dinosaur_audio.h"
#include "dinosaur_controller.h"
#include "dinosaur_power.h"
#include "display/display.h"
#include "wifi_board.h"
#include "wifi_manager.h"
#include "wzh_audio_hub.h"
#include "wzh_conversation_sounds.h"
#include "wzh_idle_power.h"
#include "wzh_system_prompts.h"
#include "wzh_mcp_tools.h"
#include "wzh_onboarding.h"
#include "wzh_power_telemetry.h"
#include "wzh_product.h"
#include "wzh_sensors.h"
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

    // 服务端 llm.emotion 经常缺失，这里拦截用户转写文本做一次本地兜底
    // 情绪判断，让“我今天考试考了满分”这类明显情绪立刻驱动身体语言。
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
    WzhDinosaur() : boot_button_(BOOT_BUTTON_GPIO), display_(controller_) {
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
        audio_hub_.StartManual(&local_audio_.audio_store(), [this]() {
            return controller_.IsMaintenanceBusy() || local_audio_.LocalCommandEchoRisk();
        });
        wzh::RegisterHubMcpTools("dinosaur", &audio_hub_);
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
        console.center = [this]() { return controller_.RequestCenter(); };
        wzh::ConfigureVoiceConsole(console);
        console.status_extra = [this](cJSON* root) {
            wzh::AddPowerTelemetry(root, power_.battery_voltage_mv(), power_.touch_valid(),
                                   power_.touch_code(), power_.touch_voltage_mv());
            wzh::AddBatterySamplingTelemetry(root, power_.battery_sampling());
            cJSON_AddBoolToObject(root, "conversation", controller_.IsConversationActive());
            cJSON_AddBoolToObject(root, "servo_powered", controller_.servo_output().powered());
            cJSON_AddNumberToObject(root, "servo_faults",
                                    static_cast<double>(controller_.servo_output().faults()));
        };
        web_console_.Configure(std::move(console));
        WifiManager::GetInstance().SetBeforeConfigApHook([this]() { web_console_.Stop(); });
        InitializeConversationMode();
        InitializeButton();
        wzh::StartIdlePower([this]() { power_.RequestAutoShutdown(); });
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
            AUDIO_CODEC_MAX_VOLUME_DB, AUDIO_CODEC_SPEAKER_MAKEUP_DB, 100);
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

    void OnConversationChannelOpened() override {
        wzh::NotifyOnboarding(wzh::OnboardingEvent::kServiceReady);
        if (wzh::ConsumeOnboardingConnectionProbe()) {
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().StopInteraction(false);
            });
        }
    }

#if CONFIG_USE_VOICE_BARGE_IN && CONFIG_WZH_DINOSAUR_MOTION_BARGE_GUARD
    size_t GetBargeInMinimumSpeechSamples() const override {
        const int64_t last_command_us = controller_.LastServoCommandMovementUs();
        const int64_t now_us = esp_timer_get_time();
        if (controller_.ServoPoweredForBargeIn() && last_command_us > 0 &&
            now_us >= last_command_us && now_us - last_command_us <= 150000) {
            return 4800;  // 300 ms at 16 kHz; experimental, not speech confirmation.
        }
        return 2880;
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
        return power_.battery_level_valid();
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
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
    }

private:
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
        sensors_.Initialize(HUMAN_SENSOR_GPIO, POSTURE_I2C_SDA_GPIO, POSTURE_I2C_SCL_GPIO,
                            &controller_.servo_output(), [this](wzh::Sensors::Event event) {
                                if (event == wzh::Sensors::Event::kPickedUp ||
                                    event == wzh::Sensors::Event::kShaken) {
                                    if (wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kMotion)) {
                                        wzh::NotifyIdleInteraction();
                                        return true;
                                    }
                                    wzh::NotifyOnboarding(wzh::OnboardingEvent::kMotion);
                                    wzh::NotifyIdleInteraction();
                                }
                                if (event == wzh::Sensors::Event::kHuman)
                                    wzh::NotifyIdleInteraction();
                                return controller_.HandleSensorEvent(static_cast<int>(event));
                            });
        sensors_.RegisterMcpTools("dinosaur");
    }

    void InitializeTouch() {
        power_.RegisterTouchHook([this](uint8_t code, uint8_t pressed, uint8_t) {
            if (pressed) {
                if (wzh::CaptureOnboardingInteraction(wzh::OnboardingEvent::kTouch))
                    return;
                wzh::NotifyOnboarding(wzh::OnboardingEvent::kTouch);
            }
            controller_.HandleTouch(code);
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
                wzh::PrepareOnboardingDoubleClick();
                Application::GetInstance().ToggleConversation();
            }
        });
        ESP_LOGI(TAG,
                 "Boot/power key: double click enters/exits conversation; single click only "
                 "configures Wi-Fi");
    }
};

DECLARE_BOARD(WzhDinosaur);
