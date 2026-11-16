"""The AFE must honor the selected VAD when old assets contain extra models."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AfeVadConfigurationTest(unittest.TestCase):
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
