"""Validate coded Opus bandwidth and actual decoded-duration bitrate reporting."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as motion_tests

ROOT = Path(__file__).resolve().parents[2]


class DecodedAudioStatsTest(unittest.TestCase):
    def run_program(self, source):
        motion_tests.WzhFluentMotionTest.run_cpp(
            self, '#include "'+str(ROOT/'main/audio/decoded_audio_stats.h')+'"\n'+source)

    def test_all_toc_configs_and_stereo_bits_follow_rfc6716_table(self):
        self.run_program(r'''
#include <cassert>
#include <vector>
int main(){
 const unsigned expected[32]={4,4,4,4,6,6,6,6,8,8,8,8,12,12,20,20,
                              4,4,4,4,8,8,8,8,12,12,12,12,20,20,20,20};
 for(unsigned config=0;config<32;++config){
  for(unsigned stereo=0;stereo<2;++stereo){
   DecodedAudioStats stats;std::vector<int16_t> pcm(24000*3,1);
   uint8_t toc=(config<<3)|(stereo<<2);
   auto result=stats.Observe(1,&toc,1,pcm.data(),pcm.size(),24000);
   assert(result&&result->minimum_band_khz==expected[config]&&result->maximum_band_khz==expected[config]);
   assert(result->stereo_packets==stereo&&result->packets==1);
   assert(result->silk_packets==(config<12));
   assert(result->hybrid_packets==(config>=12&&config<16));
   assert(result->celt_packets==(config>=16));
  }
 }
}
''')

    def test_vbr_duration_rails_failures_and_generation_rate_changes(self):
        self.run_program(r'''
#include <cassert>
#include <vector>
int main(){
 DecodedAudioStats stats;uint8_t low=8<<3,high=(28<<3)|4;
 std::vector<int16_t> pcm(1440,0);pcm[0]=-32768;pcm[1]=32767;
 stats.Failed(1);
 for(int i=0;i<50;++i){
  auto r=stats.Observe(1,i%2?&high:&low,i%2?300:180,pcm.data(),pcm.size(),24000);
  if(i<49){assert(!r);continue;}
  assert(r&&r->samples==72000&&r->bytes==12000&&r->PayloadBitsPerSecond()==32000);
  assert(r->rail_samples==100&&r->peak==32768&&r->decode_failures==1);
  assert(r->minimum_band_khz==8&&r->maximum_band_khz==20&&r->stereo_packets==25);
 }
 assert(!stats.Observe(1,&low,180,pcm.data(),pcm.size(),24000));
 stats.Failed(1);
 // A new playback generation may not inherit old packets, failures or rails.
 std::vector<int16_t> clean(16000*3,0);
 auto r=stats.Observe(2,&high,6000,clean.data(),clean.size(),16000);
 assert(r&&r->packets==1&&r->decode_failures==0&&r->rail_samples==0);
 assert(r->PayloadBitsPerSecond()==16000);
 assert(!stats.Observe(2,nullptr,1,clean.data(),clean.size(),16000));
 assert(!stats.Observe(2,&high,0,clean.data(),clean.size(),16000));
 assert(!stats.Observe(2,&high,1,nullptr,clean.size(),16000));
 assert(!stats.Observe(2,&high,1,clean.data(),0,16000));
 assert(!stats.Observe(2,&high,1,clean.data(),clean.size(),0));
 assert(!stats.Observe(2,&low,100,pcm.data(),pcm.size(),16000));
 // A decoder-rate switch also starts a fresh reporting window.
 r=stats.Observe(2,&high,6000,clean.data(),clean.size(),8000);
 assert(r&&r->packets==1&&r->minimum_band_khz==20&&r->sample_rate==8000);
}
''')
