#include "application.h"
#include <new>
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "external_tts.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "voice_enrollment.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>

#define TAG "Application"

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

int Application::AddDeviceStateChangeListener(DeviceStateMachine::StateCallback callback) {
    return state_machine_.AddStateChangeListener(std::move(callback));
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_processed_audio = [this](const int16_t* pcm, size_t count, bool speech) {
        if (!voice_enrollment_active_.load())
            return false;
        if (auto* enrollment = Board::GetInstance().GetVoiceEnrollment())
            enrollment->Capture(pcm, count, speech);
        return true;
    };
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
#if CONFIG_USE_VOICE_BARGE_IN
    callbacks.on_voice_interruption = [this](uint32_t token) {
        pending_voice_interruption_.store(token);
#if !CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        xEventGroupSetBits(event_group_, MAIN_EVENT_VOICE_INTERRUPTION);
#endif
    };
#endif
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Local models must be available before the first successful network connection.
    // A pending resource update keeps the normal update-before-model-init path.
    if (board.SupportsLocalInteraction()) {
        Settings asset_settings("assets", false);
        if (asset_settings.GetString("download_url").empty()) {
            local_assets_applied_ = Assets::GetInstance().Apply();
            audio_service_.EnableWakeWordDetection(local_assets_applied_);
        }
    }

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_TOGGLE_CONVERSATION |
        MAIN_EVENT_VOICE_INTERRUPTION;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if ((bits & MAIN_EVENT_ERROR) && !interaction_stopped_.load()) {
            if (voice_enrollment_active_.load()) {
                // A queued conversation-server error must not cancel an Ali
                // enrollment; Wi-Fi disconnect has its own cancellation path.
                ESP_LOGW(TAG, "Ignoring conversation error during voice enrollment");
            } else {
                SetDeviceState(kDeviceStateIdle);
                Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                      Lang::Sounds::OGG_EXCLAMATION);
            }
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CONVERSATION) {
            HandleToggleConversationEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

#if CONFIG_USE_VOICE_BARGE_IN
        if (bits & MAIN_EVENT_VOICE_INTERRUPTION) {
            HandleVoiceInterruptionEvent();
        }
#endif
        if ((bits & MAIN_EVENT_SEND_AUDIO) && !barge_in_reconnecting_.load()) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                // A pending stop must not leak uplink frames produced before
                // the microphone engine actually stopped.
                if (interaction_stopped_.load() || voice_enrollment_active_.load()) {
                    continue;
                }
                const bool sent = protocol_ && protocol_->SendAudio(std::move(packet));
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
                if (official_probe_active_.load() && !official_probe_sent_stop_) {
                    if (sent)
                        ++official_probe_packets_sent_;
                    else
                        ++official_probe_packets_failed_;
                }
#endif
                if (protocol_ && !sent) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
#if CONFIG_USE_VOICE_BARGE_IN
            audio_service_.ReleaseVoiceInterruptionBuffer();
#endif
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
            ServiceOfficialAsrProbe();
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
        || (state == kDeviceStateIdle && !protocol_)
#endif
    ) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
    if (official_probe_active_.exchange(false))
        audio_service_.EnableVoiceProcessing(false);
