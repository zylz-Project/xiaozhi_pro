"""Exercise the diagnostic probe lifecycle; does not contact any server."""
from pathlib import Path
import json
import unittest

from test_wzh_action_queue import queue_function
import test_firmware_runtime_optimizations as runtime_tests

ROOT = Path(__file__).resolve().parents[2]


class OfficialAsrProbeTest(unittest.TestCase):
    run_cpp = runtime_tests.FirmwareRuntimeOptimizationsTest.run_cpp

    def test_probe_is_opt_in_dinosaur_and_preserves_vadnet_test(self):
        builds = json.loads((ROOT / 'main/boards/wzh_dinosaur/config.json').read_text())['builds']
        trial = next(item for item in builds if item['name'] == 'wzh-dinosaur-official-asr-probe')
        self.assertIn('CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE=y', trial['sdkconfig_append'])
        self.assertIn('CONFIG_USE_AUDIO_DEBUGGER=n', trial['sdkconfig_append'])
        for item in builds:
            if item is not trial:
                self.assertNotIn('CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE=y', item['sdkconfig_append'])

    def test_web_recording_is_rejected_while_probe_owns_the_microphone(self):
        source = (ROOT / 'main/application.cc').read_text()
        request = queue_function(source, 'bool Application::RequestDeviceVoiceEnrollment(')
        self.assertLess(request.index('official_probe_active_.load()'),
                        request.index('voice_enrollment_active_.exchange(true)'))

    def test_bounded_probe_starts_manual_without_local_abort_and_recovers(self):
        source = (ROOT / 'main/application.cc').read_text()
        begin = queue_function(source, 'void Application::BeginOfficialAsrProbe(')
        service = queue_function(source, 'void Application::ServiceOfficialAsrProbe(')
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#define TAG "Application"
void mock_log(...){}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
int64_t now_us=1000000;
int64_t esp_timer_get_time(){return now_us;}
enum {kListeningModeManualStop=1,kDeviceStateSpeaking=2,kDeviceStateListening=3};
struct Protocol{
 bool open=true;std::vector<int>events;
 bool IsAudioChannelOpened(){return open;}
 void SendStartListening(int mode){assert(mode==1);events.push_back(1);}
 void SendStopListening(){events.push_back(2);}
};
struct Audio{
 bool commit=true,idle=true;int commits=0,disable=0;
 bool CommitVoiceInterruption(uint32_t token,bool retain){assert(token==9&&!retain);++commits;return commit;}
 void EnableVoiceProcessing(bool enable){assert(!enable);++disable;}
 bool IsPlaybackIdle(){return idle;}
};
struct Application {
 std::shared_ptr<Protocol>protocol_;Audio audio_service_;
 std::atomic<bool>official_probe_active_{false},official_probe_stt_received_{false},interaction_stopped_{false};
 int64_t official_probe_started_us_=0;bool official_probe_sent_stop_=false,official_probe_used_=false;
 uint32_t official_probe_packets_sent_=0,official_probe_packets_failed_=0;
 bool pending_listening_start_=false;int state=kDeviceStateSpeaking,starts=0;
 int GetDeviceState(){return state;}
 void StartListeningAudio(){++starts;}
 void BeginOfficialAsrProbe(uint32_t);
 void ServiceOfficialAsrProbe();
};
''' + begin + service + r'''
int main(){
 Application a;a.protocol_=std::make_shared<Protocol>();a.BeginOfficialAsrProbe(9);
 assert(a.official_probe_active_&&a.audio_service_.commits==1&&a.protocol_->events==std::vector<int>{1});
 a.BeginOfficialAsrProbe(9);assert(a.audio_service_.commits==1);
 now_us+=1999999;a.ServiceOfficialAsrProbe();assert(a.audio_service_.disable==0);
 now_us++;a.ServiceOfficialAsrProbe();assert(a.audio_service_.disable==1&&a.protocol_->events==std::vector<int>({1,2}));
 a.official_probe_stt_received_=true;a.state=kDeviceStateListening;
 now_us+=4000000;a.ServiceOfficialAsrProbe();assert(!a.official_probe_active_&&a.starts==1);
 Application b;b.protocol_=std::make_shared<Protocol>();b.audio_service_.commit=false;b.BeginOfficialAsrProbe(9);
 assert(!b.official_probe_active_&&b.protocol_->events==std::vector<int>({1,2}));
 Application c;c.protocol_=std::make_shared<Protocol>();c.BeginOfficialAsrProbe(9);
 c.state=kDeviceStateListening;c.audio_service_.idle=false;now_us+=6000000;c.ServiceOfficialAsrProbe();
 assert(c.pending_listening_start_&&c.starts==0&&!c.official_probe_active_);
 Application d;d.protocol_=std::make_shared<Protocol>();d.protocol_->open=false;d.BeginOfficialAsrProbe(9);
 assert(!d.official_probe_used_&&!d.official_probe_active_);
 Application e;e.protocol_=std::make_shared<Protocol>();e.BeginOfficialAsrProbe(9);
 e.protocol_->open=false;e.ServiceOfficialAsrProbe();
 assert(!e.official_probe_active_&&e.audio_service_.disable==1&&e.starts==0);
}
''')

    def test_candidate_routes_to_probe_without_committing_or_aborting_in_handler(self):
        source = (ROOT / 'main/application.cc').read_text()
        handler = queue_function(source, 'void Application::HandleVoiceInterruptionEvent(')
        self.run_cpp(r'''
#define CONFIG_WZH_DINO_OFFICIAL_ASR_PROBE 1
#include <atomic>
#include <cassert>
#include <cstdint>
enum {kDeviceStateSpeaking=1,kDeviceStateListening=2};
struct Audio {bool pending=true;bool IsVoiceInterruptionPending(uint32_t t){return pending&&t==7;}};
struct Application {
 std::atomic<bool>voice_enrollment_active_{false},voice_enrollment_reconnecting_{false};
 std::atomic<bool>barge_in_reconnecting_{false},interaction_stopped_{false};
 std::atomic<uint32_t>pending_voice_interruption_{7};
 Audio audio_service_;bool pending_listening_start_=false;int state=kDeviceStateSpeaking,probes=0;
 int GetDeviceState(){return state;}
 void BeginOfficialAsrProbe(uint32_t token){assert(token==7);++probes;}
 void HandleVoiceInterruptionEvent();
};
''' + handler + r'''
int main(){Application a;a.HandleVoiceInterruptionEvent();assert(a.probes==1);
 a.interaction_stopped_=true;a.HandleVoiceInterruptionEvent();assert(a.probes==1);
 a.interaction_stopped_=false;a.audio_service_.pending=false;
 a.HandleVoiceInterruptionEvent();assert(a.probes==1);}
''')


if __name__ == '__main__':
    unittest.main()
