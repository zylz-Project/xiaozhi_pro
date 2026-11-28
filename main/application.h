#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "ota.h"
#include "protocol.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT (1 << 9)
#define MAIN_EVENT_START_LISTENING (1 << 10)
#define MAIN_EVENT_STOP_LISTENING (1 << 11)
#define MAIN_EVENT_STATE_CHANGED (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED (1 << 13)
#define MAIN_EVENT_TOGGLE_CONVERSATION (1 << 14)
#define MAIN_EVENT_VOICE_INTERRUPTION (1 << 15)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void QueueTouchConversationInput(std::string text);
    bool IsVoiceEnrollmentActive() const { return voice_enrollment_active_.load(); }
    bool IsConversationReopening() const {
        return barge_in_reconnecting_.load() || voice_enrollment_reconnecting_.load();
    }
    bool RequestDeviceVoiceEnrollment(const std::string& name);
    uint32_t DeviceVoiceEnrollmentRequest() const {
        return device_voice_enrollment_generation_.load();
    }
    bool RequestCancelDeviceVoiceEnrollment(uint32_t request);
    const char* DeviceVoiceEnrollmentStatus() const;
    const char* DeviceVoiceEnrollmentFailure() const;
    bool RequestExternalVoicePreview(const std::string& voice = {});
    bool IsExternalVoicePreviewActive() const { return external_voice_preview_active_.load(); }
    const char* ExternalVoicePreviewStatus() const;
    uint32_t ExternalVoicePreviewRequest() const { return external_voice_preview_request_.load(); }
    bool RequestCancelExternalVoicePreview(uint32_t request);

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Register a listener for validated device-state transitions.
     * The callback runs in the context that calls SetDeviceState(), so it must
     * remain short and hand slow work to another task.
     */
    int AddDeviceStateChangeListener(DeviceStateMachine::StateCallback callback);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "",
               const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);
    // Safe from board callbacks: cancel speech/listening on the application task.
    void StopInteraction(bool play_exit_sound = false);

    // Complete session toggle: also exits while connecting or speaking.
    // Unlike ToggleChatState(), it never starts provisioning audio tests.
    void ToggleConversation();

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC
    size_t GetMcpWireOverhead() const;
#endif
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();
#if CONFIG_WZH_DINOSAUR_AT_DIAGNOSTIC
    void PrepareNetworkTransportSwitch(std::function<void(bool)> ready);
#endif

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    // A reconnect worker retains the protocol until its pending handshake ends.
    std::shared_ptr<Protocol> protocol_;
    std::atomic<uint32_t> protocol_generation_{0};
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    std::atomic<bool> local_assets_applied_{false};
    std::atomic<bool> interaction_stopped_{false};
    std::atomic<uint32_t> interaction_generation_{0};
    struct TouchConversationInput {
        std::string text;
        uint32_t interaction_generation = 0;
        uint32_t protocol_generation = 0;
        int64_t received_us = 0;
    };
    std::mutex touch_input_mutex_;
    TouchConversationInput pending_touch_input_;
    std::atomic<int64_t> last_listening_voice_edge_us_{0};
    int64_t next_touch_input_us_ = 0;  // Main task only.
    void SendPendingTouchConversationInput();
#if CONFIG_USE_VOICE_BARGE_IN
    std::atomic<uint32_t> pending_voice_interruption_{0};
    BargeInReplyGate barge_in_reply_gate_;
    void HandleVoiceInterruptionEvent();
#endif
#if CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE
    // Main task owns the one-shot state; network callbacks only set atomics.
    std::atomic<bool> official_probe_active_{false};
    std::atomic<bool> official_probe_stt_received_{false};
    int64_t official_probe_started_us_ = 0;
    uint32_t official_probe_packets_sent_ = 0;
    uint32_t official_probe_packets_failed_ = 0;
    bool official_probe_sent_stop_ = false;
    bool official_probe_used_ = false;
    void BeginOfficialAsrProbe(uint32_t token);
    void ServiceOfficialAsrProbe();
#endif
    // A TTS start arrives before its first audio packets, but the state change
    // runs on the main task. Keep a bounded prefix so the first syllable is
    // not dropped while the start transition is pending.
    static constexpr size_t kMaxEarlyAudioPackets = 8;
    std::mutex early_audio_mutex_;
    std::deque<std::unique_ptr<AudioStreamPacket>> early_audio_;
    std::atomic<bool> tts_start_pending_{false};
    std::atomic<bool> external_tts_selected_{false};
    std::atomic<bool> voice_enrollment_active_{false};
    std::atomic<int> device_voice_enrollment_status_{0};
    std::atomic<uint32_t> device_voice_enrollment_generation_{0};
    bool device_voice_enrollment_from_chat_ = false;  // Main task only.
    std::atomic<bool> device_voice_enrollment_attempted_{false};
    // Block late server speech for a local voice-selection turn until the next user transcript.
    std::atomic<bool> voice_selection_reply_guard_{false};
    std::atomic<uint32_t> external_voice_preview_request_{0};
    std::atomic<bool> external_voice_preview_active_{false};
    // 0 idle, 1 requested/playing, 2 complete, 3 failed, 4 cancelled.
    std::atomic<int> external_voice_preview_status_{0};
    std::atomic<bool> voice_enrollment_reconnecting_{false};
    std::atomic<bool> barge_in_reconnecting_{false};
    // A delayed server goodbye may arrive immediately after enrollment ends.
    std::atomic<int64_t> voice_enrollment_resume_guard_until_us_{0};
    std::atomic<uint32_t> external_tts_turn_{0};
    std::atomic<uint32_t> external_tts_callback_token_{0};
    bool server_music_active_ = false;  // main-task owned
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ =
        false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ =
        false;  // Waiting for playback to drain before starting listening (auto mode)
    // Double-click entry only: play the ready prompt before opening the mic.
    // Wake-word and other original XiaoZhi entry paths keep their behavior.
    bool entry_prompt_before_listening_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    std::atomic<bool> network_link_up_{false};
    int64_t network_resume_deadline_us_ = 0;  // main-task owned
    uint32_t network_resume_generation_ = 0;  // main-task owned
    bool network_connected_ = false;          // main-task owned
    bool activation_retry_pending_ = false;   // main-task owned

    // Main-task-owned transient Wi-Fi UDP retry state.
    std::unique_ptr<AudioStreamPacket> uplink_retry_;
    uint32_t uplink_retry_generation_ = 0;
    uint32_t uplink_retry_protocol_ = 0;
    unsigned uplink_retry_count_ = 0;
    int64_t uplink_retry_due_us_ = 0, uplink_retry_expires_us_ = 0;
    int64_t uplink_failure_started_us_ = 0, uplink_reconnect_after_us_ = 0;
    uint32_t uplink_failure_generation_ = 0;
    int64_t first_interrupted_uplink_us_ = 0;
    uint32_t first_interrupted_uplink_generation_ = 0;
    void SendQueuedVoice();


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleToggleConversationEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void ServiceConversationNetworkRecovery();
    void StopInteractionInternal(bool play_exit_sound, bool network_lost);
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void FlushEarlyAudio();
    void ClearEarlyAudio();
    void ConfigureWakeWordForListening();
    void ResumeAfterVoiceEnrollment(uint32_t generation);
    bool ReopenConversation(uint32_t generation, bool after_network_loss, bool replace_channel = false);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};

class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() { vTaskPrioritySet(NULL, original_priority_); }

private:
    BaseType_t original_priority_;
};

#endif  // _APPLICATION_H_
