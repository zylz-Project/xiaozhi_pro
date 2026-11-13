#include "voice_flash_dispatcher.h"
#include "sdkconfig.h"

#if CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT
#include "esp_flash_dispatcher.h"
#include "esp_log.h"
#endif

namespace voice_tts {
namespace {
bool ready = false;
}
bool InitializeFlashDispatcher() {
#if CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT
    if (ready)
        return true;
    const esp_flash_dispatcher_config_t config = {
        .task_stack_size = 3072,
        .task_priority = 10,
        .task_core_id = tskNO_AFFINITY,
        .queue_size = 1,
    };
    const esp_err_t result = esp_flash_dispatcher_init(&config);
    if (result != ESP_OK) {
        ESP_LOGE("VoiceFlash", "Flash dispatcher unavailable: %s", esp_err_to_name(result));
        return false;
    }
    ready = true;
#endif
    return ready;
}
bool FlashDispatcherReady() { return ready; }
}  // namespace voice_tts