#endif
    voice_selection_reply_guard_.store(false);
    if (external_voice_preview_active_.exchange(false))
        external_voice_preview_status_.store(4);
    const bool stopped_enrollment = voice_enrollment_active_.exchange(false);
    if (stopped_enrollment) {
        Board::GetInstance().SetVoiceEnrollmentRecording(false);
        interaction_generation_.fetch_add(1);
        audio_service_.EnableVoiceProcessing(false);
    }
    if (auto* enrollment = Board::GetInstance().GetVoiceEnrollment())
        enrollment->Cancel();
    if (external_tts_selected_.exchange(false)) {
        external_tts_turn_.fetch_add(1);
        if (auto* external = Board::GetInstance().GetExternalTts())
            external->Cancel();
        audio_service_.ResetDecoder();
    }
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        if (voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load()) {
            if (barge_in_reconnecting_.load()) {
                interaction_generation_.fetch_add(1);
                audio_service_.EnableVoiceProcessing(false);
            }
            SetDeviceState(kDeviceStateIdle);
        } else if (protocol_) {
            protocol_->CloseAudioChannel();
        }
        if (stopped_enrollment)
            SetDeviceState(kDeviceStateIdle);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_ || local_assets_applied_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets before starting the recognition engine on the update path.
    const bool applied = assets.Apply();
    if (board.SupportsLocalInteraction()) {
        local_assets_applied_.store(applied);
        Schedule([this]() {
            const auto state = GetDeviceState();
            if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
                state == kDeviceStateActivating) {
                audio_service_.EnableWakeWordDetection(local_assets_applied_.load());
            }
        });
    }
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    const auto protocol_generation = protocol_generation_.fetch_add(1) + 1;
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this, protocol_generation]() {
        Schedule([this, protocol_generation]() {
            if (protocol_generation == protocol_generation_.load())
                DismissAlert();
        });
    });

    protocol_->OnNetworkError([this, protocol_generation](const std::string& message) {
        if (protocol_generation != protocol_generation_.load())
            return;
        Schedule([this, protocol_generation, message]() {
            if (protocol_generation != protocol_generation_.load() || interaction_stopped_.load())
                return;
            if (voice_enrollment_active_.load() || barge_in_reconnecting_.load()) {
                ESP_LOGW(TAG, "Conversation service unavailable during reconnect");
                return;
            }
            last_error_message_ = message;
            xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        });
    });

    protocol_->OnIncomingAudio([this,
                                protocol_generation](std::unique_ptr<AudioStreamPacket> packet) {
        if (protocol_generation != protocol_generation_.load())
            return;
        if (interaction_stopped_.load() || external_tts_selected_.load() ||
            voice_enrollment_active_.load() || voice_selection_reply_guard_.load())
            return;
#if CONFIG_USE_VOICE_BARGE_IN
        if (barge_in_reply_gate_.blocked()) {
            return;
        }
#endif
        const uint32_t generation = audio_service_.GetPlaybackGeneration();
        std::lock_guard<std::mutex> lock(early_audio_mutex_);
        if (protocol_generation != protocol_generation_.load() || external_tts_selected_.load() ||
            voice_enrollment_active_.load() || voice_selection_reply_guard_.load())
            return;
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet), false, generation);
        } else if (tts_start_pending_.load() && early_audio_.size() < kMaxEarlyAudioPackets) {
            // The TTS start transition is queued on the main task; keep the
            // first packets so the first syllable is not lost.
            early_audio_.push_back(std::move(packet));
        }
    });

    auto* callback_protocol = protocol_.get();
    protocol_->OnAudioChannelOpened(
        [this, protocol_generation, callback_protocol, codec, &board]() {
            if (protocol_generation != protocol_generation_.load())
                return;
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            if (callback_protocol->server_sample_rate() != codec->output_sample_rate()) {
                ESP_LOGW(TAG,
                         "Server sample rate %d does not match device output sample rate %d, "
                         "resampling may cause distortion",
                         callback_protocol->server_sample_rate(), codec->output_sample_rate());
            }
        });

    protocol_->OnAudioChannelClosed([this, &board, protocol_generation]() {
        if (protocol_generation != protocol_generation_.load())
            return;
        const auto generation = interaction_generation_.load();
        Schedule([this, &board, generation, protocol_generation]() {
            if (protocol_generation != protocol_generation_.load())
                return;
            if (voice_enrollment_active_.load() || barge_in_reconnecting_.load())
                return;
            // A delayed close from the previous session must not end a new one.
            if (generation != interaction_generation_.load() ||
                (protocol_ && protocol_->IsAudioChannelOpened()))
                return;
            if (!interaction_stopped_.load() && GetDeviceState() == kDeviceStateListening &&
                esp_timer_get_time() < voice_enrollment_resume_guard_until_us_) {
                voice_enrollment_resume_guard_until_us_ = 0;
                voice_enrollment_active_.store(true);
                audio_service_.EnableVoiceProcessing(false);
                SetDeviceState(kDeviceStateSpeaking);
                ESP_LOGI(TAG, "Reopening conversation after a delayed enrollment goodbye");
                ResumeAfterVoiceEnrollment(generation);
                return;
            }
            if (external_tts_selected_.load() && !interaction_stopped_.load() &&
                GetDeviceState() == kDeviceStateSpeaking) {
                if (auto* external = board.GetExternalTts()) {
                    // A server goodbye may follow its original TTS long before
                    // the independently requested replacement audio is ready.
                    // Finish is idempotent and also covers a missing TTS stop.
                    ESP_LOGI(TAG, "Audio channel closed; waiting for external speech to finish");
                    external->Finish();
                    return;
                }
            }
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display, protocol_generation](const cJSON* root) {
        const auto callback_generation = interaction_generation_.load();
        if (protocol_generation != protocol_generation_.load())
            return;
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
        if (official_probe_active_.load()) {
            if (strcmp(type->valuestring, "stt") == 0) {
                const auto* text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text) && text->valuestring != nullptr && text->valuestring[0]) {
                    ESP_LOGI(TAG, "Official ASR probe transcript playback_busy=%d: %s",
                             !audio_service_.IsPlaybackIdle(), text->valuestring);
                    official_probe_stt_received_.store(true);
                } else {
                    ESP_LOGI(TAG, "Official ASR probe received empty transcript");
                }
                return;
            }
            if (strcmp(type->valuestring, "tts") == 0) {
                const auto* state = cJSON_GetObjectItem(root, "state");
                ESP_LOGI(TAG, "Official ASR probe server TTS: %s",
                         cJSON_IsString(state) && state->valuestring ? state->valuestring : "unknown");
            }
            if (strcmp(type->valuestring, "mcp") == 0) {
                ESP_LOGW(TAG, "Official ASR probe: ignoring device MCP command");
                return;
            }
        }
#endif
        if (voice_enrollment_active_.load() &&
            (strcmp(type->valuestring, "tts") == 0 || strcmp(type->valuestring, "stt") == 0 ||
             strcmp(type->valuestring, "llm") == 0))
            return;
        if (voice_selection_reply_guard_.load() &&
            (strcmp(type->valuestring, "tts") == 0 || strcmp(type->valuestring, "llm") == 0)) {
            ESP_LOGI(TAG, "Ignoring late server reply during local voice selection");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
#if CONFIG_USE_VOICE_BARGE_IN
            if (barge_in_reply_gate_.blocked()) {
                return;
            }
#endif
            const auto generation = callback_generation;
            if (interaction_stopped_.load()) {
                return;
            }
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                auto* external = Board::GetInstance().GetExternalTts();
                const bool replacing_external = external_tts_selected_.exchange(false);
                if (replacing_external && external)
                    external->Cancel();
                external_tts_selected_.store(external && external->Ready());
                const auto external_turn = external_tts_turn_.fetch_add(1) + 1;
                const auto external_token = external_tts_callback_token_.fetch_add(1) + 1;
                tts_start_pending_.store(true);
                Schedule([this, generation, external_turn, external_token, replacing_external]() {
                    if (interaction_stopped_.load() ||
                        generation != interaction_generation_.load() ||
                        external_turn != external_tts_turn_.load() ||
                        GetDeviceState() == kDeviceStateIdle) {
                        if (external_turn == external_tts_turn_.load()) {
                            external_tts_selected_.store(false);
                            ClearEarlyAudio();
                        }
                        return;
                    }
                    // Reset and enter Speaking while holding the early-audio
                    // lock, then replay the buffered prefix before any newer
                    // packet can be admitted.
                    if (replacing_external)
                        audio_service_.ResetDecoder();
                    server_music_active_ = false;
                    if (external_tts_selected_.load())
                        ClearEarlyAudio();
                    FlushEarlyAudio();
                    if (external_tts_selected_.load()) {
                        auto* external = Board::GetInstance().GetExternalTts();
                        const auto playback_generation = audio_service_.GetPlaybackGeneration();
                        if (!external ||
                            !external->Begin(playback_generation, [this, generation, external_turn,
                                                                   external_token](bool ok) {
                                Schedule([this, generation, external_turn, external_token, ok]() {
                                    if (external_turn != external_tts_turn_.load() ||
                                        external_token != external_tts_callback_token_.load() ||
                                        interaction_stopped_.load() ||
                                        generation != interaction_generation_.load() ||
                                        GetDeviceState() != kDeviceStateSpeaking ||
                                        !external_tts_selected_.exchange(false))
                                        return;
                                    if (!ok)
                                        ESP_LOGW(TAG,
                                                 "External voice synthesis failed; next turn "
                                                 "uses the original server voice");
                                    const bool channel_closed =
                                        !protocol_ || !protocol_->IsAudioChannelOpened();
                                    if (channel_closed) {
                                        auto& board = Board::GetInstance();
                                        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                                        board.GetDisplay()->SetChatMessage("system", "");
                                    }
                                    SetDeviceState(channel_closed || listening_mode_ ==
                                                                         kListeningModeManualStop
                                                       ? kDeviceStateIdle
                                                       : kDeviceStateListening);
                                });
                            })) {
                            external_tts_selected_.store(false);
                            ESP_LOGW(TAG, "External voice task unavailable");
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                // Preserve the same reply's buffered prefix until its scheduled start runs.
                const auto external_turn = external_tts_turn_.load();
                Schedule([this, generation, external_turn]() {
                    if (GetDeviceState() == kDeviceStateSpeaking && !interaction_stopped_.load() &&
                        generation == interaction_generation_.load() &&
                        external_turn == external_tts_turn_.load()) {
                        if (external_tts_selected_.load()) {
                            if (auto* external = Board::GetInstance().GetExternalTts()) {
                                external->Finish();
                                return;
                            }
                        }
#if CONFIG_AUDIO_PRODUCT_QUALITY
                        audio_service_.FinishPlayback();
#endif
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    const auto external_turn = external_tts_turn_.load();
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, generation, external_turn,
                              message = std::string(text->valuestring), glyphs = std::move(glyphs),
                              bpp]() {
                        if (interaction_stopped_.load() ||
                            generation != interaction_generation_.load() ||
                            external_turn != external_tts_turn_.load()) {
                            return;
                        }
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                        if (external_tts_selected_.load()) {
                            if (auto* external = Board::GetInstance().GetExternalTts()) {
                                if (external->ShouldUseServerAudio(message)) {
                                    // Stop replacement playback and clear its queued PCM before
                                    // allowing the original song packets into the audio path.
                                    external->Cancel();
                                    external_tts_callback_token_.fetch_add(1);
                                    audio_service_.ResetDecoder();
                                    external_tts_selected_.store(false);
                                    server_music_active_ = true;
                                    ESP_LOGI(TAG, "Music started; switching to server audio");
                                    return;
                                }
                                external->Enqueue(message);
                            }
                        } else if (server_music_active_) {
                            auto* external = Board::GetInstance().GetExternalTts();
                            if (external && external->ShouldResumeExternalAudio(message)) {
                                if (!external->Ready()) {
                                    ESP_LOGW(
                                        TAG,
                                        "Music end recognized but external speech is not ready");
                                    return;
                                }
                                // Stop admitting original packets before resetting the decoder;
                                // the ending comment belongs to the clone, not the song.
                                std::lock_guard<std::mutex> lock(early_audio_mutex_);
                                external_tts_selected_.store(true);
                                audio_service_.ResetDecoder();
                                const auto token = external_tts_callback_token_.fetch_add(1) + 1;
                                const auto playback_generation =
                                    audio_service_.GetPlaybackGeneration();
                                if (external->Begin(playback_generation, [this, generation,
                                                                          external_turn,
                                                                          token](bool ok) {
                                        Schedule([this, generation, external_turn, token, ok]() {
                                            if (interaction_stopped_.load() ||
                                                generation != interaction_generation_.load() ||
                                                external_turn != external_tts_turn_.load() ||
                                                token != external_tts_callback_token_.load() ||
                                                GetDeviceState() != kDeviceStateSpeaking ||
                                                !external_tts_selected_.exchange(false))
                                                return;
                                            if (!ok)
                                                ESP_LOGW(TAG,
                                                         "External voice synthesis failed; next "
                                                         "turn uses the original server voice");
                                            const bool channel_closed =
                                                !protocol_ || !protocol_->IsAudioChannelOpened();
                                            if (channel_closed) {
                                                auto& board = Board::GetInstance();
                                                board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                                                board.GetDisplay()->SetChatMessage("system", "");
                                            }
                                            SetDeviceState(channel_closed ||
                                                                   listening_mode_ ==
                                                                       kListeningModeManualStop
                                                               ? kDeviceStateIdle
                                                               : kDeviceStateListening);
                                        });
                                    })) {
                                    server_music_active_ = false;
                                    ESP_LOGI(TAG, "Music finished; resuming external speech");
                                    external->Enqueue(message);
                                } else {
                                    external_tts_selected_.store(false);
                                    ESP_LOGW(TAG, "External speech unavailable after music");
                                }
                            }
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            if (interaction_stopped_.load())
                return;
            const auto generation = interaction_generation_.load();
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring != nullptr) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
#if CONFIG_USE_VOICE_BARGE_IN
                if (text->valuestring[0] != '\0') {
                    barge_in_reply_gate_.OnTranscript();
                }
#endif
                const std::string recognized(text->valuestring);
                auto* enrollment = Board::GetInstance().GetVoiceEnrollment();
                const auto current_state = GetDeviceState();
                const bool clone_request = IsVoiceEnrollmentRequest(recognized);
                const bool selection_request = IsVoiceSelectionRequest(recognized);
                const bool selection_pending = enrollment && enrollment->SelectionPending();
                const bool voice_operation_configured =
                    !enrollment || !(clone_request || selection_request || selection_pending) ||
                    enrollment->IsConfigured();
                if (!voice_operation_configured) {
                    ESP_LOGW(TAG, "Voice operation requested but Ali API key is not configured");
                }
                if (enrollment &&
                    (current_state == kDeviceStateListening ||
                     current_state == kDeviceStateSpeaking) &&
                    (clone_request || selection_request || selection_pending)) {
                    if (voice_enrollment_active_.exchange(true))
                        return;
                    voice_selection_reply_guard_.store(true);
                    voice_enrollment_resume_guard_until_us_ = 0;
                    const auto token = interaction_generation_.fetch_add(1) + 1;
                    Schedule([this, enrollment, token, clone_request, selection_pending, recognized,
                              voice_operation_configured]() {
                        if (!voice_enrollment_active_.load() || interaction_stopped_.load() ||
                            token != interaction_generation_.load())
                            return;
                        AbortSpeaking(kAbortReasonNone);
                        ClearEarlyAudio();
                        audio_service_.ResetDecoder();
                        if (protocol_ && protocol_->IsAudioChannelOpened())
                            protocol_->SendStopListening();
                        // The server already received the audio used for STT. End
                        // this session so its pending reply is not reused after the
                        // local voice selection prompt finishes.
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            ESP_LOGI(TAG, "Closing old conversation for local voice operation");
                            protocol_->CloseAudioChannel();
                        }
                        SetDeviceState(kDeviceStateSpeaking);
                        if (!voice_operation_configured) {
                            Alert(Lang::Strings::ERROR,
                                  "音色服务未配置，请在设备管理页面设置阿里百炼 API Key。",
                                  "warning", Lang::Sounds::OGG_EXCLAMATION);
                            ResumeAfterVoiceEnrollment(token);
                            return;
                        }
                        ESP_LOGI(TAG, "Local voice operation: %s",
                                 clone_request ? "enrollment" : "selection");
                        auto capture = [this, token](bool enable) {
                            Schedule([this, token, enable]() {
                                if (voice_enrollment_active_.load() &&
                                    token == interaction_generation_.load()) {
                                    Board::GetInstance().SetVoiceEnrollmentRecording(enable);
                                    if (enable)
                                        audio_service_.EnableWakeWordDetection(false);
                                    audio_service_.EnableVoiceProcessing(enable);
                                    if (!enable)
                                        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
                                }
                            });
                        };
                        auto completion = [this, token](bool ok) {
                            Schedule([this, token, ok]() {
                                if (!voice_enrollment_active_.load() ||
                                    token != interaction_generation_.load())
                                    return;
                                ESP_LOGI(TAG, "Voice operation %s", ok ? "complete" : "failed");
                                audio_service_.EnableVoiceProcessing(false);
                                ResumeAfterVoiceEnrollment(token);
                            });
                        };
                        const bool started =
                            clone_request ? enrollment->Start(capture, completion)
                                          : enrollment->StartSelection(
                                                selection_pending ? recognized : "切换音色", completion);
                        if (!started) {
                            ESP_LOGW(TAG,
                                     "Voice operation unavailable: check Ali settings or memory");
                            ResumeAfterVoiceEnrollment(token);
                        }
                    });
                    return;
                }
                if (voice_selection_reply_guard_.exchange(false))
                    ESP_LOGI(TAG, "Accepting next user turn after local voice operation");
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                Schedule([this, display, generation, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    if (interaction_stopped_.load() || generation != interaction_generation_.load())
                        return;
                    voice_enrollment_resume_guard_until_us_ = 0;
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
#if CONFIG_USE_VOICE_BARGE_IN
            if (barge_in_reply_gate_.blocked()) {
                return;
            }
#endif
            if (interaction_stopped_.load())
                return;
            const auto generation = interaction_generation_.load();
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, generation,
                          emotion_str = std::string(emotion->valuestring)]() {
                    if (interaction_stopped_.load() || generation != interaction_generation_.load())
                        return;
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            char* message_json = cJSON_PrintUnformatted(root);
            if (message_json != nullptr) {
                ESP_LOGI(TAG, "Received custom message: %s", message_json);
                cJSON_free(message_json);
            }
            if (cJSON_IsObject(payload)) {
                char* payload_json = cJSON_PrintUnformatted(payload);
                if (payload_json == nullptr) {
                    ESP_LOGW(TAG, "Failed to serialize custom payload");
                    return;
                }
                std::string payload_str(payload_json);
                cJSON_free(payload_json);
                Schedule([display, payload_str = std::move(payload_str)]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::ToggleConversation() {
    const auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        // Reject late TTS immediately, including while OpenAudioChannel is busy.
        StopInteraction(true);
        return;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CONVERSATION);
}

void Application::HandleToggleConversationEvent() {
    if ((voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load()) &&
        GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGW(TAG, "Waiting for the previous conversation reconnect to stop");
        return;
    }
    if (Board::GetInstance().IsAudioMaintenanceActive())
        return;
    const auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        StopInteraction(true);
        return;
    }
    if (state != kDeviceStateIdle || !protocol_)
        return;
    interaction_generation_.fetch_add(1);
    interaction_stopped_.store(false);
    play_popup_on_listening_ = true;
    entry_prompt_before_listening_ = true;
    const auto mode = GetDefaultListeningMode();
    // Always pass through Connecting: the board stops local Flash playback
    // before any prompt or voice input is started, even on an open channel.
    if (SetDeviceState(kDeviceStateConnecting)) {
        Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
    }
}

void Application::HandleToggleChatEvent() {
    if ((voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load()) &&
        GetDeviceState() == kDeviceStateIdle)
        return;
    if (Board::GetInstance().IsAudioMaintenanceActive())
        return;
    auto state = GetDeviceState();
    entry_prompt_before_listening_ = false;

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        interaction_generation_.fetch_add(1);
        interaction_stopped_.store(false);
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting || interaction_stopped_.load()) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            entry_prompt_before_listening_ = false;
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    if (interaction_stopped_.load()) {
        if (protocol_->IsAudioChannelOpened())
            protocol_->CloseAudioChannel();
        play_popup_on_listening_ = false;
        entry_prompt_before_listening_ = false;
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    if (voice_enrollment_active_.load())
        return;
    if ((voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load()) &&
        GetDeviceState() == kDeviceStateIdle)
        return;
    if (Board::GetInstance().IsAudioMaintenanceActive())
        return;
    auto state = GetDeviceState();
    entry_prompt_before_listening_ = false;

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        interaction_generation_.fetch_add(1);
        interaction_stopped_.store(false);
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    if (voice_enrollment_active_.load())
        return;
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (voice_enrollment_active_.load())
        return;
    if (Board::GetInstance().IsAudioMaintenanceActive())
        return;
    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
#if CONFIG_USE_VOICE_BARGE_IN
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        const bool keyword =
            wake_word == "local:interrupt" || wake_word == "local:stop" || wake_word == "小龙小龙";
        if (keyword && audio_service_.IsVoiceInterruptionMonitoring()) {
            const auto token = pending_voice_interruption_.load();
            if (audio_service_.IsVoiceInterruptionPending(token)) {
                ESP_LOGI(TAG, "Dino keyword interruption accepted: %s", wake_word.c_str());
                HandleVoiceInterruptionEvent();
            } else {
                ESP_LOGI(TAG, "Dino keyword ignored: no recent near-end voice candidate");
            }
        }
#endif
        // WakeNet can recognize the device's own spoken wake phrase. During an
        // active conversation it must not bypass reference/AEC/near-end checks,
        // reset capture, or reopen a blocked old reply. Keep ordinary standby
        // wake-up; confirmed voice interruption owns cancellation in conversation.
        // Also cover Listening while buffered playback or its wake event drains.
        audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#if !CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        ESP_LOGI(TAG,
                 "Conversation wake word ignored: interruption requires near-end confirmation");
#endif
        return;
    }
#endif
    // Boards can handle offline commands even before a protocol is available.
    if (Board::GetInstance().HandleLocalVoiceCommand(wake_word)) {
        const auto current = GetDeviceState();
        const bool local = Board::GetInstance().SupportsLocalInteraction();
        if (current == kDeviceStateListening && !local) {
            ConfigureWakeWordForListening();
        } else {
            audio_service_.EnableWakeWordDetection(
                current == kDeviceStateIdle ||
                (current == kDeviceStateSpeaking && audio_service_.IsAfeWakeWord()) ||
                (local &&
                 (current == kDeviceStateStarting || current == kDeviceStateWifiConfiguring ||
                  current == kDeviceStateActivating || current == kDeviceStateListening)));
        }
        return;
    }
    if (!protocol_) {
        audio_service_.EnableWakeWordDetection(Board::GetInstance().SupportsLocalInteraction());
        return;
    }
    if (Board::GetInstance().SupportsLocalInteraction() &&
        (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
         state == kDeviceStateActivating)) {
        audio_service_.EnableWakeWordDetection(true);
        return;
    }
#if CONFIG_USE_VOICE_BARGE_IN
    barge_in_reply_gate_.Cancel();
    if (audio_service_.IsVoiceInterruptionMonitoring()) {
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.ResetDecoder();
    }
#endif
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        // A wake word is a new user turn: it supersedes a pending exit and
        // invalidates callbacks from the previous turn.
        interaction_stopped_.store(false);
        interaction_generation_.fetch_add(1);
        entry_prompt_before_listening_ = false;
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
            // Close the old downlink gate by entering Listening before the
            // reset. Otherwise a late server packet can refill the queue
            // between ResetDecoder and the state transition.
            audio_service_.ResetDecoder();
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    if (voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load()) {
        audio_service_.EnableWakeWordDetection(true);
        return;
    }
    interaction_generation_.fetch_add(1);
    interaction_stopped_.store(false);
    entry_prompt_before_listening_ = false;
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting || interaction_stopped_.load()) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    if (interaction_stopped_.load()) {
        if (protocol_->IsAudioChannelOpened())
            protocol_->CloseAudioChannel();
        play_popup_on_listening_ = false;
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
#if CONFIG_USE_VOICE_BARGE_IN
            barge_in_reply_gate_.Cancel();
#endif
            if (external_tts_selected_.exchange(false)) {
                external_tts_turn_.fetch_add(1);
                if (auto* external = board.GetExternalTts())
                    external->Cancel();
                audio_service_.ResetDecoder();
            }
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            ClearEarlyAudio();
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
#if CONFIG_USE_VOICE_BARGE_IN
            if (audio_service_.IsVoiceInterruptionMonitoring()) {
                if (!audio_service_.IsPlaybackIdle()) {
                    // Keep detecting speech through the actual buffered TTS tail.
                    pending_listening_start_ = true;
                    break;
                }
                audio_service_.EnableVoiceProcessing(false);
            }
#endif

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                const bool prompt_first = entry_prompt_before_listening_;
                entry_prompt_before_listening_ = false;
                // Double-click entry in non-realtime modes keeps the microphone
                // closed while the ready prompt plays, so the prompt is not
                // uploaded as user speech. Realtime (AEC) and the original
                // wake-word paths keep their simultaneous listen/play behavior.
                if (prompt_first && play_popup_on_listening_ &&
                    listening_mode_ != kListeningModeRealtime) {
                    play_popup_on_listening_ = false;
                    pending_listening_start_ = true;
                    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
                } else if (listening_mode_ == kListeningModeAutoStop &&
                           !audio_service_.IsPlaybackIdle()) {
                    // For auto mode, wait for the playback queue to drain before enabling
                    // voice processing. This prevents audio truncation when STOP arrives
                    // late due to network jitter. Instead of blocking the main loop here,
                    // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                entry_prompt_before_listening_ = false;
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            entry_prompt_before_listening_ = false;
            if (voice_enrollment_active_.load()) {
                // Enrollment owns the microphone; prompts must not arm barge-in
                // or ordinary wake words before the capture callback opens it.
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
                break;
            }
#if CONFIG_USE_VOICE_BARGE_IN
            if (listening_mode_ == kListeningModeAutoStop &&
                audio_service_.BeginVoiceInterruption()) {
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
                break;
            }
#endif

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
                ESP_LOGI(TAG, "Speaking wake detection: %s",
                         audio_service_.IsWakeWordRunning() ? "requested" : "unavailable");
            }
            break;
        case kDeviceStateStarting:
        case kDeviceStateActivating:
        case kDeviceStateWifiConfiguring:
            if (new_state == kDeviceStateWifiConfiguring || board.SupportsLocalInteraction()) {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(board.SupportsLocalInteraction() &&
                                                       local_assets_applied_.load());
            }
            break;
        case kDeviceStateUpgrading:
        case kDeviceStateFatalError:
            if (board.SupportsLocalInteraction()) {
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        default:
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening || interaction_stopped_.load() ||
        barge_in_reconnecting_.load()) {
        return;
    }

#if CONFIG_USE_VOICE_BARGE_IN
    if (audio_service_.IsVoiceInterruptionMonitoring()) {
        audio_service_.EnableVoiceProcessing(false);
    }
#endif
    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::FlushEarlyAudio() {
    std::lock_guard<std::mutex> lock(early_audio_mutex_);
    tts_start_pending_.store(false);
    if (GetDeviceState() != kDeviceStateSpeaking)
        audio_service_.ResetDecoder();
    aborted_ = false;
#if CONFIG_USE_VOICE_BARGE_IN
    if (listening_mode_ == kListeningModeAutoStop) {
        // Arm before publishing Speaking: a short TTS may start and stop in
        // the same main-task batch, before HandleStateChangedEvent can run.
        audio_service_.BeginVoiceInterruption();
    }
#endif
    SetDeviceState(kDeviceStateSpeaking);
    while (!early_audio_.empty()) {
        auto packet = std::move(early_audio_.front());
        early_audio_.pop_front();
        audio_service_.PushPacketToDecodeQueue(std::move(packet), false);
    }
}

void Application::ClearEarlyAudio() {
    std::lock_guard<std::mutex> lock(early_audio_mutex_);
    tts_start_pending_.store(false);
    early_audio_.clear();
}

void Application::ConfigureWakeWordForListening() {
    if (Board::GetInstance().SupportsLocalInteraction()) {
        audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
        return;
    }
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    if (external_voice_preview_active_.exchange(false))
        external_voice_preview_status_.store(4);
    external_tts_turn_.fetch_add(1);
    if (external_tts_selected_.exchange(false)) {
        if (auto* external = Board::GetInstance().GetExternalTts())
            external->Cancel();
    }
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_ && !voice_enrollment_reconnecting_.load() && !barge_in_reconnecting_.load() &&
        protocol_->IsAudioChannelOpened()) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::ResumeAfterVoiceEnrollment(uint32_t generation) {
    if (!voice_enrollment_active_.load() || interaction_stopped_.load() ||
        generation != interaction_generation_.load())
        return;
    if (!protocol_) {
        voice_enrollment_active_.store(false);
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        voice_enrollment_resume_guard_until_us_ = esp_timer_get_time() + 10000000;
        voice_enrollment_active_.store(false);
        SetListeningMode(GetDefaultListeningMode());
        return;
    }

    // The server may close an idle audio session while Ali is processing.
    // Keep the product in conversation mode and reopen it off the main task.
    struct Reconnect {
        Application* app;
        std::shared_ptr<Protocol> protocol;
        uint32_t generation;
    };
    auto* request = new (std::nothrow) Reconnect{this, protocol_, generation};
    if (!request) {
        voice_enrollment_active_.store(false);
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    voice_enrollment_reconnecting_.store(true);
    const auto start = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<Reconnect> request(static_cast<Reconnect*>(arg));
            auto* app = request->app;
            auto protocol = request->protocol;
            const auto token = request->generation;
            const bool opened = protocol->IsAudioChannelOpened() || protocol->OpenAudioChannel();
            app->Schedule([app, protocol, token, opened]() {
                app->voice_enrollment_reconnecting_.store(false);
                if (!app->voice_enrollment_active_.load() || app->interaction_stopped_.load() ||
                    token != app->interaction_generation_.load() || app->protocol_ != protocol) {
                    if (protocol->IsAudioChannelOpened())
                        protocol->CloseAudioChannel();
                    return;
                }
                app->voice_enrollment_active_.store(false);
                if (opened && protocol->IsAudioChannelOpened()) {
                    app->voice_enrollment_resume_guard_until_us_ = esp_timer_get_time() + 10000000;
                    ESP_LOGI(TAG, "Conversation restored after voice enrollment");
                    app->SetListeningMode(app->GetDefaultListeningMode());
                } else {
                    ESP_LOGW(TAG, "Unable to restore conversation after voice enrollment");
                    app->SetDeviceState(kDeviceStateIdle);
                }
            });
            // Free task-owned references before FreeRTOS deletes this task.
            request.reset();
            protocol.reset();
            vTaskDelete(nullptr);
        },
        "voice_reconnect", 8192, request, 3, nullptr);
    if (start != pdPASS) {
        delete request;
        voice_enrollment_reconnecting_.store(false);
        voice_enrollment_active_.store(false);
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::StopInteraction(bool play_exit_sound) {
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
    official_probe_active_.store(false);
#endif
    voice_selection_reply_guard_.store(false);
    if (external_voice_preview_active_.exchange(false))
        external_voice_preview_status_.store(4);
    voice_enrollment_resume_guard_until_us_ = 0;
    if (voice_enrollment_active_.exchange(false)) {
        Board::GetInstance().SetVoiceEnrollmentRecording(false);
    }
    if (auto* enrollment = Board::GetInstance().GetVoiceEnrollment())
        enrollment->Cancel();
    external_tts_turn_.fetch_add(1);
    if (external_tts_selected_.exchange(false)) {
        if (auto* external = Board::GetInstance().GetExternalTts())
            external->Cancel();
    }
    interaction_stopped_.store(true);
#if CONFIG_USE_VOICE_BARGE_IN
    // A rapid exit/re-entry can coalesce the Idle state notification.
    // Clear the reply barrier immediately, behind the stopped-input gate.
    barge_in_reply_gate_.Cancel();
#endif
    // Drop frames already produced; the SEND_AUDIO gate also rejects frames
    // that finish encoding after this point.
    while (audio_service_.PopPacketFromSendQueue()) {
    }
    ClearEarlyAudio();
    const auto generation = interaction_generation_.fetch_add(1) + 1;
    Schedule([this, generation, play_exit_sound]() {
        if (generation != interaction_generation_.load() || !interaction_stopped_.load())
            return;
        // A capture-start callback may already have passed its guard on the
        // main task when a button callback cancels from another task.
        Board::GetInstance().SetVoiceEnrollmentRecording(false);
        const auto state = GetDeviceState();
        play_popup_on_listening_ = false;
        pending_listening_start_ = false;
        entry_prompt_before_listening_ = false;
        audio_service_.ResetDecoder();
        if (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
            state == kDeviceStateConnecting) {
            AbortSpeaking(kAbortReasonNone);
            audio_service_.EnableVoiceProcessing(false);
            // Catch frames that finished encoding while the engine stopped.
            while (audio_service_.PopPacketFromSendQueue()) {
            }
            if (protocol_ && !voice_enrollment_reconnecting_.load() &&
                !barge_in_reconnecting_.load() && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            }
            // Queue the one exit prompt before Idle admits local sound again.
            // Ordinary stop/quiet/rest/shutdown remain silent here.
            if (play_exit_sound) {
                const auto sound = Board::GetInstance().GetConversationExitSound();
                audio_service_.PlaySound(sound.empty() ? Lang::Sounds::OGG_POPUP : sound);
            }
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

const char* Application::ExternalVoicePreviewStatus() const {
    switch (external_voice_preview_status_.load()) {
        case 1:
            return "playing";
        case 2:
            return "complete";
        case 3:
            return "failed";
        case 4:
            return "cancelled";
        default:
            return "idle";
    }
}

bool Application::RequestDeviceVoiceEnrollment(const std::string& name) {
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
    if (official_probe_active_.load())
        return false;
#endif
    const auto state = GetDeviceState();
    const bool conversation = state == kDeviceStateListening || state == kDeviceStateSpeaking;
    if (name.empty() || name.size() > 48 || (state != kDeviceStateIdle && !conversation) ||
        (conversation && interaction_stopped_.load()) || voice_enrollment_reconnecting_.load() ||
        barge_in_reconnecting_.load() || external_voice_preview_active_.load() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        !Board::GetInstance().GetVoiceEnrollment() || voice_enrollment_active_.exchange(true))
        return false;
    const auto generation = interaction_generation_.fetch_add(1) + 1;
    device_voice_enrollment_generation_.store(generation);
    device_voice_enrollment_status_.store(1);
    Schedule([this, generation, name, conversation]() {
        auto* enrollment = Board::GetInstance().GetVoiceEnrollment();
        if (!voice_enrollment_active_.load() || generation != interaction_generation_.load())
            return;
        const auto current = GetDeviceState();
        const bool still_conversation =
            current == kDeviceStateListening || current == kDeviceStateSpeaking;
        if (!enrollment ||
            (conversation ? (!still_conversation || interaction_stopped_.load())
                          : current != kDeviceStateIdle) ||
            Board::GetInstance().IsAudioMaintenanceActive()) {
            device_voice_enrollment_status_.store(4);
            voice_enrollment_active_.store(false);
            return;
        }
        if (conversation) {
            voice_enrollment_resume_guard_until_us_ = 0;
            AbortSpeaking(kAbortReasonNone);
            ClearEarlyAudio();
        }
        audio_service_.ResetDecoder();
        if (conversation) {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->SendStopListening();
                protocol_->CloseAudioChannel();
            }
        }
        SetDeviceState(kDeviceStateSpeaking);
        const bool started = enrollment->StartNamed(
            name,
            [this, generation](bool enable) {
                Schedule([this, generation, enable]() {
                    if (!voice_enrollment_active_.load() ||
                        generation != interaction_generation_.load())
                        return;
                    Board::GetInstance().SetVoiceEnrollmentRecording(enable);
                    if (enable)
                        audio_service_.EnableWakeWordDetection(false);
                    audio_service_.EnableVoiceProcessing(enable);
                    if (!enable)
                        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
                });
            },
            [this, generation, conversation](bool ok) {
                Schedule([this, generation, conversation, ok]() {
                    if (!voice_enrollment_active_.load() ||
                        generation != interaction_generation_.load())
                        return;
                    Board::GetInstance().SetVoiceEnrollmentRecording(false);
                    audio_service_.EnableVoiceProcessing(false);
                    device_voice_enrollment_status_.store(ok ? 3 : 4);
                    if (conversation) {
                        ResumeAfterVoiceEnrollment(generation);
                    } else {
                        voice_enrollment_active_.store(false);
                        if (GetDeviceState() == kDeviceStateSpeaking)
                            SetDeviceState(kDeviceStateIdle);
                    }
                });
            });
        if (!started) {
            device_voice_enrollment_status_.store(4);
            if (conversation) {
                ResumeAfterVoiceEnrollment(generation);
            } else {
                voice_enrollment_active_.store(false);
                SetDeviceState(kDeviceStateIdle);
            }
        } else {
            device_voice_enrollment_status_.store(2);
        }
    });
    return true;
}

const char* Application::DeviceVoiceEnrollmentStatus() const {
    const auto status = device_voice_enrollment_status_.load();
    if ((status == 1 || status == 2) &&
        (!voice_enrollment_active_.load() ||
         device_voice_enrollment_generation_.load() != interaction_generation_.load()))
        return "cancelled";
    if (status == 1)
        return "starting";
    if (status == 2) {
        auto* enrollment = Board::GetInstance().GetVoiceEnrollment();
        return enrollment ? enrollment->Status() : "failed";
    }
    return status == 3 ? "success" : status == 4 ? "failed" : "idle";
}

bool Application::RequestExternalVoicePreview(const std::string& voice) {
    if (GetDeviceState() != kDeviceStateIdle || voice_enrollment_active_.load() ||
        voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        external_voice_preview_active_.exchange(true))
        return false;
    external_voice_preview_status_.store(1);
    const auto generation = interaction_generation_.load();
    Schedule([this, generation, voice]() {
        auto* external = Board::GetInstance().GetExternalTts();
        if (!external_voice_preview_active_.load())
            return;
        if (generation != interaction_generation_.load()) {
            external_voice_preview_active_.store(false);
            external_voice_preview_status_.store(4);
            return;
        }
        if (GetDeviceState() != kDeviceStateIdle ||
            Board::GetInstance().IsAudioMaintenanceActive() || !external ||
            !external->ReadyForVoice(voice)) {
            external_voice_preview_active_.store(false);
            external_voice_preview_status_.store(3);
            return;
        }
        audio_service_.ResetDecoder();
        external_tts_selected_.store(true);
        const auto token = external_tts_callback_token_.fetch_add(1) + 1;
        SetDeviceState(kDeviceStateSpeaking);
        const auto playback = audio_service_.GetPlaybackGeneration();
        const bool begun =
            external->BeginWithVoice(playback, voice, [this, generation, token](bool ok) {
                Schedule([this, generation, token, ok]() {
                    if (!external_voice_preview_active_.load() ||
                        generation != interaction_generation_.load() ||
                        token != external_tts_callback_token_.load())
                        return;
                    external_voice_preview_active_.store(false);
                    external_voice_preview_status_.store(ok ? 2 : 3);
                    external_tts_selected_.store(false);
                    if (GetDeviceState() == kDeviceStateSpeaking)
                        SetDeviceState(kDeviceStateIdle);
                });
            });
        if (!begun || !external->Enqueue("你好呀，这是我现在的声音。")) {
            if (begun)
                external->Cancel();
            external_tts_selected_.store(false);
            external_voice_preview_active_.store(false);
            external_voice_preview_status_.store(3);
            SetDeviceState(kDeviceStateIdle);
            return;
        }
        external->Finish();
    });
    return true;
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (voice_enrollment_active_.load() || voice_enrollment_reconnecting_.load() ||
        barge_in_reconnecting_.load())
        return;
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && !voice_enrollment_reconnecting_.load() && !barge_in_reconnecting_.load() &&
            protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        protocol_generation_.fetch_add(1);
        interaction_generation_.fetch_add(1);
        if (barge_in_reconnecting_.load()) {
            audio_service_.EnableVoiceProcessing(false);
            SetDeviceState(kDeviceStateIdle);
        }
        if (voice_enrollment_active_.exchange(false)) {
            Board::GetInstance().SetVoiceEnrollmentRecording(false);
            if (auto* enrollment = Board::GetInstance().GetVoiceEnrollment())
                enrollment->Cancel();
            audio_service_.EnableVoiceProcessing(false);
            SetDeviceState(kDeviceStateIdle);
        }
        // Close audio channel if opened
        if (protocol_ && !voice_enrollment_reconnecting_.load() && !barge_in_reconnecting_.load() &&
            protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

#if CONFIG_USE_VOICE_BARGE_IN
bool Application::ReconnectAfterVoiceInterruption(uint32_t generation) {
    struct Reconnect {
        Application* app;
        std::shared_ptr<Protocol> protocol;
        uint32_t generation;
    };
    auto* request = new (std::nothrow) Reconnect{this, protocol_, generation};
    if (!request || !request->protocol) {
        delete request;
        return false;
    }
    const auto start = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<Reconnect> request(static_cast<Reconnect*>(arg));
            auto* app = request->app;
            auto protocol = request->protocol;
            const auto token = request->generation;
            const bool opened = protocol->IsAudioChannelOpened() || protocol->OpenAudioChannel();
            app->Schedule([app, protocol, token, opened]() {
                const bool valid = !app->interaction_stopped_.load() &&
                                   token == app->interaction_generation_.load() &&
                                   app->protocol_ == protocol &&
                                   app->GetDeviceState() == kDeviceStateListening;
                if (!valid) {
                    if (protocol->IsAudioChannelOpened())
                        protocol->CloseAudioChannel();
                    app->barge_in_reconnecting_.store(false);
                    app->audio_service_.ReleaseVoiceInterruptionBuffer();
                    return;
                }
                if (!opened || !protocol->IsAudioChannelOpened()) {
                    app->barge_in_reconnecting_.store(false);
                    app->audio_service_.ReleaseVoiceInterruptionBuffer();
                    app->audio_service_.EnableVoiceProcessing(false);
                    while (app->audio_service_.PopPacketFromSendQueue()) {
                    }
                    app->SetDeviceState(kDeviceStateIdle);
                    ESP_LOGW(TAG, "Unable to reconnect after voice interruption");
                    return;
                }
                protocol->SendStartListening(app->listening_mode_);
                app->barge_in_reconnecting_.store(false);
                xEventGroupSetBits(app->event_group_, MAIN_EVENT_SEND_AUDIO);
            });
            vTaskDelete(nullptr);
        },
        "barge_reconnect", 8192, request, 3, nullptr);
    if (start != pdPASS) {
        delete request;
        return false;
    }
    return true;
}

#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
void Application::BeginOfficialAsrProbe(uint32_t token) {
    if (official_probe_used_ || !protocol_ || !protocol_->IsAudioChannelOpened())
        return;
    official_probe_used_ = true;
    official_probe_sent_stop_ = false;
    official_probe_packets_sent_ = 0;
    official_probe_packets_failed_ = 0;
    official_probe_stt_received_.store(false);
    official_probe_started_us_ = esp_timer_get_time();
    official_probe_active_.store(true);
    // The official service does not advertise an ASR-only probe. Manual mode
    // may alter the remote turn despite no local abort.
    protocol_->SendStartListening(kListeningModeManualStop);
    if (!audio_service_.CommitVoiceInterruption(token, false)) {
        official_probe_active_.store(false);
        protocol_->SendStopListening();
        ESP_LOGW(TAG, "Official ASR probe: candidate expired before capture");
        return;
    }
    ESP_LOGI(TAG, "Official ASR probe: started; local playback not aborted");
}

void Application::ServiceOfficialAsrProbe() {
    if (!official_probe_active_.load())
        return;
    const int64_t elapsed_us = esp_timer_get_time() - official_probe_started_us_;
    if (!official_probe_sent_stop_ && elapsed_us >= 2000000) {
        official_probe_sent_stop_ = true;
        audio_service_.EnableVoiceProcessing(false);
        if (protocol_ && protocol_->IsAudioChannelOpened())
            protocol_->SendStopListening();
        ESP_LOGI(TAG, "Official ASR probe: sent listen stop; audio packets sent=%lu failed=%lu",
                 static_cast<unsigned long>(official_probe_packets_sent_),
                 static_cast<unsigned long>(official_probe_packets_failed_));
    }
    if (elapsed_us >= 6000000 || interaction_stopped_.load() || !protocol_ ||
        !protocol_->IsAudioChannelOpened()) {
        if (!official_probe_sent_stop_) {
            official_probe_sent_stop_ = true;
            audio_service_.EnableVoiceProcessing(false);
        }
        const bool received = official_probe_stt_received_.load();
        official_probe_active_.store(false);
        ESP_LOGI(TAG, "Official ASR probe ended: transcript=%d state=%d", received,
                 static_cast<int>(GetDeviceState()));
        if (!interaction_stopped_.load() && protocol_ && protocol_->IsAudioChannelOpened() &&
            GetDeviceState() == kDeviceStateListening) {
            if (audio_service_.IsPlaybackIdle()) {
                StartListeningAudio();
            } else {
                pending_listening_start_ = true;
            }
        }
    }
}
#endif

void Application::HandleVoiceInterruptionEvent() {
    if (voice_enrollment_active_.load() || voice_enrollment_reconnecting_.load() ||
        barge_in_reconnecting_.load())
        return;
    const auto state = GetDeviceState();
    const auto token = pending_voice_interruption_.load();
    if (interaction_stopped_.load() ||
        (state != kDeviceStateSpeaking &&
         !(state == kDeviceStateListening && pending_listening_start_)) ||
        !audio_service_.IsVoiceInterruptionPending(token))
        return;
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
    BeginOfficialAsrProbe(token);
#else
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    const auto diagnostic = audio_service_.GetBargeInDiagnostics(token);
    bool servo_powered = false;
    int64_t last_servo_command_us = 0;
    const bool servo_known =
        Board::GetInstance().GetBargeInServoDiagnostics(servo_powered, last_servo_command_us);
    const int64_t now_us = esp_timer_get_time();
    const int64_t servo_age_ms = last_servo_command_us > 0 && now_us >= last_servo_command_us
                                     ? (now_us - last_servo_command_us) / 1000
                                     : -1;
#endif
    const bool channel_open = protocol_ && protocol_->IsAudioChannelOpened();
    if (!channel_open)
        barge_in_reconnecting_.store(true);
    if (!audio_service_.CommitVoiceInterruption(token, !channel_open)) {
        barge_in_reconnecting_.store(false);
        return;
    }
    barge_in_reply_gate_.Begin();
    const auto generation = interaction_generation_.fetch_add(1) + 1;
    ClearEarlyAudio();
    audio_service_.ResetDecoder();
    pending_listening_start_ = play_popup_on_listening_ = entry_prompt_before_listening_ = false;
    SetDeviceState(kDeviceStateListening);
    AbortSpeaking(kAbortReasonNone);
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    ESP_LOGI(TAG,
             "Dino barge candidate token=%lu valid=%d vad=%d residual=%d threshold=%d "
             "echo_corr=%d%% required_ms=%d servo_known=%d powered=%d command_age_ms=%lld",
             static_cast<unsigned long>(token), diagnostic.pending, diagnostic.vad,
             diagnostic.residual, diagnostic.threshold, diagnostic.correlation_percent,
             diagnostic.required_speech_ms,
             servo_known, servo_powered, static_cast<long long>(servo_age_ms));
#endif
    ConfigureWakeWordForListening();
    if (channel_open) {
        protocol_->SendStartListening(listening_mode_);
    } else if (!ReconnectAfterVoiceInterruption(generation)) {
        barge_in_reconnecting_.store(false);
        audio_service_.ReleaseVoiceInterruptionBuffer();
        audio_service_.EnableVoiceProcessing(false);
        while (audio_service_.PopPacketFromSendQueue()) {
        }
        SetDeviceState(kDeviceStateIdle);
    }
    ESP_LOGI(TAG, "Voice interruption: reply cancelled, post-AEC prefix retained");
#endif
}
#endif
