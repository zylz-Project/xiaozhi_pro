"""Offline prompt-pack validation, CMake symbol selection and production worker tests."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('system_prompts', ROOT/'scripts/audio/prepare_wzh_system_prompts.py')
prompts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prompts)


class PromptPackTest(unittest.TestCase):
    def fixture(self, directory, profile='dinosaur'):
        directory.mkdir()
        manifest = prompts.load_manifest()
        files = {}
        for key, text in manifest['prompts'].items():
            path = directory/(key+'.ogg')
            # Container fixture only, NOT a recording of the requested voice.
            shutil.copyfile(ROOT/'main/assets/common/success.ogg', path)
            files[key] = {'text': text, 'sha256': prompts.sha256(path)}
        pack = {'schema': 1, 'profile': profile, 'provider': 'fixture', 'voice_id': 'test-tone',
                'requested_voice': manifest['profiles'][profile]['requested_voice'],
                'identity_confirmed': False, 'files': files}
        (directory/'pack.json').write_text(json.dumps(pack))
        return pack

    def test_identity_mismatch_is_explicit_and_changes_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)/'pack'
            pack = self.fixture(directory)
            with self.assertRaisesRegex(ValueError, 'confirmed'):
                prompts.validate_pack(directory, 'dinosaur')
            with mock.patch.object(prompts, 'inspect_audio', return_value=0.5):
                prompts.validate_pack(directory, 'dinosaur', True)
                with self.assertRaisesRegex(ValueError, 'profile'):
                    prompts.validate_pack(directory, 'panda', True)
                (directory/'0.ogg').write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'checksum'):
                    prompts.validate_pack(directory, 'dinosaur', True)
            pack['provider'] = 'edge'
            pack['identity_confirmed'] = True
            (directory/'pack.json').write_text(json.dumps(pack))
            with self.assertRaisesRegex(ValueError, 'Edge'):
                prompts.validate_pack(directory, 'dinosaur')

    def test_incomplete_pack_and_stale_transcripts_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)/'pack'
            pack = self.fixture(directory)
            pack['files']['activation']['text'] = 'stale text'
            (directory/'pack.json').write_text(json.dumps(pack))
            with mock.patch.object(prompts, 'inspect_audio', return_value=0.5):
                with self.assertRaisesRegex(ValueError, 'transcript'):
                    prompts.validate_pack(directory, 'dinosaur', True)
            (directory/'activation.ogg').unlink()
            with self.assertRaisesRegex(ValueError, 'Missing'):
                prompts.validate_pack(directory, 'dinosaur', True)

    def test_failed_generation_never_publishes_or_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/'out'
            with self.assertRaisesRegex(ValueError, 'exactly one'):
                prompts.prepare_pack(output, 'panda', 'import', source_dir=tmp, voice_id='authorized')
            self.assertFalse(output.exists())
            output.mkdir()
            (output/'keep').write_text('existing')
            with self.assertRaisesRegex(ValueError, 'already exists'):
                prompts.prepare_pack(output, 'panda', 'edge')
            self.assertEqual((output/'keep').read_text(), 'existing')
            with self.assertRaisesRegex(ValueError, 'alternatives'):
                prompts.prepare_pack(Path(tmp)/'other', 'panda', 'edge', confirm_identity=True)

    def test_network_failure_does_not_leave_a_pack_or_claim_target_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/'edge-output'
            with mock.patch.object(prompts, 'edge_source', side_effect=RuntimeError('service unavailable')):
                with self.assertRaises(RuntimeError):
                    prompts.prepare_pack(output, 'panda', 'edge')
            self.assertFalse(output.exists())
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_generated_pack_levels_must_match_the_unified_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)/'pack'
            pack = self.fixture(directory)
            pack['files']['power_off']['rms_dbfs'] = -19.5
            pack['files']['power_off']['tempo'] = 0.92
            (directory/'pack.json').write_text(json.dumps(pack))
            with mock.patch.object(prompts, 'inspect_audio', return_value=0.5), \
                    mock.patch.object(prompts, 'analyze_level', return_value=(-24.0, -6.0)):
                with self.assertRaisesRegex(ValueError, 'unified target'):
                    prompts.validate_pack(directory, 'dinosaur', True)
            with mock.patch.object(prompts, 'inspect_audio', return_value=0.5), \
                    mock.patch.object(prompts, 'analyze_level', return_value=(-19.6, -1.6)):
                prompts.validate_pack(directory, 'dinosaur', True)
            pack['files']['power_off']['tempo'] = 1.0
            (directory/'pack.json').write_text(json.dumps(pack))
            with self.assertRaisesRegex(ValueError, 'tempo'):
                prompts.validate_pack(directory, 'dinosaur', True)

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'FFmpeg required')
    def test_real_conversion_and_import_without_any_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root/'sources'
            source.mkdir()
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            'sine=frequency=440:duration=0.3', str(source/'power_off.wav')],
                           check=True, capture_output=True)
            manifest = prompts.load_manifest()
            manifest['prompts'] = {'power_off': manifest['prompts']['power_off']}
            output = root/'pack'
            prompts.prepare_pack(output, 'panda', 'import', source, 'fixture-tone', manifest=manifest)
            with self.assertRaisesRegex(ValueError, 'confirmed'):
                prompts.validate_pack(output, 'panda', manifest=manifest)
            pack = prompts.validate_pack(output, 'panda', True, manifest)
            self.assertGreater(pack['files']['power_off']['duration_seconds'], 0.2)
            self.assertEqual(pack['voice_id'], 'fixture-tone')
            manifest['max_duration_seconds']['power_off'] = 0.1
            with self.assertRaisesRegex(ValueError, 'duration'):
                prompts.validate_pack(output, 'panda', True, manifest)

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ffmpeg') and shutil.which('ffprobe'),
                         'CMake and FFmpeg required')
    def test_cmake_embeds_only_selected_profile_without_duplicate_symbols(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for profile in ['dinosaur', 'panda']:
                self.fixture(root/profile, profile)
            for board, profile in [('DINOSAUR', 'dinosaur'), ('CRAWLPANDA', 'panda'), ('TAILREDPANDA', 'panda')]:
                project = root/board
                project.mkdir()
                (project/'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(prompt_selection NONE)
set(PROJECT_DIR "{ROOT}")
set(CONFIG_BOARD_TYPE_WZH_{board} TRUE)
set(CONFIG_WZH_SYSTEM_PROMPT_ROOT "{root}")
set(CONFIG_WZH_SYSTEM_PROMPT_ALLOW_ALTERNATIVE TRUE)
file(GLOB LANG_SOUNDS "{ROOT}/main/assets/locales/zh-CN/*.ogg")
file(GLOB COMMON_SOUNDS "{ROOT}/main/assets/common/*.ogg")
include("{ROOT}/scripts/cmake/wzh_system_prompts.cmake")
set(NAMES "")
foreach(SOUND IN LISTS LANG_SOUNDS COMMON_SOUNDS)
    get_filename_component(NAME "${{SOUND}}" NAME)
    if(NAME IN_LIST NAMES)
        message(FATAL_ERROR "duplicate embedded symbol: ${{NAME}}")
    endif()
    list(APPEND NAMES "${{NAME}}")
    file(APPEND "${{CMAKE_BINARY_DIR}}/selected.txt" "${{SOUND}}\\n")
endforeach()
''')
                result = subprocess.run(['cmake', '-S', str(project), '-B', str(project/'host-config')],
                                        capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                selected = (project/'host-config/selected.txt').read_text().splitlines()
                for key in prompts.load_manifest()['prompts']:
                    self.assertIn(str(root/profile/(key+'.ogg')), selected)
                self.assertIn(str(ROOT/'main/assets/common/popup.ogg'), selected)
                self.assertFalse(any('/'+('panda' if profile=='dinosaur' else 'dinosaur')+'/' in p
                                     for p in selected))


class PromptWorkerTest(unittest.TestCase):
    def compile_run(self, program, stubs=None):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for name, text in (stubs or {}).items():
                path = directory/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            source = directory/'test.cc'
            source.write_text(program)
            executable = directory/'test'
            result = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(directory), '-I', str(ROOT/'main/boards/common/wzh'),
                                     str(source), '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_low_battery_rearms_only_after_real_recovery(self):
        self.compile_run(r'''
#include <cassert>
#include "wzh_system_prompts.h"
int main(){
 wzh::LowBatteryPromptGate gate;
 gate.Observe(false,true);assert(!gate.Take());
 gate.Observe(true,true);assert(gate.Take()&&!gate.Take());
 gate.RetryAfterInterruption();assert(gate.Take()&&!gate.Take());
 gate.Observe(false,false);gate.RetryAfterInterruption();
 gate.Observe(true,true);assert(!gate.Take());
 gate.Observe(true,false);assert(!gate.Take());
 gate.Observe(true,true);assert(gate.Take());
}
''')

    def test_production_worker_shutdown_drain_timeout_and_battery_checks(self):
        self.compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#define CONFIG_WZH_SYSTEM_PROMPTS 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdPASS=1;
uint32_t clock_ms=0;bool inside_worker=false,drive_worker=true,cancel_at_worker_delay=false;
bool cancel_shutdown_after_play=false;
unsigned worker_turns=1;
struct StopWorker{};
void (*entry)(void*)=nullptr;
uint32_t esp_log_timestamp(){return clock_ms;}
int xTaskCreate(void(*callback)(void*),const char*,int,void*,int,void*){entry=callback;return pdPASS;}
void vTaskDelay(unsigned);
constexpr int kDeviceStateIdle=0;
struct Audio {
 uint32_t generation=0,until=0,drained=0;int low=0,off=0,wifi=0,resets=0;bool stalled=false;
 void settle(){if(!stalled&&until&&clock_ms>=until){++drained;until=0;}}
 void EnableWakeWordDetection(bool){}
 void EnableVoiceProcessing(bool){}
 uint32_t GetPlaybackGeneration(){return generation;}
 uint32_t GetPlaybackDrainedCount(){settle();return drained;}
 bool IsPlaybackIdle(){settle();return until==0;}
 bool IsAudioProcessorRunning(){return false;}
 void ResetDecoder(){settle();++generation;++resets;until=0;}
 void PlaySound(std::string_view sound,std::optional<uint32_t> expected={}){
  if(expected&&*expected!=generation)return;
  if(sound=="wifi"){++wifi;until=clock_ms+500;}
  else if(sound=="off"){
   ++off;until=clock_ms+1200;
   // Simulate an application-side reset landing right after enqueue.
   if(cancel_shutdown_after_play){cancel_shutdown_after_play=false;++generation;++resets;until=0;}
  }else{++low;until=clock_ms+500;}
 }
};
struct Application {
 int state=0;Audio audio;
 static Application& GetInstance(){static Application app;return app;}
 Audio& GetAudioService(){return audio;}int GetDeviceState(){return state;}
 void Schedule(std::function<void()> callback){callback();}
};
struct Board {
 bool maintenance=false;
 static Board& GetInstance(){static Board board;return board;}
 bool IsAudioMaintenanceActive(){return maintenance;}
};
namespace Lang{namespace Sounds{constexpr std::string_view OGG_POWER_OFF="off",OGG_LOW_BATTERY="low",OGG_WIFICONFIG="wifi";}}
#include "''' + str(ROOT/'main/boards/common/wzh/wzh_system_prompts.cc') + r'''"
void run_worker(unsigned turns=1){
 worker_turns=turns;inside_worker=true;try{entry(nullptr);}catch(StopWorker&){}inside_worker=false;
}
void vTaskDelay(unsigned ms){
 if(inside_worker){
  if(--worker_turns==0)throw StopWorker{};
  clock_ms+=ms;
  if(cancel_at_worker_delay){cancel_at_worker_delay=false;Application::GetInstance().audio.ResetDecoder();}
  return;
 }
 clock_ms+=ms;if(drive_worker)run_worker();
}
int main(){
 auto& app=Application::GetInstance();auto& audio=app.audio;
 wzh::StartSystemPrompts();assert(entry);
 // Startup audio and asynchronous queue resets must finish before the prompt.
 audio.until=1000;wzh::RequestWifiSetupPrompt();clock_ms=700;run_worker();assert(audio.wifi==0);
 audio.ResetDecoder();clock_ms=1100;run_worker();assert(audio.wifi==1);
 clock_ms=1700;run_worker();assert(audio.wifi==1);
 wzh::RequestWifiSetupPrompt();app.state=1;run_worker();app.state=0;
 clock_ms=2500;run_worker();assert(audio.wifi==1);
 clock_ms=0;audio.until=0;
 wzh::ObserveSystemPromptBattery(true,true);
 app.state=1;run_worker();assert(audio.low==0);
 app.state=0;Board::GetInstance().maintenance=true;run_worker();assert(audio.low==0);
 Board::GetInstance().maintenance=false;run_worker();assert(audio.low==1);
 clock_ms=1000;run_worker();assert(audio.low==1);
 // Real worker loop: cancellation after the producer returned must re-arm the
 // reminder, not silently consume the episode. The second loop retries it.
 wzh::ObserveSystemPromptBattery(true,false);wzh::ObserveSystemPromptBattery(true,true);
 cancel_at_worker_delay=true;run_worker(2);assert(audio.low==3);
 // A reminder that already finished must not be re-armed by a reset that lands
 // before the worker's next poll (e.g. entering conversation right after).
 wzh::ObserveSystemPromptBattery(true,false);wzh::ObserveSystemPromptBattery(true,true);
 clock_ms+=1000;drive_worker=false;run_worker();assert(audio.low==4);
 clock_ms+=1000;audio.ResetDecoder();run_worker();assert(audio.low==4);
 run_worker();assert(audio.low==4);
 drive_worker=true;
 wzh::BeginSystemPromptShutdown();wzh::ObserveSystemPromptBattery(true,false);
 wzh::ObserveSystemPromptBattery(true,true);run_worker();assert(audio.low==4);
 int battery_checks=0;const auto started=clock_ms;
 assert(wzh::PlayShutdownPromptAndWait([&](){++battery_checks;}));
 assert(audio.off==1&&clock_ms-started>=1200&&battery_checks>=1);
 // A reset that discards the first attempt must not power off silently: the
 // prompt is re-queued and the second sentence completes before release.
 cancel_shutdown_after_play=true;const int off_before_retry=audio.off;
 assert(wzh::PlayShutdownPromptAndWait([&](){++battery_checks;}));
 assert(audio.off==off_before_retry+2);
 audio.stalled=true;const auto timeout_start=clock_ms;
 assert(!wzh::PlayShutdownPromptAndWait([&](){++battery_checks;}));
 assert(clock_ms-timeout_start==8000&&audio.resets>=3);
 audio.stalled=false;drive_worker=false;const auto unscheduled_start=clock_ms;
 assert(!wzh::PlayShutdownPromptAndWait([&](){++battery_checks;}));
 assert(clock_ms-unscheduled_start==8000);
 const int off_before=audio.off;run_worker();assert(audio.off==off_before);
}
''', {name: '' for name in ['application.h', 'board.h', 'assets/lang_config.h',
                            'esp_log.h', 'freertos/FreeRTOS.h', 'freertos/task.h']})

    def test_all_product_power_paths_keep_critical_cutoff_silent(self):
        for board, stem in [('wzh_crawlPanda', 'panda'), ('wzh_tailRedPanda', 'panda'),
                            ('wzh_dinosaur', 'dinosaur')]:
            source = (ROOT/'main/boards'/board/(stem+'_power.cc')).read_text()
            self.assertIn('ObserveSystemPromptBattery(battery_protection_.ready()', source)
            self.assertIn('PlayShutdownPromptAndWait([this]() { ReadBattery(', source)
            emergency = source.split('::EmergencyShutdown() {', 1)[1].split('battery_sampling()', 1)[0]
            self.assertNotIn('PlaySound', emergency)
            self.assertNotIn('PlayShutdownPrompt', emergency)

    def test_every_product_build_enables_the_prompt_pack_and_shutdown_path(self):
        for board, stem in [('wzh_crawlPanda', 'panda'), ('wzh_tailRedPanda', 'panda'),
                            ('wzh_dinosaur', 'dinosaur')]:
            config = json.loads((ROOT/'main/boards'/board/'config.json').read_text())
            builds = config['builds']
            self.assertTrue(builds, board)
            for build in builds:
                self.assertIn('CONFIG_WZH_SYSTEM_PROMPTS=y', build['sdkconfig_append'],
                              f'{board}/{build["name"]} would ship the old prompts')
            source = (ROOT/'main/boards'/board/(stem+'_power.cc')).read_text()
            shutdown = source[source.index('::Shutdown()'):source.index('::EmergencyShutdown()')]
            # Legacy and product voice packs share the same bounded worker;
            # disabling the replacement pack must not restore a 700 ms cutoff.
            self.assertIn('wzh::BeginSystemPromptShutdown();', shutdown)
            self.assertIn('wzh::PlayShutdownPromptAndWait(', shutdown)
            self.assertNotIn('700', shutdown)
            self.assertNotIn('PlaySound(', shutdown)



if __name__ == '__main__':
    unittest.main()
