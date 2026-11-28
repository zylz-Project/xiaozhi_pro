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
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
#include "wzh_onboarding.h"
#include "wzh_prompt_audio_gate.h"
#include "wzh_system_prompts.h"
#endif

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <utility>

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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_audio_task_)
        vTaskDelete(cellular_audio_task_);
#endif
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
        if (GetDeviceState() == kDeviceStateListening)
            last_listening_voice_edge_us_.store(esp_timer_get_time());
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
#if CONFIG_USE_VOICE_BARGE_IN
    callbacks.on_voice_interruption = [this](uint32_t token) {
        pending_voice_interruption_.store(token);
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        const auto until = keyword_interruption_until_us_.load();
        if (until > 0 && esp_timer_get_time() <= until)
            xEventGroupSetBits(event_group_, MAIN_EVENT_VOICE_INTERRUPTION);
#else
        xEventGroupSetBits(event_group_, MAIN_EVENT_VOICE_INTERRUPTION);
#endif
    };
#endif
    audio_service_.SetCallbacks(callbacks);
    audio_service_.Start();
    board.OnAudioServiceReady();

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
                if (!Board::GetInstance().IsNetworkAvailable())
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
#if CONFIG_ENABLE_ONLINE_UPDATES
        Settings asset_settings("assets", false);
        const bool load_local_assets = asset_settings.GetString("download_url").empty();
#else
        constexpr bool load_local_assets = true;
#endif
        if (load_local_assets) {
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

        if ((bits & MAIN_EVENT_ERROR) && !(bits & MAIN_EVENT_NETWORK_DISCONNECTED) &&
            !interaction_stopped_.load()) {
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

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
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
            ServiceExternalVoicePreview();
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            if (Board::GetInstance().UsesCellularNetwork()) {
                QueueCellularAudioSend();
            } else
#endif
            {
                while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                    if (interaction_stopped_.load() || voice_enrollment_active_.load() ||
                        external_voice_preview_status_.load() == 5) {
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
                        while (audio_service_.PopPacketFromSendQueue()) {
                        }
                        break;
                    }
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

        if (bits & (MAIN_EVENT_SCHEDULE | MAIN_EVENT_CLOCK_TICK)) {
            ServiceWakeWordRearm();
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            ServiceConversationNetworkRecovery();
            ServiceExternalVoicePreview();
            SendPendingTouchConversationInput();
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            if (cellular_server_connected_.load() && !cellular_prewarming_.load() &&
                !cellular_audio_in_flight_ &&
                (!Board::GetInstance().UsesCellularNetwork() ||
                 !Board::GetInstance().IsNetworkAvailable() || !protocol_ ||
                 !protocol_->IsAudioChannelOpened()))
                cellular_server_connected_.store(false);
            MaybePrewarmCellularAudio();
#endif
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

bool Application::IsShuttingDown() const {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
    return wzh::IsSystemPromptShutdownActive();
#else
    return false;
#endif
}

void Application::HandleNetworkConnectedEvent() {
    // A queued connection event can arrive after the link drops or shutdown
    // begins. Do not start an activation on a network that is already gone.
    if (IsShuttingDown() || !Board::GetInstance().IsNetworkAvailable()) {
        ESP_LOGI(TAG, "Ignoring stale network connection event");
        return;
    }
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        activation_network_ready_pending_ = true;
    } else if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
               (state == kDeviceStateIdle && !protocol_)) {
        if (activation_running_) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }
        SetDeviceState(kDeviceStateActivating);
        activation_running_ = true;
        if (xTaskCreate(
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    app->ActivationTask();
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_ACTIVATION_DONE);
                    vTaskDelete(nullptr);
                },
                "activation", 4096 * 2, this, 2, nullptr) != pdPASS) {
            activation_running_ = false;
            ESP_LOGE(TAG, "Cannot start activation task");
            SetDeviceState(kDeviceStateIdle);
        }
    } else if (protocol_) {
        protocol_->OnNetworkReady();
        if (IsConversationRecovering()) {
            // WebSocket can reopen as soon as the board link is ready. MQTT
            // will also wake this path when its control connection is ready.
            Schedule([this]() { ServiceConversationNetworkRecovery(); });
        }
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    const auto state = GetDeviceState();
    auto& board = Board::GetInstance();
    if (state != kDeviceStateActivating && protocol_)
        protocol_->OnNetworkLost();
    {
        std::lock_guard<std::mutex> lock(touch_input_mutex_);
        pending_touch_input_ = {};
    }
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    cellular_server_connected_.store(false);
#endif
    if (board.SupportsLocalInteraction() &&
        (state == kDeviceStateConnecting || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking)) {
        SuspendConversationForNetworkLoss(20000000);
        if (protocol_ && !voice_enrollment_reconnecting_.load() && !barge_in_reconnecting_.load())
            protocol_->CloseAudioChannel(false);
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        board.GetDisplay()->UpdateStatusBar(true);
        return;
    }
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
            protocol_->CloseAudioChannel(false);
        }
        if (stopped_enrollment)
            SetDeviceState(kDeviceStateIdle);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

bool Application::IsConversationRecovering() const {
    return network_resume_deadline_us_.load() > esp_timer_get_time() &&
           network_resume_generation_.load() == interaction_generation_.load();
}

void Application::PrepareConversationNetworkSwitch() {
    Schedule([this] {
        constexpr int64_t kSwitchRecoveryUs = 45000000;
        if (GetDeviceState() == kDeviceStateIdle) {
            if (IsConversationRecovering())
                network_resume_deadline_us_.store(esp_timer_get_time() + kSwitchRecoveryUs);
            else
                StopInteraction();
        } else {
            SuspendConversationForNetworkLoss(kSwitchRecoveryUs);
        }
    });
}

void Application::SuspendConversationForNetworkLoss(int64_t recovery_us) {
    const auto state = GetDeviceState();
    if (state != kDeviceStateConnecting && state != kDeviceStateListening &&
        state != kDeviceStateSpeaking)
        return;
    const bool resume = !interaction_stopped_.load() && !IsShuttingDown() &&
                        !voice_enrollment_active_.load() && !external_voice_preview_active_.load();
    const auto generation = interaction_generation_.load();
    StopInteraction();
    pending_listening_start_ = play_popup_on_listening_ = entry_prompt_before_listening_ = false;
    audio_service_.ResetDecoder();
    audio_service_.EnableVoiceProcessing(false);
    SetDeviceState(kDeviceStateIdle);
    if (resume && interaction_generation_.load() == generation + 1) {
        network_resume_generation_.store(generation + 1);
        network_resume_deadline_us_.store(esp_timer_get_time() + recovery_us);
    }
}

void Application::ServiceConversationNetworkRecovery() {
    if (!IsConversationRecovering() || IsShuttingDown() || GetDeviceState() != kDeviceStateIdle ||
        voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load())
        return;
    auto& board = Board::GetInstance();
    if (!board.IsNetworkAvailable() || board.IsAudioMaintenanceActive() || !protocol_ ||
        !protocol_->IsReady())
        return;
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load())
        return;
