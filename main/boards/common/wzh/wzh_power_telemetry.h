#ifndef WZH_POWER_TELEMETRY_H_
#define WZH_POWER_TELEMETRY_H_

#include <cJSON.h>

#include "wzh_battery_voltage.h"
#include "wzh_touch_input.h"

namespace wzh {
// Serialize cached samples only: HTTP requests must never compete for ADC1.
// Stable touch codes own the channel indicators; instantaneous voltage is
// diagnostic data and may still be crossing a debounce threshold.
inline void AddPowerTelemetry(cJSON* root, int battery_mv, bool touch_stable_valid, int touch_code,
                              int touch_mv) {
    if (!root)
        return;
    auto* battery = cJSON_GetObjectItemCaseSensitive(root, "battery");
    if (cJSON_IsObject(battery)) {
        if (battery_mv > 0)
            cJSON_AddNumberToObject(battery, "voltage_mv", battery_mv);
        else
            cJSON_AddNullToObject(battery, "voltage_mv");
    }
    auto* touch = cJSON_AddObjectToObject(root, "touch");
    if (!touch)
        return;
    const int decoded = TouchInputFilter::DecodeMillivolts(touch_mv);
    const bool valid = touch_stable_valid && decoded >= 0 && touch_code >= 0 && touch_code <= 15;
    cJSON_AddBoolToObject(touch, "available", true);
    cJSON_AddBoolToObject(touch, "valid", valid);
    cJSON_AddNumberToObject(touch, "code", valid ? touch_code : -1);
    cJSON_AddNumberToObject(touch, "raw_code", decoded);
    if (touch_mv >= 0)
        cJSON_AddNumberToObject(touch, "voltage_mv", touch_mv);
    else
        cJSON_AddNullToObject(touch, "voltage_mv");
}

inline void AddBatterySamplingTelemetry(cJSON* root, const BatterySamplingStatus& sample) {
    if (!root)
        return;
    auto* battery = cJSON_GetObjectItemCaseSensitive(root, "battery");
    if (!cJSON_IsObject(battery))
        return;
    cJSON_AddStringToObject(battery, "voltage_source", "rest");
    cJSON_AddBoolToObject(battery, "sample_valid", sample.valid);
    cJSON_AddBoolToObject(battery, "under_load", sample.under_load);
    cJSON_AddBoolToObject(battery, "rest_ready", sample.rest_ready);
    if (sample.rest_age_ms >= 0)
        cJSON_AddNumberToObject(battery, "rest_age_ms", sample.rest_age_ms);
    else
        cJSON_AddNullToObject(battery, "rest_age_ms");
    if (sample.valid && sample.live_mv > 0)
        cJSON_AddNumberToObject(battery, "live_voltage_mv", sample.live_mv);
    else
        cJSON_AddNullToObject(battery, "live_voltage_mv");
    if (sample.valid && sample.protection_mv > 0)
        cJSON_AddNumberToObject(battery, "protection_voltage_mv", sample.protection_mv);
    else
        cJSON_AddNullToObject(battery, "protection_voltage_mv");
}
}  // namespace wzh
#endif
