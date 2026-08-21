#include <driver/i2c_master.h>
#include <esp_log.h>

#include <cstring>
#include <utility>

#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/display.h"
#include "panda_audio.h"
#include "panda_controller.h"
#include "panda_power.h"
#include "wifi_board.h"

#define TAG "WzhTailRedPanda"

// 这块板没有屏幕，但仍需要接收服务端下发的 llm.emotion。
// 通过继承 NoDisplay 拦截情绪事件，就能在不引入显示硬件的情况下驱动舵机。
class PandaDisplay : public NoDisplay {
public:
    explicit PandaDisplay(PandaController& controller) : controller_(controller) {}

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
        }
    }

private:
    PandaController& controller_;
};

class WzhTailRedPanda : public WifiBoard {
public:
    WzhTailRedPanda() : boot_button_(BOOT_BUTTON_GPIO), display_(controller_) {
        // 初始化顺序有依赖：ES8311 需要 I2C；动作控制器需要先拿到本地
        // 音频对象；MCP 工具最后注册，确保回调触发时控制器已经可用。
        InitializeI2c();
        power_.Initialize();
        local_audio_.Initialize();
        controller_.Initialize(&local_audio_);
        controller_.RegisterMcpTools();
        InitializeConversationMode();
        InitializeButton();
    }

    AudioCodec* GetAudioCodec() override {
        // 延迟构造 codec，保证 codec_i2c_bus_ 已由构造函数创建。
        // 在线录音、TTS 和离线声音都复用这个唯一实例。
        static Es8311AudioCodec codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, false);
        return &codec;
    }

    Display* GetDisplay() override { return &display_; }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        level = power_.battery_level();
        // 当前硬件没有接入充电状态检测脚，只能报告电池正在供电。
        charging = false;
        discharging = true;
        return true;
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        // 在线/离线主要由对话状态决定；断网事件仍应立即强制返回离线，
        // 防止语音通道关闭回调尚未到达时继续保留在线状态。
        WifiBoard::SetNetworkEventCallback([this, callback = std::move(callback)](
                                               NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::Disconnected || event == NetworkEvent::WifiConfigModeEnter) {
                controller_.SetConversationActive(false);
            }
            if (callback) {
                callback(event, data);
            }
        });
    }

private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;
    PandaPower power_;
    PandaAudio local_audio_;
    PandaController controller_;
    PandaDisplay display_;

    void InitializeConversationMode() {
        // Wi-Fi 连着但没有说话时也属于离线自主模式。只有建立会话、收音
        // 或播放服务端回复期间，才暂停本地声音并进入在线情绪模式。
        // speaking 状态单独上报，让 TTS 播放期间小熊猫能随语音轻摆。
        Application::GetInstance().AddDeviceStateChangeListener(
            [this](DeviceState, DeviceState new_state) {
                const bool active =
                    new_state == kDeviceStateConnecting || new_state == kDeviceStateListening ||
                    new_state == kDeviceStateSpeaking || new_state == kDeviceStateAudioTesting;
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
            // 启动/配网阶段优先进入配网，其余状态沿用 XiaoZhi 的按键对话行为。
            if (app.GetDeviceState() == kDeviceStateStarting ||
                app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnDoubleClick([this]() {
            // 在线时只做动作，避免本地声音与服务端 TTS 同时播放。
            controller_.TriggerAction(PandaAction::kHappy, !controller_.IsConversationActive());
        });
        ESP_LOGI(TAG, "Boot key: click to talk/configure, double click for a happy action");
    }
};

DECLARE_BOARD(WzhTailRedPanda);