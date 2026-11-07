"""Run the production codec lifecycle with actual vendor gain/register conversion."""
from pathlib import Path
import unittest

import test_firmware_runtime_optimizations as runtime_tests
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
CODEC = ROOT / 'main/audio/codecs/es8311_audio_codec.cc'
VENDOR = ROOT / 'managed_components/espressif__esp_codec_dev'


class Es8311OutputGainTest(unittest.TestCase):
    run_cpp = runtime_tests.FirmwareRuntimeOptimizationsTest.run_cpp

    def program(self):
        source = CODEC.read_text()
        volume = (VENDOR / 'esp_codec_dev_vol.c').read_text()
        device = (VENDOR / 'esp_codec_dev.c').read_text()
        chip = (VENDOR / 'device/es8311/es8311.c').read_text()
        range_start = chip.index('static const esp_codec_dev_vol_range_t vol_range =')
        volume_range = chip[range_start:chip.index('};', range_start) + 2]
        helpers = '\n'.join(queue_function(volume, marker) for marker in [
            'int esp_codec_dev_vol_calc_reg(', 'float esp_codec_dev_vol_calc_db(',
            'float esp_codec_dev_col_calc_hw_gain('])
        return r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x)==0)
constexpr int ESP_CODEC_DEV_INVALID_ARG=-1,ESP_CODEC_DEV_WRONG_STATE=-2;
constexpr int ESP_CODEC_DEV_TYPE_IN_OUT=3,ESP_CODEC_DEV_WORK_MODE_BOTH=3;
constexpr int I2S_NUM_0=0,GPIO_NUM_NC=-1,ES8311_DAC_REG32=0x32;
using i2c_port_t=int;using gpio_num_t=int;
struct esp_codec_dev_vol_map_t{int vol;float db_value;};
struct esp_codec_dev_vol_range_t{esp_codec_dev_vol_map_t min_vol,max_vol;};
struct esp_codec_dev_vol_curve_t{esp_codec_dev_vol_map_t*vol_map;int count;};
struct esp_codec_dev_hw_gain_t{float pa_voltage=0,codec_dac_voltage=0,pa_gain=0;};
struct audio_codec_i2s_cfg_t{int port;void*rx_handle;void*tx_handle;};
struct audio_codec_i2c_cfg_t{int port;uint8_t addr;void*bus_handle;};
struct es8311_codec_cfg_t{
 void*ctrl_if=nullptr;void*gpio_if=nullptr;int codec_mode=0,pa_pin=0;
 bool use_mclk=false;esp_codec_dev_hw_gain_t hw_gain;
 bool pa_reverted=false,no_dac_ref=false;
};
struct audio_codec_if_t{};
struct audio_codec_es8311_t:audio_codec_if_t{bool is_open=false;float hw_gain=0;};
audio_codec_es8311_t chip_instance;es8311_codec_cfg_t configured_chip;
int last_reg=0,curve_calls=0,volume_calls=0;
void*audio_codec_new_i2s_data(audio_codec_i2s_cfg_t*){return &chip_instance;}
void*audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t*){return &chip_instance;}
void*audio_codec_new_gpio(){return &chip_instance;}
int es8311_write_reg(audio_codec_es8311_t*,int reg,uint8_t value){
 assert(reg==0x32);last_reg=value;return 0;
}
''' + volume_range + helpers + queue_function(device, 'static float _get_vol_db(') + queue_function(
            chip, 'static int es8311_set_vol(') + r'''
