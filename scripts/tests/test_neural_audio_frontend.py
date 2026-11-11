"""Exercise production neural framing/binding with deterministic SDK doubles.

These tests verify integration, not NSNet/WebRTC acoustic accuracy.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT/'main/audio/engines'

SDK = r'''
#pragma once
#define ESP_NSNET_PREFIX "nsnet"
struct srmodel_list_t { int num; };
enum vad_mode_t { VAD_MODE_0, VAD_MODE_1, VAD_MODE_2 };
enum vad_state_t { VAD_SILENCE, VAD_SPEECH };
struct esp_nsn_data_t { int16_t history[512]; };
using vad_handle_t = void*;
struct esp_nsn_iface_t {
 esp_nsn_data_t*(*create)(char*);
 int(*get_samp_chunksize)(esp_nsn_data_t*);
 int(*process)(esp_nsn_data_t*,int16_t*,int16_t*);
 int(*get_samp_rate)(esp_nsn_data_t*);
 void(*destroy)(esp_nsn_data_t*);
};
vad_handle_t vad_create_with_param(vad_mode_t,int,int,int,int);
vad_state_t vad_process_with_trigger(vad_handle_t,int16_t*);
void vad_reset_trigger(vad_handle_t);
void vad_destroy(vad_handle_t);
char*esp_srmodel_filter(srmodel_list_t*,const char*,const char*);
const esp_nsn_iface_t*esp_nsnet_handle_from_name(char*);
'''

BACKEND = r'''
#include <cassert>
#include <cstring>
#include <functional>
#include <vector>
int ns_created=0,ns_destroyed=0,vad_created=0,vad_destroyed=0,cleaned=0;
int ns_chunk=512,ns_rate=16000;
bool fail_ns=false,fail_vad=false,missing=false,unknown=false;
std::vector<char> order;
void(*on_ns)()=nullptr;
char*esp_srmodel_filter(srmodel_list_t*,const char*prefix,const char*){
 static char ns[]="nsnet2",other[]="nsnet3";
 if(missing)return nullptr;
 assert(std::strcmp(prefix,"nsnet")==0);return unknown?other:ns;
}
esp_nsn_data_t*NsCreate(char*){if(fail_ns)return nullptr;++ns_created;return new esp_nsn_data_t{};}
int NsChunk(esp_nsn_data_t*){return ns_chunk;}
int NsRate(esp_nsn_data_t*){return ns_rate;}
void NsDestroy(esp_nsn_data_t*p){++ns_destroyed;delete p;}
int NsProcess(esp_nsn_data_t*p,int16_t*in,int16_t*out){
 assert(reinterpret_cast<uintptr_t>(in)%16==0&&reinterpret_cast<uintptr_t>(out)%16==0);
 order.push_back('N');
 for(int i=0;i<512;++i){out[i]=p->history[i];p->history[i]=in[i]/2;}
 if(on_ns)on_ns();
 return 0;
}
vad_handle_t vad_create_with_param(vad_mode_t mode,int rate,int ms,int speech,int noise){
 assert(mode==VAD_MODE_2&&rate==16000&&ms==10&&speech==60&&noise==100);
 if(fail_vad)return nullptr;
 ++vad_created;return new int{};
}
vad_state_t vad_process_with_trigger(vad_handle_t,int16_t*pcm){
 order.push_back('V');return pcm[0]>500?VAD_SPEECH:VAD_SILENCE;
}
void vad_reset_trigger(vad_handle_t){++cleaned;}
void vad_destroy(vad_handle_t p){++vad_destroyed;delete static_cast<int*>(p);}
esp_nsn_iface_t ns_interface{NsCreate,NsChunk,NsProcess,NsRate,NsDestroy};
const esp_nsn_iface_t*esp_nsnet_handle_from_name(char*){return &ns_interface;}
struct Output {
 std::vector<int16_t> pcm;std::vector<bool> speech;
 static void Emit(void*ctx,const int16_t*data,size_t count,bool voice){
  auto*self=static_cast<Output*>(ctx);order.push_back('O');
  self->pcm.insert(self->pcm.end(),data,data+count);self->speech.push_back(voice);
 }
};
'''


class NeuralAudioFrontendTest(unittest.TestCase):
    def test_vadnet_missing_model_fails_closed_for_barge_in(self):
        source=(ENGINE/'afe_audio_engine.h').read_text()
        method=queue_function(source, 'bool CanMonitorVoiceInterruption() const')
        program = r'''
#define CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND 0
#define CONFIG_VOICE_BARGE_IN_VADNET_EXPERIMENT 1
#include <cassert>
struct Base{virtual bool CanMonitorVoiceInterruption()const{return false;}};
struct Engine : Base{void* afe_data_=nullptr;bool vadnet_ready_=false;
''' + method + r'''
};
int main(){Engine e;assert(!e.CanMonitorVoiceInterruption());e.afe_data_=&e;
 assert(!e.CanMonitorVoiceInterruption());e.vadnet_ready_=true;assert(e.CanMonitorVoiceInterruption());}
'''
        import test_wzh_fluent_motion
        test_wzh_fluent_motion.WzhFluentMotionTest.run_cpp(self, program)

    def test_vadnet_afe_frames_require_speech_for_whole_packet_and_fence_stale_turns(self):
        source = (ENGINE/'afe_audio_engine.cc').read_text()
        program = r'''
#define CONFIG_USE_VOICE_BARGE_IN 1
#define CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND 0
#define CONFIG_VOICE_BARGE_IN_VADNET_EXPERIMENT 1
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
enum {VAD_SILENCE, VAD_SPEECH};
struct afe_fetch_result_t{int16_t* data;int data_size;int vad_state;};
struct AfeAudioEngine{
 std::atomic<uint32_t>control_generation_{0};std::atomic<bool>output_reset_pending_{false};
 std::vector<int16_t>output_buffer_;bool output_all_speech_=true;int frame_samples_=960;
 std::function<void(bool)>vad_state_change_callback_;
 std::function<void(std::vector<int16_t>&&)>output_callback_;
 void HandleVoiceResult(const afe_fetch_result_t*,uint32_t);
 void EmitNeuralChunk(const int16_t*,size_t,bool,uint32_t);
};
''' + queue_function(source, 'void AfeAudioEngine::HandleVoiceResult(') + \
            queue_function(source, 'void AfeAudioEngine::EmitNeuralChunk(') + r'''
int main(){
 AfeAudioEngine engine;int packets=0;std::vector<bool> detected;
 engine.vad_state_change_callback_=[&](bool speech){detected.push_back(speech);};
 engine.output_callback_=[&](std::vector<int16_t>&& pcm){assert(pcm.size()==960);++packets;};
 std::vector<int16_t> pcm(320,400);afe_fetch_result_t r{pcm.data(),640,VAD_SPEECH};
 engine.HandleVoiceResult(&r,0);engine.HandleVoiceResult(&r,0);r.vad_state=VAD_SILENCE;
 engine.HandleVoiceResult(&r,0);assert(packets==1&&!detected[0]);
 r.vad_state=VAD_SPEECH;for(int i=0;i<3;++i)engine.HandleVoiceResult(&r,0);
 assert(packets==2&&detected[1]);
 engine.HandleVoiceResult(&r,0);engine.control_generation_=1;engine.output_reset_pending_=true;
 engine.HandleVoiceResult(&r,0);assert(packets==2);
 for(int i=0;i<3;++i){engine.HandleVoiceResult(&r,1);}assert(packets==3&&detected[2]);
}
'''
        import test_wzh_fluent_motion
        test_wzh_fluent_motion.WzhFluentMotionTest.run_cpp(self, program)

    def run_frontend(self, program):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder/'fake_sdk.h').write_text(SDK)
            for header in ['esp_nsn_models.h', 'esp_vad.h', 'model_path.h']:
                (folder/header).write_text('#include "fake_sdk.h"\n')
            source = folder/'test.cc'
            source.write_text('#include "'+str(ENGINE/'neural_audio_frontend.h')+'"\n'+BACKEND+program)
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(folder), str(source),
                                     str(ENGINE/'neural_audio_frontend.cc'), '-o', str(folder/'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(folder/'test')], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_nsnet_precedes_vad_and_arbitrary_input_chunks_preserve_samples(self):
        self.run_frontend(r'''
int main(){
 srmodel_list_t models{2};NeuralAudioFrontend pipeline;assert(pipeline.Initialize(&models));
 order.clear();Output result;std::vector<int16_t> input(2560);
 for(size_t i=0;i<input.size();++i)input[i]=2000+2*(i%511);
 assert(pipeline.Process(input.data(),511,Output::Emit,&result));assert(result.pcm.empty());
 assert(pipeline.Process(input.data()+511,5,Output::Emit,&result));
 assert(pipeline.Process(input.data()+516,input.size()-516,Output::Emit,&result));
 assert(std::count(order.begin(),order.end(),'N')==5);
 assert(std::count(order.begin(),order.end(),'V')==16);
 assert(std::count(order.begin(),order.end(),'O')==16);
 for(size_t i=0;i<order.size();++i)if(order[i]=='O')assert(i>0&&order[i-1]=='V');
 assert(result.pcm.size()==input.size());
 for(size_t i=0;i<512;++i)assert(result.pcm[i]==0);
 for(size_t i=512;i<input.size();++i)assert(result.pcm[i]==input[i-512]/2);
 assert(result.speech.size()==16);
 for(size_t i=0;i<result.speech.size();++i)assert(result.speech[i]==(i>=4));
 pipeline.Close();pipeline.Close();assert(ns_created==ns_destroyed&&vad_created==vad_destroyed);
}
''')

    def test_reset_flushes_previous_waveform_and_partial_input_without_recreation(self):
        self.run_frontend(r'''
int main(){
 srmodel_list_t models{2};NeuralAudioFrontend pipeline;assert(pipeline.Initialize(&models));
 Output result;std::vector<int16_t> old(700,3000),fresh(2560,0);
 pipeline.Process(old.data(),old.size(),Output::Emit,&result);
 const int created=ns_created+vad_created,clean_before=cleaned;
 pipeline.Reset();assert(cleaned==clean_before+1&&ns_created+vad_created==created);
 result={};pipeline.Process(fresh.data(),fresh.size(),Output::Emit,&result);
 assert(result.pcm.size()==fresh.size());
 for(auto sample:result.pcm)assert(sample==0);
 for(bool speech:result.speech)assert(!speech);
 assert(!pipeline.Process(nullptr,512,Output::Emit,&result));
 assert(!pipeline.Process(fresh.data(),0,Output::Emit,&result));
 assert(!pipeline.Process(fresh.data(),512,nullptr,&result));
}
''')

    def test_missing_models_allocations_and_incompatible_formats_fail_closed(self):
        self.run_frontend(r'''
int main(){
 srmodel_list_t models{2};NeuralAudioFrontend pipeline;
 assert(!pipeline.Initialize(nullptr));
 for(int failure=0;failure<6;++failure){
  missing=failure==0;unknown=failure==1;fail_ns=failure==2;fail_vad=failure==3;
  ns_chunk=failure==4?256:512;ns_rate=failure==5?8000:16000;
  assert(!pipeline.Initialize(&models)&&!pipeline.Ready());
  assert(ns_created==ns_destroyed&&vad_created==vad_destroyed);
 }
 missing=unknown=fail_ns=fail_vad=false;ns_chunk=512;ns_rate=16000;
 assert(pipeline.Initialize(&models)&&pipeline.Ready());
 pipeline.Close();assert(ns_created==ns_destroyed&&vad_created==vad_destroyed);
}
''')

    def test_packet_vad_covers_every_sample_and_session_fences_reject_stale_output(self):
        source = (ENGINE/'afe_audio_engine.cc').read_text()
        program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
struct AfeAudioEngine {
 std::atomic<uint32_t>control_generation_{0};std::atomic<bool>output_reset_pending_{false};
 std::vector<int16_t>output_buffer_;int frame_samples_=960;bool output_all_speech_=true;
 std::function<void(bool)>vad_state_change_callback_;
 std::function<void(std::vector<int16_t>&&)>output_callback_;
 void EmitNeuralChunk(const int16_t*,size_t,bool,uint32_t);
};
''' + queue_function(source, 'void AfeAudioEngine::EmitNeuralChunk(') + r'''
int main(){
 AfeAudioEngine engine;std::vector<bool>states;int packets=0;
 engine.vad_state_change_callback_=[&](bool v){states.push_back(v);};
 engine.output_callback_=[&](std::vector<int16_t>&&pcm){assert(pcm.size()==960);++packets;};
 std::vector<int16_t>data(512,1000);
 engine.EmitNeuralChunk(data.data(),512,false,0);
 engine.EmitNeuralChunk(data.data(),512,true,0);
 assert(packets==1&&!states.back()); // Latest 32 ms state cannot relabel older silence.
 engine.EmitNeuralChunk(data.data(),512,true,0);
 engine.EmitNeuralChunk(data.data(),512,true,0);
 assert(packets==2&&states.back());
 engine.control_generation_=1;engine.EmitNeuralChunk(data.data(),512,true,0);assert(packets==2);
 engine.output_reset_pending_=true;engine.EmitNeuralChunk(data.data(),512,true,1);assert(packets==2);
 engine.output_reset_pending_=false;engine.output_buffer_.clear();engine.output_all_speech_=true;
 engine.vad_state_change_callback_=[&](bool){++engine.control_generation_;};
 engine.EmitNeuralChunk(data.data(),512,true,1);engine.EmitNeuralChunk(data.data(),512,true,1);
 assert(packets==2); // Invalidated by the VAD callback before PCM delivery.
 engine.frame_samples_=0;engine.EmitNeuralChunk(data.data(),512,true,2); // Bounded failure.
}
'''
        import test_wzh_fluent_motion
        test_wzh_fluent_motion.WzhFluentMotionTest.run_cpp(self, program)

    def test_afe_uses_post_nsnet_vad_and_discards_results_invalidated_during_inference(self):
        source = (ENGINE/'afe_audio_engine.cc').read_text()
        self.run_frontend(r'''
#define CONFIG_USE_VOICE_BARGE_IN 1
#define CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#include <atomic>
int64_t esp_timer_get_time(){return 10000000;}
struct afe_fetch_result_t{int16_t*data;int data_size;int vad_state;};
struct AfeAudioEngine{
 NeuralAudioFrontend neural_frontend_;std::atomic<uint32_t>control_generation_{0};
 std::atomic<bool>output_reset_pending_{false};std::vector<int16_t>output_buffer_;
 bool output_all_speech_=true;int frame_samples_=960;
 uint32_t neural_budget_misses_=0,neural_timing_frames_=0;
 int64_t last_neural_budget_log_us_=-5000000,neural_timing_sum_us_=0,neural_timing_max_us_=0;
 bool local_commands_suspended_=false;
 std::function<void(bool)>vad_state_change_callback_;
 std::function<void(std::vector<int16_t>&&)>output_callback_;
 void HandleVoiceResult(const afe_fetch_result_t*,uint32_t);
 void EmitNeuralChunk(const int16_t*,size_t,bool,uint32_t);
};
''' + queue_function(source, 'void AfeAudioEngine::HandleVoiceResult(')
            + queue_function(source, 'void AfeAudioEngine::EmitNeuralChunk(') + r'''
AfeAudioEngine*target=nullptr;
void Invalidate(){++target->control_generation_;target->output_reset_pending_=true;on_ns=nullptr;}
int main(){
 srmodel_list_t models{2};AfeAudioEngine engine;assert(engine.neural_frontend_.Initialize(&models));
 int packets=0;std::vector<bool>speech;
 engine.vad_state_change_callback_=[&](bool voice){speech.push_back(voice);};
 engine.output_callback_=[&](std::vector<int16_t>&&pcm){assert(pcm.size()==960);++packets;};
 std::vector<int16_t>pcm(512,2000);afe_fetch_result_t result{pcm.data(),1024,VAD_SILENCE};
 for(int i=0;i<4;++i)engine.HandleVoiceResult(&result,0);
 assert(packets==2&&speech==std::vector<bool>({false,true}));
 target=&engine;on_ns=Invalidate;engine.HandleVoiceResult(&result,0);assert(packets==2);
 pcm.assign(512,0);for(int i=0;i<4;++i)engine.HandleVoiceResult(&result,1);
 assert(packets==4&&!speech[2]&&!speech[3]);
 engine.HandleVoiceResult(nullptr,1);assert(packets==4);
}
''')
