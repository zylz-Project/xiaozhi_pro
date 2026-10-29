"""Power telemetry preserves measured values and never reports invalid touch as released."""
import unittest

import test_wzh_developer_console as console_tests


class WzhPowerTelemetryTest(unittest.TestCase):
    def test_rest_voltage_preserves_cache_and_reports_live_protection_age_and_invalid_samples(self):
        console_tests.WzhDeveloperConsoleTest.run_cpp(self, r'''
#include <cassert>
#include <cstring>
#include "wzh_power_telemetry.h"
using namespace wzh;
const cJSON* item(const cJSON* parent,const char* key){return cJSON_GetObjectItemCaseSensitive(parent,key);}
int main(){
    AddBatterySamplingTelemetry(nullptr,{});
    auto* root=cJSON_CreateObject();AddBatterySamplingTelemetry(root,{});
    assert(!item(root,"battery"));
    auto* battery=cJSON_AddObjectToObject(root,"battery");
    cJSON_AddNumberToObject(battery,"voltage_mv",3800);
    AddBatterySamplingTelemetry(root,{3200,3250,true,true,false,45000});
    assert(!std::strcmp(item(battery,"voltage_source")->valuestring,"rest"));
    assert(item(battery,"voltage_mv")->valueint==3800);
    assert(item(battery,"live_voltage_mv")->valueint==3200);
    assert(item(battery,"protection_voltage_mv")->valueint==3250);
    assert(item(battery,"rest_age_ms")->valueint==45000);
    assert(cJSON_IsTrue(item(battery,"sample_valid"))&&cJSON_IsTrue(item(battery,"under_load")));
    assert(cJSON_IsFalse(item(battery,"rest_ready")));cJSON_Delete(root);
    root=cJSON_CreateObject();battery=cJSON_AddObjectToObject(root,"battery");
    AddBatterySamplingTelemetry(root,{});
    assert(cJSON_IsNull(item(battery,"live_voltage_mv")));
    assert(cJSON_IsNull(item(battery,"protection_voltage_mv")));
    assert(cJSON_IsNull(item(battery,"rest_age_ms")));
    assert(cJSON_IsFalse(item(battery,"sample_valid")));cJSON_Delete(root);
}
''')

    def test_cached_voltage_all_touch_channels_and_missing_invalid_samples(self):
        console_tests.WzhDeveloperConsoleTest.run_cpp(self, r'''
#include <cassert>
#include "wzh_power_telemetry.h"
using namespace wzh;
const cJSON* item(const cJSON* parent,const char* key){return cJSON_GetObjectItemCaseSensitive(parent,key);}
int main(){
    AddPowerTelemetry(nullptr,3738,true,1,210);
    const int voltages[]={0,210,380,570,750,950,1170,1390,1590,1790,1990,2210,2410,2610,2810,3010};
    for(int code=0;code<16;++code){
        auto* root=cJSON_CreateObject();auto* battery=cJSON_AddObjectToObject(root,"battery");
        cJSON_AddNumberToObject(battery,"level",53);
        AddPowerTelemetry(root,3738,true,code,voltages[code]);
        assert(item(battery,"voltage_mv")->valueint==3738&&item(battery,"level")->valueint==53);
        const auto* touch=item(root,"touch");
        assert(cJSON_IsTrue(item(touch,"available"))&&cJSON_IsTrue(item(touch,"valid")));
        assert(item(touch,"code")->valueint==code&&item(touch,"raw_code")->valueint==code);
        assert(item(touch,"voltage_mv")->valueint==voltages[code]);
        cJSON_Delete(root);
    }
    const int unknown_mv[]={-1,3300,4095};
    for(int mv:unknown_mv){
        auto* root=cJSON_CreateObject();auto* battery=cJSON_AddObjectToObject(root,"battery");
        AddPowerTelemetry(root,0,true,15,mv); // Old stable contact must not mask an invalid current sample.
        assert(cJSON_IsNull(item(battery,"voltage_mv")));
        const auto* touch=item(root,"touch");
        assert(cJSON_IsFalse(item(touch,"valid"))&&item(touch,"code")->valueint==-1);
        assert(item(touch,"raw_code")->valueint==-1);
        assert(mv<0?cJSON_IsNull(item(touch,"voltage_mv")):cJSON_IsNumber(item(touch,"voltage_mv")));
        cJSON_Delete(root);
    }
    auto* root=cJSON_CreateObject();
    AddPowerTelemetry(root,4000,false,0,210); // Stable confirmation is still pending.
    const auto* touch=item(root,"touch");
    assert(!item(root,"battery")&&cJSON_IsFalse(item(touch,"valid")));
    assert(item(touch,"code")->valueint==-1&&item(touch,"raw_code")->valueint==1);
    cJSON_Delete(root);
}
''')
