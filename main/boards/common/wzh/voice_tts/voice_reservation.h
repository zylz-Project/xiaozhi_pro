#pragma once

namespace voice_tts {
// -1 means no unresolved create; 0..9 is the observed count before it.
bool LoadVoiceReservation(int& before);
bool SaveVoiceReservation(int before);
// Persists evidence of device-owned voices that may have no local label.
bool LoadCloudVoiceObservation(bool& observed);
bool SaveCloudVoiceObservation(bool observed);
}  // namespace voice_tts
