"""只读审查诊断：提取当前分支并运行最小桩，不代替设备集成测试。

运行：python3 docs/review/wzh_product_audit_2026-10-15_reproduce.py
需要 g++；观测结果写入系统临时目录，不修改固件或设备状态。
"""
from pathlib import Path
import subprocess, tempfile, textwrap, json, sys
root=Path(__file__).resolve().parents[2]
out=Path(tempfile.gettempdir())/'wzh_audit_20261015'
out.mkdir(parents=True, exist_ok=True)
results=[]
def cpp_case(name, source):
    with tempfile.TemporaryDirectory() as directory:
        directory=Path(directory)
        (directory/'case.cc').write_text(source)
        subprocess.run(['g++','-std=c++17','-pthread','-I',str(root/'main/boards/common/wzh'),str(directory/'case.cc'),'-o',str(directory/'case')],check=True,capture_output=True)
        result=subprocess.check_output([str(directory/'case')],text=True).strip()
        results.append({'case':name,'observed':result})
# Execute the actual local-command branch extracted from Application.
s=(root/'main/application.cc').read_text()
a=s.index('void Application::HandleWakeWordDetectedEvent() {')
branch=s[a:s.index('    if (!protocol_)',a)]+'}\n'
cpp_case('A02_speaking_local_event_disables_detection',r'''
#include <iostream>
#include <string>
enum DeviceState { kDeviceStateIdle, kDeviceStateSpeaking };
struct Audio { bool enabled=true; std::string GetLastWakeWord() { return "local:happy"; }
 void EnableWakeWordDetection(bool value) { enabled=value; } };
struct Board { static Board& GetInstance() { static Board board; return board; }
 bool HandleLocalVoiceCommand(const std::string&) { return true; } };
struct Application { Audio audio_service_; DeviceState GetDeviceState() { return kDeviceStateSpeaking; }
 void HandleWakeWordDetectedEvent(); };
'''+branch+r'''
int main() { Application app; app.HandleWakeWordDetectedEvent();
 std::cout << "state=speaking, enabled_before=true, enabled_after=" << std::boolalpha << app.audio_service_.enabled; }
''')
# Execute the actual PIR debounce/dispatch block extracted from Sensors::Task.
s=(root/'main/boards/common/wzh/wzh_sensors.cc').read_text()
a=s.index('        const bool human = gpio_get_level(human_) != 0;')
block=s[a:s.index('        if (device_ != nullptr',a)]
cpp_case('A03_boot_presence_event_is_lost',r'''
#include <iostream>
#include <mutex>
#include <algorithm>
#include <functional>
int gpio_get_level(int) { return 1; }
enum class Event { kHuman };
struct Probe { std::mutex mutex_; struct {bool human=false;} status_;
 int human_=41, count=0; std::function<void(Event)> hook_=[this](Event){++count;};
 void Run() { bool candidate=false, stable=false; int human_samples=0; uint32_t last_event=0;
 for (uint32_t now=0; now<=20000; now+=20) {
'''+block+r'''
 } } };
int main() { Probe probe; probe.Run(); std::cout << "PIR=HIGH for 20s, dispatched_events=" << probe.count; }
''')
# Execute the production budget class; no class implementation is copied.
cpp_case('A04_power_budget_is_not_motion_heartbeat',r'''
#include <iostream>
#include "wzh_protection.h"
int main() { wzh::ServoPowerBudget budget; budget.PowerOn(0);
 std::cout << std::boolalpha << "no further motion writes: allow_at_1000ms=" << budget.Allow(1000)
 << ", allow_at_59999ms=" << budget.Allow(59999) << ", allow_at_60000ms=" << budget.Allow(60000); }
''')
cpp_case('A04_short_release_resets_continuous_budget',r'''
#include <iostream>
#include "wzh_protection.h"
int main() { wzh::ServoPowerBudget budget; bool allowed=true;
 for (uint32_t n=0;n<30;++n) { uint32_t now=n*20001; budget.PowerOn(now);
 allowed=allowed && budget.Allow(now+20000); budget.Release(); }
 std::cout << std::boolalpha << "30 cycles of 20s powered +1ms release: all_allowed=" << allowed; }
''')
# Verify current battery thresholds by executing the production policy.
cpp_case('PASS_current_battery_thresholds',r'''
#include <iostream>
#include <cassert>
#include "wzh_protection.h"
int main() { wzh::BatteryProtection battery; for(int n=0;n<3;++n) battery.Update(3300); assert(battery.low());
 for(int n=0;n<20;++n) battery.Update(3399); assert(battery.low());
 battery.Update(3400); battery.Update(3400); assert(battery.low()); battery.Update(3400); assert(!battery.low());
 std::cout << "3300mV lock, 3399mV stays locked, three 3400mV samples recover: PASS"; }
''')
(out/'reproductions.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
for entry in results: print(entry['case']+': '+entry['observed'])
