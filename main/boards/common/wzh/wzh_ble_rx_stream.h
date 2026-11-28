#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/message_buffer.h>

namespace wzh {

// The control block contains a FreeRTOS spinlock and stays in internal RAM.
// Only the byte storage moves to PSRAM: it is accessed by the NimBLE host and
// the BLE worker, never by an ISR or DMA. Both live for the service lifetime.
inline MessageBufferHandle_t CreateBleRxBuffer(size_t capacity, bool& external) {
    external = false;
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    // FreeRTOS static message buffers reserve one byte to distinguish full
    // from empty; dynamic creation adds this byte itself.
    auto* storage =
        static_cast<uint8_t*>(heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage != nullptr) {
        auto* control = static_cast<StaticStreamBuffer_t*>(
            heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (control != nullptr) {
            auto stream = xMessageBufferCreateStatic(capacity + 1, storage, control);
            if (stream != nullptr) {
                external = true;
                return stream;
            }
            heap_caps_free(control);
        }
        heap_caps_free(storage);
    }
#endif
    return xMessageBufferCreate(capacity);
}

}  // namespace wzh
