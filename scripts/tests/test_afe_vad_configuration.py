"""The AFE must honor the selected VAD when old assets contain extra models."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AfeVadConfigurationTest(unittest.TestCase):
    def test_failed_webrtc_initialization_retries_without_noise_suppression(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        start = source.index('    afe_iface_ = esp_afe_handle_from_config(afe_config);')
        end = source.index('    afe_config_free(afe_config);', start)
        selection = source[start:end]
        program = r'''
#include <cassert>
#define CONFIG_SR_NSN_WEBRTC 1
#define CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr bool kUseProductMultiNet = true;
struct Config { bool ns_init=true; };
struct Interface {
 int calls=0; bool fail_all=false; bool last_ns=true;
 void* create_from_config(Config* config) {
  ++calls; last_ns=config->ns_init;
  return (!fail_all && !config->ns_init) ? this : nullptr;
 }
} instance;
Interface* esp_afe_handle_from_config(Config*) {return &instance;}
int main() {
 Config config; Config* afe_config=&config;
 Interface* afe_iface_=nullptr; void* afe_data_=nullptr;
''' + selection + r'''
 assert(afe_data_ && instance.calls==2 && !instance.last_ns);
 instance.calls=0; instance.fail_all=true; config.ns_init=true; afe_data_=nullptr;
''' + selection + r'''
 assert(!afe_data_ && instance.calls==2 && !instance.last_ns);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'retry.cc'; cpp.write_text(program)
            exe = Path(directory) / 'retry'
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(cpp), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_webrtc_noise_suppression_is_explicit_and_ignores_old_assets(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        start = source.index('    afe_config->ns_init = false;')
        end = source.index('    afe_config->vad_init =', start)
        selection = source[start:end]
        program = r'''
#include <cassert>
enum { AFE_NS_MODE_WEBRTC=7, AFE_NS_MODE_NET=9 };
struct Config { bool ns_init; char* ns_model_name; int afe_ns_mode; };
int main() {
 char old_model[] = "nsnet2";
 Config config{true, old_model, AFE_NS_MODE_NET};
 auto* afe_config = &config;
 constexpr bool kUseProductMultiNet=PRODUCT, kUseAfeForVoiceProcessing=PROCESSING;
 (void)kUseProductMultiNet; (void)kUseAfeForVoiceProcessing;
''' + selection + r'''
#if CONFIG_SR_NSN_WEBRTC && !CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND
 if (PRODUCT && PROCESSING) {
  assert(config.ns_init && config.ns_model_name == nullptr && config.afe_ns_mode == AFE_NS_MODE_WEBRTC);
 } else
#endif
 assert(!config.ns_init);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'test.cc'; cpp.write_text(program)
            for product, processing, webrtc, neural in ((1, 1, 1, 0), (1, 0, 1, 0),
                                                        (0, 1, 1, 0), (1, 1, 0, 0),
                                                        (1, 1, 1, 1)):
                exe = Path(directory) / f'test-{product}-{processing}-{webrtc}-{neural}'
                result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         f'-DPRODUCT={product}', f'-DPROCESSING={processing}',
                                         f'-DCONFIG_SR_NSN_WEBRTC={webrtc}',
                                         f'-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND={neural}',
                                         str(cpp), '-o', str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(exe)], check=True, timeout=5)

    def test_old_vadnet_asset_cannot_override_webrtc_configuration(self):
        if not shutil.which('g++'):
            self.skipTest('g++ required')
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        start = source.index('#if CONFIG_SR_VADN_WEBRTC', source.index('bool AfeAudioEngine::Initialize('))
        end = source.index('#if CONFIG_VOICE_BARGE_IN_VADNET_EXPERIMENT', start)
        selection = source[start:end]
        assignment_start = source.index('    afe_config->vad_model_name = vad_model_name;', end)
        assignment_end = source.index('    afe_config->wakenet_init', assignment_start)
        assignment = source[assignment_start:assignment_end]
        program = r'''
#include <cassert>
#define ESP_LOGI(...) ((void)0)
#define ESP_VADN_PREFIX "vadnet"
struct Models {char* available;};
char* esp_srmodel_filter(Models* models, const char*, const char*) {return models->available;}
struct Config {char* vad_model_name;};
char* run(Models* models_) {
 (void)models_;
 Config config{reinterpret_cast<char*>(1)};
 Config* afe_config = &config;
''' + selection + assignment + r'''
 return config.vad_model_name;
}
int main() {
 char old_vadnet[] = "vadnet1_medium";
 Models old_assets{old_vadnet};
 Models no_model{nullptr};
#if CONFIG_SR_VADN_WEBRTC || CONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND
 assert(run(&old_assets)==nullptr);
#else
 assert(run(&old_assets)==old_vadnet);
#endif
 assert(run(&no_model)==nullptr);
 assert(run(nullptr)==nullptr);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'test.cc'
            cpp.write_text(program)
            for webrtc, neural in ((1, 0), (0, 0), (0, 1)):
                exe = Path(directory) / f'test-{webrtc}-{neural}'
                result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         f'-DCONFIG_SR_VADN_WEBRTC={webrtc}',
                                         f'-DCONFIG_VOICE_BARGE_IN_NEURAL_FRONTEND={neural}',
                                         str(cpp), '-o', str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
