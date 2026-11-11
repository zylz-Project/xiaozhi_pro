"""Startup ADC qualification against conservative divider models."""
import unittest
from pathlib import Path
import test_wzh_motion_output as output

ROOT = Path(__file__).resolve().parents[2]


class FastBatteryStartupTest(unittest.TestCase):
    def run_cpp(self, code):
        output.WzhMotionOutputTest.run_cpp(self, code)

    def test_healthy_pack_can_start_and_original_protection_is_initialized(self):
        self.run_cpp(r'''
#include "wzh_fast_battery_start.h"
#include <cassert>
using namespace wzh;
int main(){
 for(int mv:{3900,4200}){
  FastBatteryStart gate;bool ready=false;uint32_t when=0;
  for(uint32_t t=2000;t<15000;t+=200){
   if(gate.Observe(t,true,mv,3400)){ready=true;when=t;break;}
  }
  assert(ready&&when<10000);
  StableBatteryProtection protection;
  assert(CommitFastBatteryStart(gate,protection,false,3300,3400,3000));
  assert(protection.ready()&&!protection.low()&&!protection.critical());
  // The next live readings continue to enforce the existing critical path.
  for(int i=0;i<2;++i){protection.Update(2900,false,3300,3400,3000);assert(!protection.critical());}
  protection.Update(2900,false,3300,3400,3000);assert(protection.critical());
 }
}
'''.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>'))

    def test_worst_assumed_residual_cannot_fast_qualify_low_battery(self):
        self.run_cpp(r'''
#include "wzh_fast_battery_start.h"
#include <cassert>
#include <cmath>
using namespace wzh;
int main(){
 // Old full capacitor, conservatively slow RC, and maximum assumed ADC
 // over-reading: a battery below recovery voltage must never fast qualify.
 for(int actual=2800;actual<3400;actual+=10){
  FastBatteryStart gate;
  for(unsigned t=2000;t<15000;t+=200){
   const float e=std::exp(-float(t)/6000.0f);
   const int observed=int(std::floor(actual+(4400-actual)*e+200));
   assert(!gate.Observe(t,true,observed,3400));
  }
 }
 FastBatteryStart startup;
 // Fresh battery with an initially empty capacitor may read low; observation
 // never marks critical/low protection or cuts power before the slow path.
 for(unsigned t=2000;t<15000;t+=200){
  const int observed=int(4200*(1-std::exp(-float(t)/3740.0f)));
  if(startup.Observe(t,true,observed,3400))break;
 }
}
''')

    def test_invalid_calibration_bad_adc_gaps_and_unstable_samples(self):
        self.run_cpp(r'''
#include "wzh_fast_battery_start.h"
#include <cassert>
using namespace wzh;
int main(){
 int calls=0,battery=0;
 auto read=[&](int& raw){raw=(calls++<2)?0:1900;return true;};
 auto calibrated=[](int raw,int& mv){mv=raw;return true;};
 assert(ReadFastBatteryProbe(read,calibrated,battery)&&calls==34&&battery==3800);
 auto no_cali=[](int,int&){return false;};calls=0;assert(!ReadFastBatteryProbe(read,no_cali,battery));
 calls=0;auto bad=[&](int& raw){raw=1900;return ++calls!=12;};
 assert(!ReadFastBatteryProbe(bad,calibrated,battery));
 calls=0;auto saturated=[&](int& raw){raw=(++calls==4)?4095:1900;return true;};
 assert(!ReadFastBatteryProbe(saturated,calibrated,battery));
 FastBatteryStart gate;StableBatteryProtection protection;
 assert(!CommitFastBatteryStart(gate,protection,false,3300,3400,3000));
 assert(!gate.Observe(6000,true,4200,3400));
 for(unsigned i=0;i<8;++i)assert(!gate.Observe(6000,true,4200,3400));
 assert(!gate.Observe(6200,true,4200,3400));
 assert(!gate.Observe(6400,true,4200,3400));
 assert(!gate.Observe(6600,true,4200,3400));
 assert(gate.Observe(6800,true,4200,3400));
 FastBatteryStart fresh;
 gate=fresh;
 for(unsigned t=6000;t<6800;t+=200)assert(!gate.Observe(t,true,4200,3400));
 assert(!gate.Observe(6800,false,4200,3400));
 for(unsigned t=7000;t<7800;t+=200)assert(!gate.Observe(t,true,4200,3400));
 assert(!gate.Observe(8400,true,4200,3400)); // stale samples must not count
 for(unsigned t=8600;t<9400;t+=200)assert(!gate.Observe(t,true,4200,3400));
 assert(gate.Observe(9400,true,4200,3400));
 assert(!gate.Observe(9600,true,4200,3400)); // retry never writes past five slots
 FastBatteryStart noisy;
 for(unsigned t=6000;t<=6800;t+=200)assert(!noisy.Observe(t,true,t==6400?4400:4200,3400));
}
''')

    def test_production_power_fast_branch_uses_real_gate_and_latches(self):
        for board, stem in [('wzh_crawlPanda','panda'),('wzh_tailRedPanda','panda'),('wzh_dinosaur','dinosaur')]:
            source=(ROOT/'main/boards'/board/(stem+'_power.cc')).read_text()
            start=source.index('        if (!battery_settled && now < 15000 &&')
            end=source.index('        if ((battery_settled || now >= 15000)', start)
            branch=source[start:end]
            with self.subTest(board=board):
                self.run_cpp(r'''
#include "wzh_fast_battery_start.h"
#include <atomic>
#include <cassert>
#include <cmath>
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
constexpr int BATTERY_ADC_CHANNEL=2,BATTERY_LOW_MV=3300,BATTERY_RECOVER_MV=3400,BATTERY_CRITICAL_MV=3000;
int raw_value=1950; bool adc_valid=true,calibration_valid=true;
int adc_oneshot_read(int,int,int* out){*out=raw_value;return adc_valid?0:-1;}
int adc_cali_raw_to_voltage(void*,int raw,int* mv){*mv=raw;return calibration_valid?0:-1;}
struct Output { unsigned flags=wzh::kBatteryPending;void SetFault(wzh::MotionFault f,bool on){if(on)flags|=f;else flags&=~unsigned(f);} };
struct Power {
 int adc_=0;void* battery_adc_cali_=this;Output output;std::atomic<Output*> servo_output_{&output};
 wzh::FastBatteryStart fast_battery;wzh::StableBatteryProtection battery_protection_;
 wzh::BatteryRestVoltage battery_rest_voltage_;bool battery_settled=false;
 uint32_t last_fast_battery_sample_ms=0,last_fast_battery_log_ms=0,last_battery_sample_ms=0;
 std::atomic<int> battery_live_mv_{0},battery_protection_mv_{0};std::atomic<bool> battery_sample_valid_{false};
 void Tick(uint32_t now){
''' + branch + r'''
 }
};
int main(){
 Power normal;for(uint32_t t=1200;t<10000&&!normal.battery_settled;t+=20)normal.Tick(t);
 assert(normal.battery_settled&&normal.last_battery_sample_ms<10000);
 assert(!(normal.output.flags&wzh::kBatteryPending)&&normal.battery_protection_.ready());
 assert(normal.battery_live_mv_==3900&&normal.battery_sample_valid_);
 Power no_cal;no_cal.battery_adc_cali_=nullptr;
 for(uint32_t t=1200;t<15000;t+=20)no_cal.Tick(t);
 assert(!no_cal.battery_settled&&(no_cal.output.flags&wzh::kBatteryPending));
 Power old_cap;for(uint32_t t=1200;t<15000;t+=20){
  // 3.3 V cell with an old 4.4 V capacitor and a 200 mV ADC overread.
  float voltage=3300+1100*std::exp(-float(t)/6000.0f)+200;
  raw_value=int(voltage/2);old_cap.Tick(t);
 }
 assert(!old_cap.battery_settled&&(old_cap.output.flags&wzh::kBatteryPending));
}
''')

    def test_three_power_tasks_keep_fallback_and_share_qualification(self):
        for board, stem in [('wzh_crawlPanda','panda'),('wzh_tailRedPanda','panda'),('wzh_dinosaur','dinosaur')]:
            source=(ROOT/'main/boards'/board/(stem+'_power.cc')).read_text()
            config=(ROOT/'main/boards'/board/'config.h').read_text()
            self.assertIn('#define BATTERY_DIVIDER_RATIO 2', config)
            self.assertIn('wzh::ReadFastBatteryProbe(', source)
            self.assertIn('wzh::CommitFastBatteryStart(', source)
            self.assertIn('(battery_settled || now >= 15000)', source)
            self.assertIn('now - last_battery_sample_ms >= BATTERY_READ_INTERVAL_MS', source)
            self.assertIn('battery_adc_cali_ != nullptr', source)

if __name__ == '__main__':
    unittest.main()
