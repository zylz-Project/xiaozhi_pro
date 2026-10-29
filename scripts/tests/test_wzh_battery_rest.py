"""Replay rest/load voltage, ADC errors, and all three real power paths on host."""
from pathlib import Path
import re
import unittest

import test_wzh_motion_output as output
from test_wzh_action_queue import queue_function as function

ROOT = Path(__file__).resolve().parents[2]
PRODUCTS = [('wzh_crawlPanda', 'panda', 'PandaPower'),
            ('wzh_tailRedPanda', 'panda', 'PandaPower'),
            ('wzh_dinosaur', 'dinosaur', 'DinosaurPower')]


class WzhBatteryRestTest(unittest.TestCase):
    def run_cpp(self, program):
        output.WzhMotionOutputTest.run_cpp(self, program)

    def test_rest_reading_holds_during_short_actions_and_speech_and_rejects_spikes(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_battery_voltage.h"
using namespace wzh;
int main(){
    BatteryRestVoltage f;f.ObserveLoad(false,0);
    assert(!f.RestReady(1199)&&!f.Update(4100,1199));
    assert(f.RestReady(1200)&&!f.Update(3800,1200));
    assert(!f.Update(4090,2200));assert(f.Update(3800,3200));
    assert(f.millivolts()==3800); // One high second cannot jump the display.
    for(unsigned t=4000;t<7000;t+=20){
        f.ObserveLoad(true,t);
        assert(!f.Update(3200,t)&&f.millivolts()==3800);
    }
    f.ObserveLoad(false,7000);assert(!f.RestReady(8199));
    // Short motion wholly between the one-second voltage reads resets rest.
    f.ObserveLoad(true,8000);f.ObserveLoad(false,8020);
    assert(!f.RestReady(9000)&&f.RestReady(9220));
    assert(f.Update(3800,10000)&&f.millivolts()==3800);
    assert(f.Update(3700,11000));
    assert(f.Update(3700,12000)&&f.millivolts()<3800&&f.millivolts()>3700);
    const auto held=f.millivolts();f.Invalidate();
    assert(!f.RestReady(15000)&&f.millivolts()==held);
    f.ObserveLoad(false,16000);
    assert(!f.Update(3600,18000)&&!f.Update(3600,19000));
    assert(f.Update(3600,20000));
    // Old samples cannot bias a new measurement after a long conversation.
    f.ObserveLoad(true,21000);f.ObserveLoad(false,70000);
    assert(!f.Update(3500,72000)&&!f.Update(3500,73000));
    assert(f.Update(3500,74000)&&f.millivolts()==3500);
    BatteryRestVoltage wrap;const uint32_t start=UINT32_MAX-600;
    wrap.ObserveLoad(false,start);assert(!wrap.RestReady(start+1199));
    assert(wrap.Update(3700,start+1200)==false);
    assert(!wrap.Update(3700,start+2200)&&wrap.Update(3700,start+3200));
    assert(wrap.millivolts()==3700);
    std::array<int,32> raw;raw.fill(1900);
    for(unsigned i=0;i<4;++i){raw[i]=0;raw[i+4]=4095;}
    assert(BatteryRestVoltage::TrimmedRaw(raw)==1900);
    raw.fill(1400);assert(BatteryRestVoltage::TrimmedRaw(raw)==1400);
}
''')

    def test_normal_protection_uses_stable_voltage_recovery_requires_rest_and_critical_is_live(self):
        self.run_cpp(r'''
#include <cassert>
#include "wzh_battery_voltage.h"
using namespace wzh;
int main(){
    StableBatteryProtection p;
    auto sample=[&](int mv,bool rested=false){p.Update(mv,rested,3300,3400,3000);};
    for(int i=0;i<4;++i){sample(3800);assert(!p.ready());}
    sample(3800);assert(p.ready()&&!p.low());
    // Isolated one- and two-second load dips, each followed by recovery.
    sample(3200);sample(3800);sample(3800);assert(!p.low());
    sample(3200);sample(3200);sample(3800);sample(3800);assert(!p.low());
    for(int i=0;i<10;++i){sample(i==5?3100:3800);assert(!p.low());}
    for(int i=0;i<5;++i)sample(3300);
    assert(p.low()&&p.millivolts()==3300&&!p.critical());
    for(int i=0;i<20;++i)sample(3700); // Loaded voltage must not auto-rearm motion.
    assert(p.low());
    sample(3400,true);sample(3400,true);assert(p.low());
    sample(3400,false);sample(3400,true);sample(3400,true);assert(p.low());
    sample(3400,true);assert(!p.low());
    sample(2990);sample(2990);assert(!p.critical());
    sample(2990);assert(p.critical()); // Critical path never waits for rest/IIR.
    p.Invalidate();assert(!p.ready()&&!p.critical());
    sample(2990);sample(2990);assert(!p.critical());
    sample(2990);assert(p.critical());
    StableBatteryProtection empty;
    for(int i=0;i<4;++i){empty.Update(3300,true,3300,3400,3000);assert(!empty.ready());}
    empty.Update(3300,true,3300,3400,3000);assert(empty.ready()&&empty.low());
}
''')

    def test_real_adc_bursts_and_protection_on_all_three_products(self):
        for board, stem, cls in PRODUCTS:
            base = ROOT/'main/boards'/board
            source = (base/(stem+'_power.cc')).read_text()
            config = (base/'config.h').read_text()
            self.assertRegex(config, r'#define BATTERY_DEMO_MODE 0\b')
            normal = config.split('#else', 1)[1].split('#endif', 1)[0]
            thresholds = {key: int(value) for key, value in re.findall(
                r'#define (BATTERY_\w+_MV) (\d+)', normal)}
            self.assertEqual(thresholds, {'BATTERY_LOW_MV': 3300,
                                         'BATTERY_RECOVER_MV': 3400,
                                         'BATTERY_CRITICAL_MV': 3000})
            method = function(source, 'void '+cls+'::ReadBattery(').replace(cls+'::', 'Power::')
            getter = function(source, 'wzh::BatterySamplingStatus '+cls+'::battery_sampling(').replace(
                cls+'::', 'Power::')
            with self.subTest(board=board):
                self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include "wzh_battery_voltage.h"
constexpr int BATTERY_ADC_CHANNEL=2,ESP_OK=0,BATTERY_DIVIDER_RATIO=2;
constexpr int BATTERY_LOW_MV=3300,BATTERY_RECOVER_MV=3400,BATTERY_CRITICAL_MV=3000;
constexpr int BATTERY_EMPTY_VOLTAGE_MV=3200,BATTERY_FULL_VOLTAGE_MV=4200;
int reading=1900,read_count=0,fail_at=-1;uint32_t clock_ms=15000;
uint32_t esp_log_timestamp(){return clock_ms;}
int adc_oneshot_read(int,int channel,int* value){
    assert(channel==2);const int position=read_count++;
    *value=position<2?0:reading;return position==fail_at?-1:ESP_OK;
}
int AdcToMv(int,int raw){return raw;}
struct Output{
    unsigned flags=wzh::kBatteryPending;
    void SetFault(wzh::MotionFault f,bool on){if(on)flags|=f;else flags&=~unsigned(f);}
};
struct Power{
    int adc_=0,battery_adc_cali_=0,shutdowns=0;
    Output out;std::atomic<Output*>servo_output_{&out};
    wzh::StableBatteryProtection battery_protection_;
    wzh::BatteryRestVoltage battery_rest_voltage_;
    std::atomic<int>battery_voltage_mv_{0},battery_live_mv_{0},battery_level_{0},battery_protection_mv_{0};
    std::atomic<bool>battery_sample_valid_{false},battery_load_active_{false},battery_rest_ready_{false};
    std::atomic<uint32_t>battery_rest_updated_ms_{0};
    void EmergencyShutdown(){++shutdowns;}
    void ReadBattery(uint32_t);
    wzh::BatterySamplingStatus battery_sampling()const;
};
''' + method + '\n' + getter + r'''
int main(){
    Power p;assert(p.battery_sampling().rest_age_ms==-1);
    p.battery_rest_voltage_.ObserveLoad(false,10000);
    auto sample=[&](int mv){reading=mv/2;read_count=0;p.ReadBattery(clock_ms);clock_ms+=1000;};
    for(int i=0;i<5;++i)sample(3800);
    assert(read_count==34&&p.out.flags==0&&p.battery_voltage_mv_==3800);
    assert(p.battery_level_==60&&p.battery_sampling().rest_age_ms==1000);
    p.battery_rest_voltage_.ObserveLoad(true,clock_ms);
    sample(3200);sample(3800);assert(!(p.out.flags&wzh::kLowBattery));
    for(int i=0;i<5;++i)sample(3200);
    assert(p.out.flags&wzh::kLowBattery);assert(p.battery_voltage_mv_==3800);
    assert(p.battery_protection_mv_==3200&&p.battery_live_mv_==3200);
    for(int i=0;i<10;++i)sample(3600);
    assert(p.out.flags&wzh::kLowBattery);
    p.battery_rest_voltage_.ObserveLoad(false,clock_ms-2000);
    for(int i=0;i<5;++i)sample(3600);
    assert(!(p.out.flags&wzh::kLowBattery));
    const auto rest=p.battery_voltage_mv_.load();
    fail_at=12;sample(3700);
    assert(!p.battery_sample_valid_&&p.battery_live_mv_==0&&(p.out.flags&wzh::kBatteryPending));
    assert(p.battery_voltage_mv_==rest&&p.battery_sampling().protection_mv==0);
    fail_at=-1;sample(3700);sample(3700);sample(3700);sample(3700);
    assert(p.out.flags&wzh::kBatteryPending);
    sample(3700);assert(!(p.out.flags&wzh::kBatteryPending));
    sample(2980);sample(2980);assert(p.shutdowns==0);
    sample(2980);assert(p.shutdowns==1);
}
''')


if __name__ == '__main__':
    unittest.main()