void*es8311_codec_new(es8311_codec_cfg_t*config){
 configured_chip=*config;chip_instance.hw_gain=esp_codec_dev_col_calc_hw_gain(&config->hw_gain);
 return &chip_instance;
}
struct Device{
 std::vector<esp_codec_dev_vol_map_t>points{{0,-50},{100,0}};
 bool opened=false;
};
struct esp_codec_dev_cfg_t{int dev_type;void*codec_if;void*data_if;};
struct esp_codec_dev_sample_info_t{
 int bits_per_sample;uint8_t channel;int channel_mask;uint32_t sample_rate;int mclk_multiple;
};
void*esp_codec_dev_new(esp_codec_dev_cfg_t*){return new Device;}
int esp_codec_dev_open(void*handle,esp_codec_dev_sample_info_t*){
 static_cast<Device*>(handle)->opened=true;chip_instance.is_open=true;return 0;
}
int esp_codec_dev_set_in_gain(void*,float){return 0;}
int esp_codec_dev_set_vol_curve(void*handle,esp_codec_dev_vol_curve_t*curve){
 auto*dev=static_cast<Device*>(handle);assert(dev->opened&&curve->count==3);
 dev->points.assign(curve->vol_map,curve->vol_map+curve->count);++curve_calls;return 0;
}
int esp_codec_dev_set_out_vol(void*handle,int volume){
 auto*dev=static_cast<Device*>(handle);assert(dev->opened);++volume_calls;
 esp_codec_dev_vol_curve_t curve{dev->points.data(),static_cast<int>(dev->points.size())};
 return es8311_set_vol(&chip_instance,_get_vol_db(&curve,volume));
}
int esp_codec_dev_close(void*handle){static_cast<Device*>(handle)->opened=false;return 0;}
void esp_codec_dev_delete(void*handle){delete static_cast<Device*>(handle);}
int esp_codec_dev_write_reg(void*,int reg,int value){assert(reg==0x44&&value==0x58);return 0;}
int esp_codec_dev_read_reg(void*,int reg,int*value){*value=reg==0x44?0x58:0;return 0;}
void gpio_set_level(int,int){}
struct AudioCodec{
 int output_volume_=70,persisted=-1;
 float speaker_makeup_gain_db_=0;
 void SetOutputVolume(int value){output_volume_=persisted=value;}
};
struct Es8311AudioCodec:AudioCodec{
 bool duplex_=false,input_reference_=false,pa_inverted_=false;
 bool input_enabled_=false,output_enabled_=false;
 int input_channels_=1,input_sample_rate_=0,output_sample_rate_=0,pa_pin_=-1;
 float input_gain_=0,max_output_volume_db_=0;
 void*rx_handle_=nullptr;void*tx_handle_=nullptr;
 void*data_if_=nullptr;void*ctrl_if_=nullptr;void*gpio_if_=nullptr;void*codec_if_=nullptr;
 void*dev_=nullptr;std::mutex data_if_mutex_;
 void CreateDuplexChannels(int,int,int,int,int){}void ResetCodec(){}
 Es8311AudioCodec(void*,i2c_port_t,int,int,gpio_num_t,gpio_num_t,gpio_num_t,gpio_num_t,
                  gpio_num_t,gpio_num_t,uint8_t,bool,bool,float,bool,float,float,float=0);
 void UpdateDeviceState();void SetOutputVolume(int);
};
''' + '\n'.join(queue_function(source, marker) for marker in [
            'Es8311AudioCodec::Es8311AudioCodec(',
            'void Es8311AudioCodec::UpdateDeviceState(',
            'void Es8311AudioCodec::SetOutputVolume('])

    def test_wzh_pa_gain_top_end_curve_mute_and_restore_after_reopen(self):
        self.run_cpp(self.program() + r'''
int main(){
 for(bool reference:{false,true}){
  Es8311AudioCodec codec(nullptr,0,24000,24000,-1,48,13,46,14,-1,0x30,
                          false,false,3.3f,reference,20.0f,-3.0f,9.0f);
  assert(configured_chip.hw_gain.pa_gain==20&&configured_chip.hw_gain.pa_voltage==3.3f);
  assert(chip_instance.hw_gain==20&&codec.input_gain_==(reference?6:36));
  assert(codec.speaker_makeup_gain_db_==9.0f);
  codec.SetOutputVolume(100);assert(codec.persisted==100&&!codec.dev_);
  int start=curve_calls;codec.output_enabled_=true;codec.UpdateDeviceState();
  assert(curve_calls==start+1&&last_reg==145); // Actual ES8311 -23 dB register.
  int previous=-1;
  for(int volume=0;volume<=100;++volume){
   codec.SetOutputVolume(volume);assert(codec.persisted==volume&&codec.output_volume_==volume);
   assert(last_reg>=previous);previous=last_reg;
   if(volume==0)assert(last_reg==0);
   if(volume==70)assert(last_reg==121); // -15 dB target - 20 dB PA gain.
   if(volume==80)assert(last_reg==131);
   if(volume==90)assert(last_reg==138);
  }
  codec.output_enabled_=false;codec.UpdateDeviceState();assert(!codec.dev_);
  codec.output_enabled_=true;codec.UpdateDeviceState();
  assert(curve_calls==start+2&&last_reg==145&&codec.output_volume_==100);
  codec.SetOutputVolume(-1);assert(last_reg==0&&codec.persisted==0);
  codec.SetOutputVolume(101);assert(last_reg==145&&codec.persisted==100);
  codec.output_enabled_=false;codec.UpdateDeviceState();
 }
}
''')

    def test_other_boards_keep_the_vendor_curve_and_hardware_compensation(self):
        self.run_cpp(self.program() + r'''
int main(){
 Es8311AudioCodec codec(nullptr,0,24000,24000,-1,48,13,46,14,-1,0x30,
                         false,false,5.0f,false,0.0f,0.0f);
 assert(configured_chip.hw_gain.pa_gain==0);
 assert(codec.speaker_makeup_gain_db_==0);
 codec.output_enabled_=true;codec.UpdateDeviceState();assert(curve_calls==0);
 for(int volume=0;volume<=100;++volume){
  codec.SetOutputVolume(volume);
  const float target=volume==0?-96.0f:-50.0f+volume*.5f;
  assert(last_reg==esp_codec_dev_vol_calc_reg(&vol_range,target-chip_instance.hw_gain));
 }
 codec.output_enabled_=false;codec.UpdateDeviceState();
}
''')