#endif
    if (protocol_->DeferUntilReconnectFinishes([this] { ServiceConversationNetworkRecovery(); }))
        return;
    if (!IsConversationRecovering())
        return;
#if CONFIG_USE_VOICE_BARGE_IN
#if CONFIG_WZH_SYSTEM_PROMPTS
    if (wzh::IsOnboardingIntroPending()) {
        network_resume_deadline_us_.store(0);
        HandleToggleConversationEvent();
        return;
    }
#endif
    auto previous_generation = network_resume_generation_.load();
    const auto generation = previous_generation + 1;
    if (!interaction_generation_.compare_exchange_strong(previous_generation, generation))
        return;
    network_resume_deadline_us_.store(0);
    ESP_LOGI(TAG, "Network restored; opening a fresh conversation");
    interaction_stopped_.store(false);
    play_popup_on_listening_ = entry_prompt_before_listening_ = true;
    barge_in_reconnecting_.store(true);
    if (SetDeviceState(kDeviceStateConnecting) &&
        ReconnectAfterVoiceInterruption(generation, false, true, true))
        return;
    barge_in_reconnecting_.store(false);
    SetDeviceState(kDeviceStateIdle);
#else
    network_resume_deadline_us_.store(0);
    HandleToggleConversationEvent();
#endif
}

