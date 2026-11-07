#include "voice_reservation.h"

#include <nvs.h>
#include <cstdint>

namespace voice_tts {
namespace {
constexpr uint32_t kMagic = 0x31525356;
struct Record {
    uint32_t magic;
    int32_t before;
};
}  // namespace
bool LoadVoiceReservation(int& before) {
    before = -1;
    nvs_handle_t nvs = 0;
    const auto opened = nvs_open("voice_tts", NVS_READONLY, &nvs);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (opened != ESP_OK)
        return false;
    Record record{};
    size_t size = sizeof(record);
    const auto result = nvs_get_blob(nvs, "voice_intent", &record, &size);
    nvs_close(nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (result != ESP_OK || size != sizeof(record) || record.magic != kMagic ||
        record.before < -1 || record.before > 9)
        return false;
    before = record.before;
    return true;
}
bool SaveVoiceReservation(int before) {
    if (before < -1 || before > 9)
        return false;
    nvs_handle_t nvs = 0;
    if (nvs_open("voice_tts", NVS_READWRITE, &nvs) != ESP_OK)
        return false;
    const Record record{kMagic, before};
    auto result = nvs_set_blob(nvs, "voice_intent", &record, sizeof(record));
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}
}  // namespace voice_tts
