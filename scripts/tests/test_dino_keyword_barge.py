"""Exercise the dinosaur-only keyword confirmation and candidate lifetime."""
from pathlib import Path
import unittest

import test_voice_barge_in as barge
from test_wzh_application_interaction import function
import scripts.build_default_assets as assets

ROOT = Path(__file__).resolve().parents[2]


class DinoKeywordBargeTest(unittest.TestCase):
    run_cpp = barge.VoiceBargeInTest.run_cpp

    def test_recent_candidate_expires_and_rearms(self):
        self.run_cpp('#define CONFIG_WZH_DINO_KEYWORD_BARGE_IN 1\n' + barge.HEADER + r'''
#include <cassert>
int main(){
 VoiceBargeIn detector;auto token=detector.Begin();auto speech=BargeWave(1500);int64_t now=0;
 bool found=false;
 for(int i=0;i<12&&!found;++i){now+=60000;FeedBargeReference(detector,now);
  found=detector.Observe(speech.data(),speech.size(),true,now);}
 assert(found&&detector.pending(token,now));
 for(int i=0;i<51;++i){now+=60000;FeedBargeReference(detector,now);}
 assert(!detector.pending(token,now));
 detector.Observe(speech.data(),speech.size(),true,now);
 assert(detector.token()!=token&&!detector.pending(token,now));
 token=detector.token();found=false;
 for(int i=0;i<12&&!found;++i){now+=60000;FeedBargeReference(detector,now);
  found=detector.Observe(speech.data(),speech.size(),true,now);}
 assert(found&&detector.pending(token,now));
 assert(detector.Commit(token,now));
}
''')

    def test_only_keyword_with_recent_candidate_cancels(self):
        source = (ROOT / 'main/application.cc').read_text()
        wake = function(source, 'void Application::HandleWakeWordDetectedEvent()')
        wake = wake[:wake.index('    if (!protocol_)')] + '}\n'
        callback = function(source, 'callbacks.on_voice_interruption =') + ';'
        self.run_cpp(r'''
#define CONFIG_USE_VOICE_BARGE_IN 1
#define CONFIG_WZH_DINO_KEYWORD_BARGE_IN 1
#define ESP_LOGI(...) ((void)0)
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
enum {kDeviceStateIdle,kDeviceStateSpeaking,kDeviceStateListening,kDeviceStateStarting,
      kDeviceStateWifiConfiguring,kDeviceStateActivating};
enum {MAIN_EVENT_VOICE_INTERRUPTION=1};
unsigned bits=0;void xEventGroupSetBits(void*,unsigned b){bits|=b;}
struct Audio {std::string word;bool monitoring=true,pending=true,wake=false;
 std::string GetLastWakeWord(){return word;} bool IsVoiceInterruptionMonitoring(){return monitoring;}
 bool IsVoiceInterruptionPending(uint32_t token){return pending&&token==42;}
 bool IsAfeWakeWord(){return true;} void EnableWakeWordDetection(bool v){wake=v;}};
struct Board {static Board& GetInstance(){static Board b;return b;}
 bool IsAudioMaintenanceActive(){return false;} bool HandleLocalVoiceCommand(const std::string&){return true;}
 bool SupportsLocalInteraction(){return true;}};
struct Callbacks {std::function<void(uint32_t)> on_voice_interruption;};
struct Application {Audio audio_service_;std::atomic<bool>voice_enrollment_active_{false};
 std::atomic<uint32_t>pending_voice_interruption_{0};int state=kDeviceStateSpeaking,aborts=0;
 void* event_group_=nullptr;Callbacks callbacks;
 int GetDeviceState(){return state;} void HandleVoiceInterruptionEvent(){++aborts;}
 void ConfigureWakeWordForListening(){} void HandleWakeWordDetectedEvent();
 void Install(){
''' + callback + r'''
 }
};
''' + wake + r'''
int main(){Application a;a.Install();a.callbacks.on_voice_interruption(42);
 assert(bits==0&&a.pending_voice_interruption_==42&&a.aborts==0);
 a.audio_service_.word="local:happy";a.HandleWakeWordDetectedEvent();assert(a.aborts==0);
 a.audio_service_.word="local:interrupt";a.audio_service_.pending=false;
 a.HandleWakeWordDetectedEvent();assert(a.aborts==0&&a.audio_service_.wake);
 a.audio_service_.pending=true;a.HandleWakeWordDetectedEvent();assert(a.aborts==1);
 a.audio_service_.word="local:stop";a.HandleWakeWordDetectedEvent();assert(a.aborts==2);
 a.audio_service_.word="小龙小龙";a.HandleWakeWordDetectedEvent();assert(a.aborts==3);
 a.audio_service_.monitoring=false;a.HandleWakeWordDetectedEvent();assert(a.aborts==3);
}
''')

    def test_keyword_vocabulary_is_opt_in_and_dinosaur_only(self):
        wanted = {'ting', 'ting yi xia', 'deng yi xia', 'bie shuo hua',
                  'bie shuo le', 'xian bie shuo', 'ni hao', 'wo you wen ti', 'wo yao shuo hua'}
        def keywords(product, enabled):
            return {entry['command'] for entry in assets.wzh_offline_commands(product, enabled)
                    if entry['text'] == 'local:interrupt'}
        self.assertEqual(keywords('dinosaur', True), wanted)
        for product in ('dinosaur', 'crawl', 'tail'):
            self.assertFalse(keywords(product, False))
        self.assertFalse(keywords('crawl', True))
        self.assertFalse(keywords('tail', True))
