#pragma once

#include "wzh_web_console.h"

class ExternalTts;
class VoiceEnrollment;

namespace wzh {

// One selected WZH board owns the service instances through optional Board hooks.
ExternalTts* ProductExternalTts();
VoiceEnrollment* ProductVoiceEnrollment();
bool VoiceManagementBusy();
void ConfigureVoiceConsole(WebConsole::Options& options);

}  // namespace wzh
