#include "wzh_voice_services.h"

#include "application.h"
#include "voice_tts/ali_voice_enrollment.h"
#include "voice_tts/voice_config.h"
#include "voice_tts/voice_names.h"
#include "voice_tts/voice_session.h"
#include "voice_tts/voice_web.h"

namespace wzh {
namespace {
voice_tts::VoiceWeb& Management() {
    static voice_tts::VoiceWeb service;
    return service;
}

bool CanChangeVoice() {
    auto& app = Application::GetInstance();
    return !Management().Busy() && !app.IsVoiceEnrollmentActive() &&
           !app.IsExternalVoicePreviewActive();
}
}  // namespace

ExternalTts* ProductExternalTts() {
    static voice_tts::VoiceSession session;
    return &session;
}

VoiceEnrollment* ProductVoiceEnrollment() {
    static voice_tts::AliVoiceEnrollment enrollment;
    return &enrollment;
}

bool VoiceManagementBusy() { return Management().Busy(); }

void ConfigureVoiceConsole(WebConsole::Options& options) {
    options.voice_command = [](const std::string& action, const std::string& id,
                               const std::string& name) {
        return Management().Command(action, id, name);
    };
    options.voice_upload = [](uint8_t* data, size_t length) {
        return Management().Upload(data, length);
    };
    options.voice_device_record = [](const std::string& name) {
        return !Management().Busy() && voice_tts::IsValidVoiceName(name) &&
               Application::GetInstance().RequestDeviceVoiceEnrollment(name);
    };
    options.voice_status_extra = [](cJSON* root) { Management().AddStatus(root); };
    options.save_ali_key = [](const std::string& key) {
        return CanChangeVoice() && voice_tts::SaveAliKey(key);
    };
    options.has_ali_key = [] { return !voice_tts::LoadAliKey().empty(); };
    options.save_ali_voice = [](const std::string& voice) {
        return CanChangeVoice() && voice_tts::SaveAliVoice(voice);
    };
    options.ali_voice = voice_tts::LoadAliVoice;
    options.previous_ali_voice = voice_tts::LoadPreviousAliVoice;
    options.restore_ali_voice = [] {
        return CanChangeVoice() && voice_tts::RestorePreviousAliVoice();
    };
    options.voice_enrollment_status = [] { return ProductVoiceEnrollment()->Status(); };
    options.voice_enrollment_failure = [] { return ProductVoiceEnrollment()->Failure(); };
    options.preview_ali_voice = [] {
        return Application::GetInstance().RequestExternalVoicePreview();
    };
    options.cancel_voice = [] {
        auto& app = Application::GetInstance();
        const bool management_cancelled = Management().Cancel();
        const bool local_active =
            app.IsVoiceEnrollmentActive() || app.IsExternalVoicePreviewActive();
        if (local_active)
            app.StopInteraction();
        return management_cancelled || local_active;
    };
    options.voice_preview_status = [] {
        return Application::GetInstance().ExternalVoicePreviewStatus();
    };
}

}  // namespace wzh
