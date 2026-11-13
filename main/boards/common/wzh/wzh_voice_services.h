#pragma once

#include "wzh_web_console.h"

class ExternalTts;
class VoiceEnrollment;

namespace voice_tts {
class VoiceWeb;
}

namespace wzh {

// One selected WZH board owns the service instances through optional Board hooks.
ExternalTts* ProductExternalTts();
VoiceEnrollment* ProductVoiceEnrollment();
bool VoiceManagementBusy();
// Shared voice manager used by the developer console and the BLE transport.
voice_tts::VoiceWeb& VoiceManagement();
void ConfigureVoiceConsole(WebConsole::Options& options);

}  // namespace wzh