void Application::HandleActivationDoneEvent() {
    activation_running_ = false;
    const bool link_resumed = std::exchange(activation_network_ready_pending_, false);
    if (IsShuttingDown()) {
        ota_.reset();
        return;
    }
    auto& board = Board::GetInstance();
    const bool ready = activation_protocol_ready_.load() && board.IsNetworkAvailable() &&
                       protocol_ && protocol_->IsReady();
    ESP_LOGI(TAG, "Activation finished, protocol ready: %d", ready);

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = board.GetDisplay();
    if (ready) {
        const auto message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
    } else {
        display->ShowNotification(Lang::Strings::SERVER_NOT_CONNECTED);
    }
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (!ready) {
        // A deferred network event can arrive while the activation task is exiting.
        if (link_resumed && board.IsNetworkAvailable())
            HandleNetworkConnectedEvent();
        return;
    }
    if (IsConversationRecovering())
        return;

    Schedule([this, protocol_generation = protocol_generation_.load()]() {
        const auto generation = audio_service_.GetPlaybackGeneration();
        if (IsShuttingDown() || !Board::GetInstance().IsNetworkAvailable() ||
            protocol_generation != protocol_generation_.load() || !protocol_ ||
            !protocol_->IsReady())
            return;
        // Play the success sound to indicate the device is ready
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
        ESP_LOGI(TAG, "Voice request: success text=我准备好啦！");
#endif
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS, generation);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    if (!IsShuttingDown())
        CheckAssetsVersion();

    // Check for new firmware version
    if (!IsShuttingDown())
        CheckNewVersion();

    activation_protocol_ready_.store(
        !IsShuttingDown() && Board::GetInstance().IsNetworkAvailable() && InitializeProtocol());
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

#if CONFIG_ENABLE_ONLINE_UPDATES
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (IsShuttingDown())
            return;
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
        settings.EraseKey("download_url");
    }
#endif

    // Apply assets before starting the recognition engine on the update path.
    const bool applied = assets.Apply();
    if (board.SupportsLocalInteraction()) {
        local_assets_applied_.store(applied);
        Schedule([this]() {
            const auto state = GetDeviceState();
            if (!IsShuttingDown() &&
                (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
                 state == kDeviceStateActivating)) {
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
    auto& board = Board::GetInstance();
    const int initial_retry_delay = board.UsesCellularNetwork() ? 2 : 10;
    int retry_delay = initial_retry_delay;
    while (!IsShuttingDown() && board.IsNetworkAvailable()) {
        auto display = board.GetDisplay();
#if CONFIG_ENABLE_ONLINE_UPDATES
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
#else
        display->SetStatus(Lang::Strings::SERVER_NOT_FOUND);
#endif

        esp_err_t err = ota_->CheckVersion();
        if (IsShuttingDown())
            return;
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

#if CONFIG_ENABLE_ONLINE_UPDATES
            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);
#endif

            ESP_LOGW(TAG, "Service configuration failed (code=%d), retry in %d s (%d/%d)", err,
                     retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (IsShuttingDown() || !board.IsNetworkAvailable() ||
                    GetDeviceState() == kDeviceStateIdle)
                    return;
            }
            retry_delay = std::min(retry_delay * 2, 60);
            continue;
        }
        retry_count = 0;
        retry_delay = initial_retry_delay;

#if CONFIG_ENABLE_ONLINE_UPDATES
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }
#endif

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
        for (int i = 0; i < 10 && !IsShuttingDown(); ++i) {
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

bool Application::InitializeProtocol() {
    conversation_configured_.store(false);
    const auto protocol_generation = protocol_generation_.fetch_add(1) + 1;
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    cellular_server_connected_.store(false);
#endif
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    const bool use_cached_config = !ota_->HasMqttConfig() && !ota_->HasWebsocketConfig();
    if (ota_->HasMqttConfig() ||
        (use_cached_config && !Settings("mqtt", false).GetString("endpoint").empty())) {
        protocol_ = std::make_shared<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig() ||
               (use_cached_config && !Settings("websocket", false).GetString("url").empty())) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No conversation service configuration available");
        return false;
    }
    conversation_configured_.store(true);

    protocol_->OnConnected([this, protocol_generation]() {
        Schedule([this, protocol_generation]() {
            if (protocol_generation != protocol_generation_.load())
                return;
            DismissAlert();
            ServiceConversationNetworkRecovery();
        });
    });

#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    protocol_->OnDisconnected([this, protocol_generation]() {
        if (protocol_generation == protocol_generation_.load()) {
            cellular_server_connected_.store(false);
            conversation_transport_disconnected_.store(true);
        }
    });
#endif

    protocol_->OnNetworkError([this, protocol_generation](const std::string& message) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        if (protocol_generation == protocol_generation_.load())
            cellular_server_connected_.store(false);
        if (cellular_prewarming_.load()) {
            ESP_LOGW(TAG, "Cellular audio preconnect failed: %s", message.c_str());
            return;
        }
#endif
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
        const auto interaction = interaction_generation_.load();
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
        std::lock_guard<std::mutex> lock(early_audio_mutex_);
        const uint32_t generation = audio_service_.GetPlaybackGeneration();
        if (protocol_generation != protocol_generation_.load() ||
            interaction != interaction_generation_.load() || interaction_stopped_.load() ||
            external_tts_selected_.load() || voice_enrollment_active_.load() ||
            voice_selection_reply_guard_.load())
            return;
        if (GetDeviceState() == kDeviceStateSpeaking && !tts_start_pending_.load()) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            if (dinosaur_server_audio_packets_.fetch_add(1) == 0)
                ESP_LOGI(TAG, "First original speech packet received for playback");
#endif
            audio_service_.PushPacketToDecodeQueue(std::move(packet), false, generation);
        } else if (tts_start_pending_.load() && early_audio_.size() < kMaxEarlyAudioPackets) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            if (dinosaur_server_audio_packets_.fetch_add(1) == 0)
                ESP_LOGI(TAG, "First original speech packet received for playback");
#endif
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
            // Configure even a prewarmed channel: its open callback may be
            // the only one before the user starts talking.
            if (board.UsesCellularNetwork()) {
                audio_service_.SetDownlinkJitter(AudioService::kCellularPlayoutTargetMs,
                                                 AudioService::kCellularPlayoutMaxWaitMs,
                                                 AudioService::kCellularPlayoutRecoveryMs);
            } else {
                audio_service_.SetDownlinkJitter(AudioService::kLocalPlayoutTargetMs,
                                                 AudioService::kLocalPlayoutMaxWaitMs);
            }
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            cellular_server_connected_.store(board.UsesCellularNetwork() &&
                                             board.IsNetworkAvailable());
            conversation_transport_disconnected_.store(false);
            if (cellular_prewarming_.load())
                return;
#endif
            board.OnConversationChannelOpened();
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        cellular_server_connected_.store(false);
        cellular_prewarmed_.store(false);
        const bool transport_disconnected = conversation_transport_disconnected_.exchange(false);
        if (cellular_prewarming_.load())
            return;
#endif
        const auto generation = interaction_generation_.load();
        Schedule([this, &board, generation, protocol_generation
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                  ,
                  transport_disconnected
#endif
        ]() {
            if (protocol_generation != protocol_generation_.load())
                return;
            if (voice_enrollment_active_.load() || barge_in_reconnecting_.load())
                return;
            // A delayed close from the previous session must not end a new one.
            if (generation != interaction_generation_.load() ||
                (protocol_ && protocol_->IsAudioChannelOpened()))
                return;
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            if (board.UsesCellularNetwork())
                cellular_prewarm_retired_ = true;
#endif
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
            if (external_voice_preview_status_.load() == 5) {
                external_voice_preview_status_.store(4);
                external_voice_preview_active_.store(false);
                external_tts_selected_.store(false);
                if (auto* external = board.GetExternalTts())
                    external->Cancel();
                audio_service_.ResetDecoder();
            } else if (external_tts_selected_.load() && !interaction_stopped_.load() &&
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR && CONFIG_USE_VOICE_BARGE_IN
            if (transport_disconnected && !interaction_stopped_.load() &&
                board.IsNetworkAvailable() &&
                (GetDeviceState() == kDeviceStateSpeaking ||
                 GetDeviceState() == kDeviceStateListening)) {
                if (disconnect_recovery_generation_ != generation) {
                    disconnect_recovery_generation_ = generation;
                    disconnect_recovery_attempted_ = false;
                }
                if (!disconnect_recovery_attempted_) {
                    disconnect_recovery_attempted_ = true;
                    ESP_LOGW(TAG, "Conversation transport disconnected; attempting to reconnect");
                    barge_in_reconnecting_.store(true);
                    SetDeviceState(kDeviceStateListening);
                    if (ReconnectAfterVoiceInterruption(generation, false, true, true))
                        return;
                    barge_in_reconnecting_.store(false);
                }
            }
#endif
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display, protocol_generation](const cJSON* root) {
        const auto callback_generation = interaction_generation_.load();
        if (protocol_generation != protocol_generation_.load() || IsShuttingDown())
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
                    ESP_LOGD(TAG, "Official ASR probe transcript playback_busy=%d bytes=%u",
                             !audio_service_.IsPlaybackIdle(), static_cast<unsigned>(strlen(text->valuestring)));
                    official_probe_stt_received_.store(true);
                } else {
                    ESP_LOGI(TAG, "Official ASR probe received empty transcript");
                }
                return;
            }
            if (strcmp(type->valuestring, "tts") == 0) {
                const auto* state = cJSON_GetObjectItem(root, "state");
                ESP_LOGI(
                    TAG, "Official ASR probe server TTS: %s",
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                dinosaur_server_audio_packets_.store(0);
#endif
                auto* external = Board::GetInstance().GetExternalTts();
                bool replacing_external;
                uint32_t external_turn, external_token;
                {
                    std::lock_guard<std::mutex> lock(early_audio_mutex_);
                    if (external_voice_preview_status_.load() == 5) {
                        external_voice_preview_active_.store(false);
                        external_voice_preview_status_.store(4);
                    }
                    replacing_external = external_tts_selected_.exchange(false);
                    if (replacing_external && external)
                        external->Cancel();
                    external_tts_selected_.store(external && external->Ready());
                    external_turn = external_tts_turn_.fetch_add(1) + 1;
                    external_token = external_tts_callback_token_.fetch_add(1) + 1;
                    tts_start_pending_.store(true);
                }
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                        if (!external_tts_selected_.load()) {
                            ESP_LOGI(
                                TAG, "Original speech packets received for playback: %lu",
                                static_cast<unsigned long>(dinosaur_server_audio_packets_.load()));
                            const auto status = audio_service_.GetPlaybackBufferStatus();
                            ESP_LOGI(TAG,
                                     "Downlink playback: buffered=%lums decode=%lu pcm=%lu "
                                     "rebuffer_total=%lu queue_full_total=%lu",
                                     static_cast<unsigned long>(status.buffered_ms),
                                     static_cast<unsigned long>(status.decode_packets),
                                     static_cast<unsigned long>(status.pcm_frames),
                                     static_cast<unsigned long>(status.rebuffer_count),
                                     static_cast<unsigned long>(status.queue_full_count));
                        }
#endif
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
                if (cJSON_IsString(text) && text->valuestring != nullptr) {
                    const auto external_turn = external_tts_turn_.load();
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGD(TAG, "TTS sentence received (%u bytes)",
                             static_cast<unsigned>(std::strlen(text->valuestring)));
                    Schedule([this, display, generation, external_turn,
                              message = std::string(text->valuestring), glyphs = std::move(glyphs),
                              bpp]() {
                        if (interaction_stopped_.load() ||
                            generation != interaction_generation_.load() ||
                            external_turn != external_tts_turn_.load() ||
                            external_voice_preview_status_.load() == 5) {
                            return;
                        }
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
                        ESP_LOGI(TAG, "Voice server sentence: %.512s", message.c_str());
#endif
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
                ESP_LOGD(TAG, "Speech transcript received (%u bytes)",
                         static_cast<unsigned>(std::strlen(text->valuestring)));
#if CONFIG_USE_VOICE_BARGE_IN
                if (text->valuestring[0] != '\0') {
                    barge_in_reply_gate_.OnTranscript();
                }
#endif
                const std::string recognized(text->valuestring);
                auto* enrollment = Board::GetInstance().GetVoiceEnrollment();
                const auto current_state = GetDeviceState();
                const bool clone_request = IsVoiceEnrollmentRequest(recognized);
                const bool selection_pending = enrollment && enrollment->SelectionPending();
                const bool native_selection = IsNativeVoiceSelection(recognized, selection_pending);
                const bool selection_request =
                    native_selection || IsVoiceSelectionRequest(recognized);
                const bool voice_operation_configured =
                    !enrollment || !(clone_request || selection_request || selection_pending) ||
                    native_selection || enrollment->IsConfigured();
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
                    Schedule([this, enrollment, token, clone_request, selection_pending,
                              native_selection, recognized, voice_operation_configured]() {
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
                                                selection_pending || native_selection ? recognized
                                                                                      : "切换音色",
                                                completion);
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                if (Board::GetInstance().UsesCellularNetwork()) {
                    constexpr unsigned kMaxPendingCellularMcp = 16;
                    constexpr size_t kMaxCellularMcpBytes = 32768;
                    const unsigned pending = cellular_pending_mcp_.fetch_add(1);
                    if (pending >= kMaxPendingCellularMcp) {
                        cellular_pending_mcp_.fetch_sub(1);
                        ESP_LOGW(TAG, "Cellular MCP queue full");
                        return;
                    }
                    char* json = cJSON_PrintUnformatted(payload);
                    if (!json) {
                        cellular_pending_mcp_.fetch_sub(1);
                        ESP_LOGE(TAG, "Cannot copy cellular MCP message");
                        return;
                    }
                    const size_t size = strlen(json);
                    if (size > kMaxCellularMcpBytes) {
                        cJSON_free(json);
                        cellular_pending_mcp_.fetch_sub(1);
                        ESP_LOGW(TAG, "Cellular MCP message exceeds size limit");
                        return;
                    }
                    std::string message(json, size);
                    cJSON_free(json);
                    Schedule([this, protocol_generation, message = std::move(message)]() {
                        cellular_pending_mcp_.fetch_sub(1);
                        if (protocol_generation == protocol_generation_.load())
                            McpServer::GetInstance().ParseMessage(message);
                    });
                    return;
                }
#endif
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
            ESP_LOGD(TAG, "Received custom message");
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

    return protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    if (IsShuttingDown())
        return;
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
    // The activation task owns the whole announcement, including its digits.
    // Fence an existing Flash producer before sharing the decoder with the prompt.
    std::unique_lock<std::mutex> reservation(wzh::PromptAudioGate());
    if (IsShuttingDown() || GetDeviceState() != kDeviceStateActivating)
        return;
    if (!audio_service_.IsPlaybackIdle() || Board::GetInstance().IsLocalPlaybackPending())
        audio_service_.ResetDecoder();
    ESP_LOGI(TAG,
             "Voice request: activation text=请在小智控制台添加设备，验证码是：（数字不写入日志）");
#endif
    const auto generation = audio_service_.GetPlaybackGeneration();
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

    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", {});
    audio_service_.PlaySound(Lang::Sounds::OGG_ACTIVATION, generation);

    for (const auto& digit : code) {
        if (IsShuttingDown() || audio_service_.GetPlaybackGeneration() != generation)
            return;
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound, generation);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    const auto generation = audio_service_.GetPlaybackGeneration();
    if (IsShuttingDown())
        return;
    ESP_LOGW(TAG, "Alert displayed (emotion=%s)", emotion ? emotion : "unknown");
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA || CONFIG_BOARD_TYPE_WZH_DINOSAUR || \
    CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
        if (sound == Lang::Sounds::OGG_UPGRADE)
            ESP_LOGI(TAG, "Voice request: upgrade text=正在更新，请勿断电。");
#endif
        audio_service_.PlaySound(sound, generation);
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
        state == kDeviceStateSpeaking || IsConversationRecovering()) {
        // Reject late TTS immediately, including while OpenAudioChannel is busy.
        StopInteraction(true);
        return;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CONVERSATION);
}

void Application::HandleToggleConversationEvent() {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load() && GetDeviceState() == kDeviceStateIdle) {
        QueueCellularInteraction(MAIN_EVENT_TOGGLE_CONVERSATION);
        Board::GetInstance().GetDisplay()->ShowNotification("正在连接，请稍候");
        return;
    }
    if (barge_in_reconnecting_.load() && GetDeviceState() == kDeviceStateIdle &&
        Board::GetInstance().UsesCellularNetwork()) {
        QueueCellularInteraction(MAIN_EVENT_TOGGLE_CONVERSATION);
        Board::GetInstance().GetDisplay()->ShowNotification("正在恢复连接，请稍候");
        return;
    }
#endif
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
    if (state != kDeviceStateIdle)
        return;
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (Board::GetInstance().UsesCellularNetwork() &&
        (!protocol_ || !Board::GetInstance().IsNetworkAvailable())) {
        ESP_LOGW(TAG, "Cannot start conversation: 4G network is not ready");
        Board::GetInstance().GetDisplay()->ShowNotification("4G网络尚未就绪");
        return;
    }
#endif
    if (!protocol_)
        return;
    const auto generation = interaction_generation_.fetch_add(1) + 1;
    interaction_stopped_.store(false);
    play_popup_on_listening_ = true;
    entry_prompt_before_listening_ = true;
    const auto mode = GetDefaultListeningMode();
    // Always pass through Connecting: the board stops local Flash playback
    // before any prompt or voice input is started, even on an open channel.
    if (SetDeviceState(kDeviceStateConnecting)) {
        Schedule([this, mode, generation]() {
            if (interaction_stopped_.load() || generation != interaction_generation_.load() ||
                GetDeviceState() != kDeviceStateConnecting)
                return;
#if CONFIG_WZH_SYSTEM_PROMPTS
            if (wzh::DeferOnboardingConversation([this, mode, generation](bool played) {
                    if (generation != interaction_generation_.load() || IsShuttingDown() ||
                        interaction_stopped_.load() || GetDeviceState() != kDeviceStateConnecting)
                        return;
                    if (played) {
                        ContinueOpenAudioChannel(mode);
                    } else {
                        play_popup_on_listening_ = entry_prompt_before_listening_ = false;
                        audio_service_.ResetDecoder();
                        SetDeviceState(kDeviceStateIdle);
                    }
                }))
                return;
#endif
            ContinueOpenAudioChannel(mode);
        });
    }
}

void Application::HandleToggleChatEvent() {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load() && GetDeviceState() == kDeviceStateIdle) {
        QueueCellularInteraction(MAIN_EVENT_TOGGLE_CHAT);
        return;
    }
#endif
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        ConsumePrewarmedCellularAudio();
#endif
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting || interaction_stopped_.load() || !protocol_) {
        return;
    }
    const auto generation = interaction_generation_.load();
    if (protocol_->DeferUntilReconnectFinishes([this, mode, generation]() {
            if (generation == interaction_generation_.load() && !IsShuttingDown())
                ContinueOpenAudioChannel(mode);
        })) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
            if (board.UsesCellularNetwork())
                cellular_prewarm_next_us_ = esp_timer_get_time() + 30000000;
#endif
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

#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    ConsumePrewarmedCellularAudio();
#endif
    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load() && GetDeviceState() == kDeviceStateIdle) {
        QueueCellularInteraction(MAIN_EVENT_START_LISTENING);
        return;
    }
#endif
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        ConsumePrewarmedCellularAudio();
#endif
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
    if (Board::GetInstance().IsAudioMaintenanceActive()) {
        // Detection disarms the audio engine. Maintenance can finish while
        // the application remains idle and never dispatches another state event.
        wake_word_rearm_pending_ = true;
        return;
    }
    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
#if CONFIG_USE_VOICE_BARGE_IN
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        const bool keyword = wake_word == "local:interrupt" || wake_word == "local:stop" ||
                             wake_word == "local:chat_wake";
        if (keyword && audio_service_.IsVoiceInterruptionMonitoring()) {
            // MultiNet and the AEC/VAD candidate run independently; either may
            // arrive first. Keep the keyword briefly for a later candidate.
            keyword_interruption_until_us_.store(esp_timer_get_time() + 1000000);
            const auto token = pending_voice_interruption_.load();
            if (audio_service_.IsVoiceInterruptionPending(token)) {
                HandleVoiceInterruptionEvent();
            } else {
                ESP_LOGI(TAG,
                         "Keyword detected: %s; waiting briefly for near-end voice confirmation",
                         wake_word.c_str());
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

void Application::ServiceWakeWordRearm() {
    if (!wake_word_rearm_pending_)
        return;
    if (IsShuttingDown() || audio_service_.IsWakeWordRunning()) {
        wake_word_rearm_pending_ = false;
        return;
    }
    auto& board = Board::GetInstance();
    const auto state = GetDeviceState();
    if (board.IsAudioMaintenanceActive() || voice_enrollment_active_.load() ||
        external_voice_preview_status_.load() == 5 ||
        (interaction_stopped_.load() &&
         (state == kDeviceStateListening || state == kDeviceStateSpeaking))) {
        return;
    }

    wake_word_rearm_pending_ = false;
    switch (state) {
        case kDeviceStateIdle:
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateListening:
            ConfigureWakeWordForListening();
            break;
        case kDeviceStateSpeaking:
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateActivating:
            if (board.SupportsLocalInteraction() && local_assets_applied_.load()) {
                audio_service_.EnableWakeWordDetection(true);
            }
            break;
        default:
            break;
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load()) {
        cellular_prewarm_wake_word_ = wake_word;
        ESP_LOGI(TAG, "Wake word waiting for cellular audio preconnect");
        return;
    }
#endif
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
    if (GetDeviceState() != kDeviceStateConnecting || interaction_stopped_.load() || !protocol_) {
        return;
    }
    const auto generation = interaction_generation_.load();
    if (protocol_->DeferUntilReconnectFinishes([this, wake_word, generation]() {
            if (generation == interaction_generation_.load() && !IsShuttingDown())
                ContinueWakeWordInvoke(wake_word);
        })) {
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    ConsumePrewarmedCellularAudio();
#endif
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
    if (IsShuttingDown())
        return;
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    wake_word_rearm_pending_ = false;
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
                if (external_voice_preview_status_.load() == 5) {
                    external_voice_preview_status_.store(4);
                    external_voice_preview_active_.store(false);
                }
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
            if (voice_enrollment_active_.load() ||
                external_voice_preview_status_.load() == 5) {
                // Local voice prompts must not arm barge-in or wake detection.
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
    if (!protocol_ || GetDeviceState() != kDeviceStateListening || interaction_stopped_.load() ||
        voice_enrollment_active_.load() || barge_in_reconnecting_.load()) {
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
    if (GetDeviceState() != kDeviceStateSpeaking) {
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
        keyword_interruption_until_us_.store(0);
#endif
        audio_service_.ResetDecoder();
    }
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
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    keyword_interruption_until_us_.store(0);
#endif
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
            return "queued";
        case 2:
            return "complete";
        case 3:
            return "failed";
        case 4:
            return "cancelled";
        case 5:
            return "playing";
        default:
            return "idle";
    }
}

bool Application::RequestDeviceVoiceEnrollment(const std::string& name) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load())
        return false;
#endif
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
    if (official_probe_active_.load())
        return false;
#endif
    const auto state = GetDeviceState();
    const bool conversation = state == kDeviceStateListening || state == kDeviceStateSpeaking;
    if (name.empty() || name.size() > 48 || IsShuttingDown() ||
        (state != kDeviceStateIdle && !conversation) ||
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
        if (!enrollment || IsShuttingDown() ||
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
        device_voice_enrollment_from_chat_ = conversation;
        SetDeviceState(kDeviceStateSpeaking);
        const bool started = enrollment->StartNamed(
            name,
            [this, generation](bool enable) {
                Schedule([this, generation, enable]() {
                    if (!voice_enrollment_active_.load() ||
                        generation != interaction_generation_.load() ||
                        device_voice_enrollment_status_.load() != 2)
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
                        generation != interaction_generation_.load() ||
                        device_voice_enrollment_status_.load() != 2)
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

bool Application::RequestCancelDeviceVoiceEnrollment(uint32_t request) {
    const auto status = device_voice_enrollment_status_.load();
    if (!request || request != DeviceVoiceEnrollmentRequest() || (status != 1 && status != 2) ||
        !voice_enrollment_active_.load())
        return false;
    Schedule([this, request]() {
        const auto current = device_voice_enrollment_status_.load();
        if (request != interaction_generation_.load() ||
            request != DeviceVoiceEnrollmentRequest() || (current != 1 && current != 2) ||
            !voice_enrollment_active_.load())
            return;
        if (auto* enrollment = Board::GetInstance().GetVoiceEnrollment())
            enrollment->Cancel();
        device_voice_enrollment_status_.store(5);
        Board::GetInstance().SetVoiceEnrollmentRecording(false);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.ResetDecoder();
        if (device_voice_enrollment_from_chat_) {
            ResumeAfterVoiceEnrollment(request);
        } else {
            voice_enrollment_active_.store(false);
            if (GetDeviceState() == kDeviceStateSpeaking)
                SetDeviceState(kDeviceStateIdle);
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
    return status == 3 ? "success" : status == 4 ? "failed" : status == 5 ? "cancelled" : "idle";
}

bool Application::RequestExternalVoicePreview(const std::string& voice) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
    if (cellular_prewarming_.load())
        return false;
#endif
    const auto state = GetDeviceState();
    const bool conversation = state == kDeviceStateListening || state == kDeviceStateSpeaking;
    if ((state != kDeviceStateIdle && !conversation) || IsShuttingDown() ||
        (conversation && interaction_stopped_.load()) || voice_enrollment_active_.load() ||
        voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load() ||
        Board::GetInstance().IsAudioMaintenanceActive() ||
        external_voice_preview_active_.exchange(true))
        return false;
    const auto request = external_voice_preview_request_.fetch_add(1) + 1;
    external_voice_preview_status_.store(1);
    constexpr int64_t kQueueTimeoutUs = 20 * 1000000;
    Schedule([this, pending = VoicePreviewRequest{voice, request, interaction_generation_.load(),
                                                  esp_timer_get_time() + kQueueTimeoutUs}]() mutable {
        if (pending.request != external_voice_preview_request_.load() ||
            !external_voice_preview_active_.load())
            return;
        pending_voice_preview_ = std::move(pending);
        ServiceExternalVoicePreview();
    });
    return true;
}

void Application::ServiceExternalVoicePreview() {
    if (!pending_voice_preview_)
        return;
    auto& pending = *pending_voice_preview_;
    if (!external_voice_preview_active_.load() ||
        pending.request != external_voice_preview_request_.load()) {
        pending_voice_preview_.reset();
        return;
    }
    const auto fail = [this](int status) {
        pending_voice_preview_.reset();
        external_voice_preview_status_.store(status);
        external_voice_preview_active_.store(false);
    };
    auto& board = Board::GetInstance();
    const auto state = GetDeviceState();
    const bool listening = state == kDeviceStateListening;
    const bool speaking = state == kDeviceStateSpeaking;
    if (pending.generation != interaction_generation_.load() || IsShuttingDown() ||
        (!listening && !speaking && state != kDeviceStateIdle) ||
        ((listening || speaking) && interaction_stopped_.load()) ||
        voice_enrollment_active_.load() || voice_enrollment_reconnecting_.load() ||
        barge_in_reconnecting_.load() || board.IsAudioMaintenanceActive()) {
        fail(4);
        return;
    }
    if (esp_timer_get_time() >= pending.deadline_us) {
        fail(3);
        return;
    }
    if (speaking || !audio_service_.IsPlaybackIdle() || board.IsLocalPlaybackPending() ||
        (listening && (pending_listening_start_ || audio_service_.IsVoiceDetected())))
        return;

    auto* external = board.GetExternalTts();
    if (!external || !external->ReadyForVoice(pending.voice)) {
        fail(3);
        return;
    }
    // The receive callback claims this same lock before starting a cloud reply.
    // Keep an incoming TTS from replacing the external player before Begin.
    std::unique_lock<std::mutex> lock(early_audio_mutex_);
    if (tts_start_pending_.load() || external_tts_selected_.load() ||
        !external_voice_preview_active_.load() || IsShuttingDown())
        return;
    const auto generation = pending.generation;
    const auto request = pending.request;
    auto voice = std::move(pending.voice);
    pending_voice_preview_.reset();
    if (listening) {
        audio_service_.EnableVoiceProcessing(false);
        while (audio_service_.PopPacketFromSendQueue()) {
        }
    }
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();
    external_voice_preview_status_.store(5);
    external_tts_selected_.store(true);
    const auto token = external_tts_callback_token_.fetch_add(1) + 1;
    SetDeviceState(kDeviceStateSpeaking);
    const auto finish = [this, generation, request, token, listening](bool ok) {
        std::lock_guard<std::mutex> guard(early_audio_mutex_);
        if (!external_voice_preview_active_.load() || external_voice_preview_status_.load() != 5 ||
            request != external_voice_preview_request_.load() ||
            generation != interaction_generation_.load() ||
            token != external_tts_callback_token_.load())
            return;
        external_tts_selected_.store(false);
        external_voice_preview_status_.store(ok ? 2 : 3);
        external_voice_preview_active_.store(false);
        if (GetDeviceState() == kDeviceStateSpeaking && !IsShuttingDown()) {
            const bool resume = listening && !interaction_stopped_.load() && protocol_ &&
                                protocol_->IsAudioChannelOpened();
            SetDeviceState(resume ? kDeviceStateListening : kDeviceStateIdle);
        }
    };
    const bool begun = external->BeginWithVoice(
        audio_service_.GetPlaybackGeneration(), voice,
        [this, finish](bool ok) { Schedule([finish, ok]() { finish(ok); }); });
    if (!begun || !external->Enqueue("你好呀，这是我现在的声音。")) {
        if (begun)
            external->Cancel();
        lock.unlock();
        finish(false);
        return;
    }
    external->Finish();
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    if (IsShuttingDown())
        return;
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    conversation_configured_.store(false);
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

#if CONFIG_ENABLE_ONLINE_UPDATES
bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    if (IsShuttingDown())
        return false;
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade");

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (IsShuttingDown())
        return false;

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
#endif

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

void Application::QueueTouchConversationInput(std::string text) {
    const auto interaction = interaction_generation_.load();
    const auto protocol = protocol_generation_.load();
    const auto state = GetDeviceState();
    if (text.empty() || (state != kDeviceStateListening && state != kDeviceStateSpeaking) ||
        IsShuttingDown() || interaction_stopped_.load() || voice_enrollment_active_.load() ||
        external_voice_preview_active_.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(touch_input_mutex_);
    if (interaction == interaction_generation_.load() && protocol == protocol_generation_.load())
        pending_touch_input_ = {std::move(text), interaction, protocol, esp_timer_get_time()};
}

void Application::SendPendingTouchConversationInput() {
    const auto now = esp_timer_get_time();
    const auto state = GetDeviceState();
    const bool can_send =
        state == kDeviceStateListening && !pending_listening_start_ && !tts_start_pending_.load() &&
        !barge_in_reconnecting_.load() && !voice_enrollment_reconnecting_.load() &&
        !audio_service_.IsVoiceDetected() &&
        now - last_listening_voice_edge_us_.load() >= 1200000 && audio_service_.IsPlaybackIdle();
    TouchConversationInput input;
    {
        std::lock_guard<std::mutex> lock(touch_input_mutex_);
        if (pending_touch_input_.text.empty())
            return;
        if ((state != kDeviceStateListening && state != kDeviceStateSpeaking) || IsShuttingDown() ||
            interaction_stopped_.load() || voice_enrollment_active_.load() ||
            external_voice_preview_active_.load() ||
            pending_touch_input_.interaction_generation != interaction_generation_.load() ||
            pending_touch_input_.protocol_generation != protocol_generation_.load() ||
            now - pending_touch_input_.received_us > 6000000) {
            pending_touch_input_ = {};
            return;
        }
        if (!can_send || now < next_touch_input_us_ ||
            now - pending_touch_input_.received_us < 350000)
            return;
        input = std::move(pending_touch_input_);
        pending_touch_input_ = {};
    }
    if (GetDeviceState() != kDeviceStateListening || IsShuttingDown() ||
        audio_service_.IsVoiceDetected() || tts_start_pending_.load() || !protocol_ ||
        !protocol_->IsAudioChannelOpened() || !Board::GetInstance().IsNetworkAvailable() ||
        Board::GetInstance().IsAudioMaintenanceActive() || interaction_stopped_.load() ||
        input.interaction_generation != interaction_generation_.load() ||
        input.protocol_generation != protocol_generation_.load())
        return;
    next_touch_input_us_ = now + 4000000;
    protocol_->SendListenText(input.text);
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

size_t Application::GetMcpWireOverhead() const {
    return protocol_ ? protocol_->GetMcpWireOverhead() : 0;
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

void Application::ResetProtocol(std::function<void()> after_reset) {
    Schedule([this, after_reset = std::move(after_reset)]() mutable {
        if (protocol_ && protocol_->DeferUntilReconnectFinishes(
                             [this, after_reset]() { ResetProtocol(after_reset); })) {
            return;
        }
        protocol_generation_.fetch_add(1);
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        cellular_server_connected_.store(false);
#endif
        auto previous = interaction_generation_.fetch_add(1);
        const auto next = previous + 1;
        network_resume_generation_.compare_exchange_strong(previous, next);
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
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
        if (after_reset && protocol_) {
            struct TransportReset {
                Application* app;
                std::shared_ptr<Protocol> protocol;
                std::function<void()> completion;
            };
            auto* reset = new (std::nothrow)
                TransportReset{this, std::move(protocol_), std::move(after_reset)};
            if (reset &&
                xTaskCreate(
                    [](void* arg) {
                        auto* request = static_cast<TransportReset*>(arg);
                        request->protocol->CloseTransport();
                        request->app->Schedule([protocol = std::move(request->protocol),
                                                done = std::move(request->completion)]() mutable {
                            protocol.reset();
                            done();
                        });
                        delete request;
                        vTaskDelete(nullptr);
                    },
                    "transport_reset", 5120, reset, 3, nullptr) == pdPASS) {
                conversation_configured_.store(false);
                return;
            }
            if (reset) {
                protocol_ = std::move(reset->protocol);
                after_reset = std::move(reset->completion);
                delete reset;
            }
            ESP_LOGW(TAG, "Cannot start transport cleanup task; closing channel in place");
        }
#endif
        // Close audio channel if opened
        if (protocol_ && !voice_enrollment_reconnecting_.load() && !barge_in_reconnecting_.load() &&
            protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel(!after_reset);
        }
        // Reset protocol
        protocol_.reset();
        conversation_configured_.store(false);
        if (after_reset)
            Schedule(std::move(after_reset));
    });
}

#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
void Application::QueueCellularAudioSend() {
    if (cellular_audio_in_flight_)
        return;
    if (!protocol_ || interaction_stopped_.load() || voice_enrollment_active_.load() ||
        external_voice_preview_status_.load() == 5) {
        while (audio_service_.PopPacketFromSendQueue()) {
        }
        return;
    }
    auto packet = audio_service_.PopPacketFromSendQueue();
    if (!packet)
        return;
    if (!cellular_audio_task_ &&
        xTaskCreate([](void* arg) { static_cast<Application*>(arg)->RunCellularAudioSend(); },
                    "cellular_audio", 4096, this, 4, &cellular_audio_task_) != pdPASS) {
        ESP_LOGW(TAG, "Cannot start cellular audio sender");
        HandleCellularAudioSendFailure();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(cellular_audio_mutex_);
        cellular_audio_send_ = {protocol_, std::move(packet), protocol_generation_.load(),
                                interaction_generation_.load()};
    }
    cellular_audio_in_flight_ = true;
    xTaskNotifyGive(cellular_audio_task_);
}

void Application::RunCellularAudioSend() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        CellularAudioSend request;
        {
            std::lock_guard<std::mutex> lock(cellular_audio_mutex_);
            request = std::move(cellular_audio_send_);
        }
        auto& board = Board::GetInstance();
        const auto protocol_generation = request.protocol_generation;
        const auto interaction_generation = request.interaction_generation;
        const bool current = protocol_generation == protocol_generation_.load() &&
                             interaction_generation == interaction_generation_.load() &&
                             !interaction_stopped_.load() && board.UsesCellularNetwork();
        const bool sent = current && board.IsNetworkAvailable() &&
                          request.protocol->SendAudio(std::move(request.packet));
        request.protocol.reset();
        Schedule([this, protocol_generation, interaction_generation, sent]() {
            cellular_audio_in_flight_ = false;
            if (protocol_generation == protocol_generation_.load() &&
                interaction_generation == interaction_generation_.load() &&
                !interaction_stopped_.load() && Board::GetInstance().UsesCellularNetwork()) {
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
                if (official_probe_active_.load() && !official_probe_sent_stop_) {
                    if (sent)
                        ++official_probe_packets_sent_;
                    else
                        ++official_probe_packets_failed_;
                }
#endif
                if (!sent) {
                    HandleCellularAudioSendFailure();
                    return;
                }
            }
            // Capture may have queued more packets while the modem was busy.
            xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
        });
    }
}

void Application::HandleCellularAudioSendFailure() {
    while (audio_service_.PopPacketFromSendQueue()) {
    }
#if CONFIG_USE_VOICE_BARGE_IN
    if (GetDeviceState() != kDeviceStateListening || !Board::GetInstance().IsNetworkAvailable())
        return;
    const auto generation = interaction_generation_.load();
    if (cellular_recovery_generation_ != generation) {
        cellular_recovery_generation_ = generation;
        cellular_recovery_attempted_ = false;
    }
    const bool retry = !cellular_recovery_attempted_;
    cellular_recovery_attempted_ = true;
    ESP_LOGW(TAG, "Cellular audio send failed; %s channel", retry ? "reopening" : "closing");
    if (!retry)
        Board::GetInstance().RequestNetworkRecheck();
    barge_in_reconnecting_.store(true);
    if (!ReconnectAfterVoiceInterruption(generation, true, retry)) {
        barge_in_reconnecting_.store(false);
        audio_service_.EnableVoiceProcessing(false);
        SetDeviceState(kDeviceStateIdle);
        ESP_LOGE(TAG, "Unable to start cellular audio recovery task");
    }
#endif
}

void Application::QueueCellularInteraction(EventBits_t event) {
    const auto interaction = interaction_generation_.load();
    const auto protocol = protocol_generation_.load();
    if (cellular_pending_interaction_generation_ != interaction ||
        cellular_pending_protocol_generation_ != protocol)
        cellular_prewarm_pending_events_ = 0;
    cellular_pending_interaction_generation_ = interaction;
    cellular_pending_protocol_generation_ = protocol;
    cellular_prewarm_pending_events_ |= event;
}

void Application::ReplayCellularInteraction() {
    const auto events = cellular_prewarm_pending_events_;
    cellular_prewarm_pending_events_ = 0;
    if (events && GetDeviceState() == kDeviceStateIdle &&
        cellular_pending_interaction_generation_ == interaction_generation_.load() &&
        cellular_pending_protocol_generation_ == protocol_generation_.load())
        xEventGroupSetBits(event_group_, events);
}

void Application::ConsumePrewarmedCellularAudio() {
    if (cellular_prewarmed_.exchange(false))
        Board::GetInstance().OnConversationChannelOpened();
}

void Application::MaybePrewarmCellularAudio() {
#if CONFIG_WZH_SYSTEM_PROMPTS
    if (wzh::IsOnboardingIntroPending())
        return;
#endif
    auto& board = Board::GetInstance();
    const int64_t now = esp_timer_get_time();
    if (cellular_prewarm_retired_ || now < cellular_prewarm_next_us_ ||
        cellular_prewarming_.load() || !board.UsesCellularNetwork() ||
        !board.IsNetworkAvailable() || board.IsAudioMaintenanceActive() ||
        GetDeviceState() != kDeviceStateIdle || !protocol_ || voice_enrollment_active_.load() ||
        voice_enrollment_reconnecting_.load() || barge_in_reconnecting_.load() ||
        external_voice_preview_active_.load())
        return;
    // Bound background work, including after repeated server or radio failures.
    cellular_prewarm_next_us_ = now + 30000000;
    if (protocol_->IsAudioChannelOpened())
        return;
    struct Prewarm {
        Application* app;
        std::shared_ptr<Protocol> protocol;
        uint32_t protocol_generation;
        uint32_t interaction_generation;
    };
    auto* request = new (std::nothrow)
        Prewarm{this, protocol_, protocol_generation_.load(), interaction_generation_.load()};
    if (!request)
        return;
    cellular_prewarming_.store(true);
    const auto created = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<Prewarm> request(static_cast<Prewarm*>(arg));
            auto* app = request->app;
            auto protocol = request->protocol;
            const auto protocol_generation = request->protocol_generation;
            const auto interaction_generation = request->interaction_generation;
            // A timed-out channel may still own the modem socket.
            protocol->CloseAudioChannel(false);
            bool opened = false;
            if (protocol_generation == app->protocol_generation_.load() &&
                interaction_generation == app->interaction_generation_.load() &&
                Board::GetInstance().IsNetworkAvailable())
                opened = protocol->OpenAudioChannel();
            if (opened && (protocol_generation != app->protocol_generation_.load() ||
                           interaction_generation != app->interaction_generation_.load() ||
                           !Board::GetInstance().IsNetworkAvailable())) {
                protocol->CloseAudioChannel(false);
                opened = false;
            }
            app->Schedule([app, protocol, protocol_generation, interaction_generation, opened]() {
                const bool valid = protocol_generation == app->protocol_generation_.load() &&
                                   interaction_generation == app->interaction_generation_.load() &&
                                   app->protocol_ == protocol &&
                                   app->GetDeviceState() == kDeviceStateIdle &&
                                   Board::GetInstance().IsNetworkAvailable();
                app->cellular_prewarmed_.store(valid && opened);
                app->cellular_prewarming_.store(false);
                ESP_LOGI(TAG, "Cellular audio preconnect %s", valid && opened ? "ready" : "unavailable");
                auto wake_word = std::move(app->cellular_prewarm_wake_word_);
                app->cellular_prewarm_wake_word_.clear();
                if (valid && !wake_word.empty())
                    app->BeginWakeWordInvoke(wake_word);
                else if (!wake_word.empty())
                    app->audio_service_.EnableWakeWordDetection(true);
                if (wake_word.empty())
                    app->ReplayCellularInteraction();
                else
                    app->cellular_prewarm_pending_events_ = 0;
            });
            request.reset();
            protocol.reset();
            vTaskDelete(nullptr);
        },
        "cell_prewarm", 8192, request, 3, nullptr);
    if (created != pdPASS) {
        delete request;
        cellular_prewarming_.store(false);
        ESP_LOGW(TAG, "Cannot start cellular audio preconnect task");
    }
}
#endif

#if CONFIG_USE_VOICE_BARGE_IN
bool Application::ReconnectAfterVoiceInterruption(uint32_t generation, bool close_first,
                                                  bool reopen, bool resume_listening) {
    struct Reconnect {
        Application* app;
        std::shared_ptr<Protocol> protocol;
        uint32_t generation;
        bool close_first;
        bool reopen;
        bool resume_listening;
        DeviceState state;
    };
    auto* request = new (std::nothrow) Reconnect{
        this, protocol_, generation, close_first, reopen, resume_listening, GetDeviceState()};
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
            if (request->close_first && protocol->IsAudioChannelOpened()) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                // A failed cellular send may leave the AT channel out of
                // sync. Avoid a goodbye publish before trying a fresh hello.
                if (Board::GetInstance().UsesCellularNetwork())
                    protocol->CloseAudioChannel(false);
                else
#endif
                    protocol->CloseAudioChannel();
            }
            const bool still_requested =
                !app->interaction_stopped_.load() && token == app->interaction_generation_.load();
            const bool opened = request->reopen && still_requested &&
                                (protocol->IsAudioChannelOpened() || protocol->OpenAudioChannel());
            const bool recheck_on_failure = request->close_first && request->reopen;
            const bool resume_listening = request->resume_listening;
            const auto state = request->state;
            app->Schedule(
                [app, protocol, token, opened, recheck_on_failure, resume_listening, state]() {
                    (void)recheck_on_failure;
                    const bool valid = !app->interaction_stopped_.load() &&
                                       token == app->interaction_generation_.load() &&
                                       app->protocol_ == protocol && app->GetDeviceState() == state;
                    if (!valid) {
                        if (protocol->IsAudioChannelOpened())
                            protocol->CloseAudioChannel();
                        app->barge_in_reconnecting_.store(false);
                        app->audio_service_.ReleaseVoiceInterruptionBuffer();
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                        app->ReplayCellularInteraction();
#endif
                        return;
                    }
                    if (!opened || !protocol->IsAudioChannelOpened()) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                        if (recheck_on_failure && Board::GetInstance().UsesCellularNetwork()) {
                            ESP_LOGW(TAG, "Cellular audio reconnect failed; rechecking network");
                            Board::GetInstance().RequestNetworkRecheck();
                        }
#endif
                        app->barge_in_reconnecting_.store(false);
                        app->audio_service_.ReleaseVoiceInterruptionBuffer();
                        app->audio_service_.EnableVoiceProcessing(false);
                        while (app->audio_service_.PopPacketFromSendQueue()) {
                        }
                        app->SetDeviceState(kDeviceStateIdle);
                        ESP_LOGW(TAG, "Unable to reconnect after voice interruption");
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                        app->ReplayCellularInteraction();
#endif
                        return;
                    }
                    app->barge_in_reconnecting_.store(false);
                    if (state == kDeviceStateConnecting) {
#if CONFIG_BOARD_TYPE_WZH_DINOSAUR
                        app->ConsumePrewarmedCellularAudio();
#endif
                        app->SetListeningMode(app->GetDefaultListeningMode());
                        return;
                    }
                    if (resume_listening) {
                        if (!app->audio_service_.IsPlaybackIdle()) {
                            app->pending_listening_start_ = true;
                            // Playback may drain between the first check and
                            // arming the deferred start.
                            if (app->audio_service_.IsPlaybackIdle()) {
                                app->pending_listening_start_ = false;
                                app->StartListeningAudio();
                            }
                        } else {
                            app->pending_listening_start_ = false;
                            app->StartListeningAudio();
                        }
                    } else {
                        protocol->SendStartListening(app->listening_mode_);
                    }
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_SEND_AUDIO);
                });
            request.reset();
            protocol.reset();
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
    // The TTS stop notification can switch state before the listening state
    // handler runs, while the decoder is still playing the end of the reply.
    const bool playing_reply = state == kDeviceStateSpeaking ||
                               (state == kDeviceStateListening &&
                                (pending_listening_start_ || !audio_service_.IsPlaybackIdle()));
    if (interaction_stopped_.load() || !playing_reply ||
        !audio_service_.IsVoiceInterruptionPending(token))
        return;
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    const auto until = keyword_interruption_until_us_.load();
    if (until <= 0 || esp_timer_get_time() > until)
        return;
#endif
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
#if CONFIG_WZH_DINO_KEYWORD_BARGE_IN
    keyword_interruption_until_us_.store(0);
#endif
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
             diagnostic.required_speech_ms, servo_known, servo_powered,
             static_cast<long long>(servo_age_ms));
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
