#pragma once

namespace voice_tts {
// -1 means no unresolved create; 0..9 is the observed count before it.
bool LoadVoiceReservation(int& before);
bool SaveVoiceReservation(int before);
}  // namespace voice_tts
