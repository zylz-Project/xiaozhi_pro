"""Exercise the dinosaur-only keyword confirmation and candidate lifetime."""
from pathlib import Path
import unittest

import test_voice_barge_in as barge
from test_wzh_application_interaction import function
import scripts.build_default_assets as assets

ROOT = Path(__file__).resolve().parents[2]


class DinoKeywordBargeTest(unittest.TestCase):
    run_cpp = barge.VoiceBargeInTest.run_cpp

    def test_tail_keyword_accepts_one_qualified_frame_but_rejects_echo(self):
        self.run_cpp('#define CONFIG_BOARD_TYPE_WZH_TAILREDPANDA 1\n'
                     '#define CONFIG_WZH_DINO_KEYWORD_BARGE_IN 1\n' + barge.HEADER + r'''
#include <cassert>
int main() {
 VoiceBargeIn detector;
 const auto token = detector.Begin();
 const auto speech = BargeWave(280);
 const auto echo = BargeWave(1000, true);
 int64_t now = 0;
 for (int i=0; i<2; ++i) {
  now += 60000;
  FeedBargeReference(detector, now);
  assert(!detector.Observe(speech.data(), speech.size(), false, now, false, 0, true));
 }
 assert(detector.threshold()==125 && detector.settling_ms()==120);
 now += 60000;
 FeedBargeReference(detector, now);
 assert(!detector.Observe(echo.data(), echo.size(), true, now, false, 0, true));
 assert(detector.NoteKeyword(now, false)==0);
 now += 60000;
 FeedBargeReference(detector, now);
 assert(detector.Observe(speech.data(), speech.size(), true, now, false, 0, true));
 assert(detector.verified_speech_ms()==60 && detector.required_speech_ms()==60);
 assert(detector.MatchedKeyword(now, false)==token);
 for (int i=0; i<3; ++i) {
  now += 60000;
  FeedBargeReference(detector, now, 0);
  assert(!detector.Observe(speech.data(), speech.size(), false, now, false, 0, true));
 }
 assert(!detector.ReferenceReady(now));
 assert(detector.NoteKeyword(now, false)==token);
 FeedBargeReference(detector, now+1000, 0, true);
 assert(!detector.MatchedKeyword(now+1000, false));
}
''')

    def test_tail_profile_preserves_speech_across_a_three_frame_vad_gap(self):
        self.run_cpp('#define CONFIG_WZH_DINO_KEYWORD_BARGE_IN 1\n' + barge.HEADER + r'''
#include <cassert>
int main() {
 const auto speech = BargeWave(1500);
 for (const auto limit : {1920u, 2880u}) {
  VoiceBargeIn detector;
  const auto token = detector.Begin(limit);
  int64_t now = 0;
  auto frame = [&](bool vad) {
   now += 60000;
   FeedBargeReference(detector, now);
   return detector.Observe(speech.data(), speech.size(), vad, now);
  };
  for (int i = 0; i < 5; ++i) assert(!frame(false));
  assert(!frame(true));
  for (int i = 0; i < 3; ++i) assert(!frame(false));
  const bool accepted = frame(true);
  assert(accepted == (limit == 2880u));
  assert(detector.pending(token, now) == accepted);
 }
}
''')

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

    def test_tail_keyword_pairs_in_both_event_orders_and_expires(self):
        self.run_cpp('#define CONFIG_WZH_DINO_KEYWORD_BARGE_IN 1\n' + barge.HEADER + r'''
#include <cassert>
int main() {
 VoiceBargeIn detector;
 const auto speech=BargeWave(1500);
 int64_t now=0;
 auto frame=[&](bool vad) {
  now+=60000;
  FeedBargeReference(detector,now);
  detector.Observe(speech.data(),speech.size(),vad,now,false,0,true);
  return detector.MatchedKeyword(now,false);
 };
 auto prime=[&](){detector.Begin(); for(int i=0;i<5;++i)assert(!frame(false));};
 prime();
 assert(!frame(true));
 assert(!detector.NoteKeyword(now+1000,false));
 assert(frame(true));
 assert(detector.Commit(detector.token(),now));
 assert(!detector.ExpireKeyword(now+500000));
 prime();
 assert(!frame(true)&&!frame(true));
 assert(detector.NoteKeyword(now+1000,false)==detector.token());
 assert(detector.Commit(detector.token(),now+1000));
 prime();
 assert(!detector.NoteKeyword(now,false));
 for(int i=0;i<7;++i)assert(!frame(false));
 assert(detector.ExpireKeyword(now));
 assert(!frame(true)&&!frame(true));
 assert(!detector.MatchedKeyword(now,false));
 assert(detector.NoteKeyword(now,false));
 detector.Cancel();
 prime();
 assert(!frame(true)&&!frame(true));
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
 a.audio_service_.word="local:chat_wake";a.HandleWakeWordDetectedEvent();assert(a.aborts==3);
 a.audio_service_.word="小龙小龙";a.HandleWakeWordDetectedEvent();assert(a.aborts==3);
 a.audio_service_.monitoring=false;a.HandleWakeWordDetectedEvent();assert(a.aborts==3);
}
''')

    def test_keyword_vocabulary_is_opt_in_for_all_products(self):
        wanted = {'ting', 'ting yi xia', 'deng yi xia', 'bie shuo hua',
                  'bie shuo le', 'xian bie shuo', 'ni hao', 'wo you wen ti', 'wo yao shuo hua'}
        def keywords(product, enabled):
            return {entry['command'] for entry in assets.wzh_offline_commands(product, enabled)
                    if entry['text'] == 'local:interrupt'}
        for product in ('dinosaur', 'crawl', 'tail'):
            self.assertFalse(keywords(product, False))
            effective = keywords(product, True)
            name = {'crawl': 'xiao hua', 'tail': 'xiao wu', 'dinosaur': 'xiao wei'}[product]
            self.assertTrue(wanted | {'deng', 'bie', 'man', 'zan', 'shou', 'xiu',
                                      'zan ting', 'ni deng yi xia', name, 'xu',
                                      'deng deng', 'ting zhi', 'bie bo le',
                                      'wo cha yi ju', 'ting xia lai', 'qing ni ting',
                                      'ting bu yao jiang le'} <= effective)
            self.assertFalse(({'xiao hua', 'xiao wu', 'xiao wei'} - {name}) & effective)

    def test_each_product_has_its_own_wake_phrase(self):
        phrases = {'crawl': 'xiao hua xiao hua', 'tail': 'xiao wu xiao wu',
                   'dinosaur': 'xiao wei xiao wei'}
        for product, phrase in phrases.items():
            for enabled in (False, True):
                with self.subTest(product=product, keyword_barge=enabled):
                    commands = assets.wzh_offline_commands(product, enabled)
                    wake = [item['command'] for item in commands if item['text'] == 'local:chat_wake']
                    self.assertEqual(wake, [phrase])
