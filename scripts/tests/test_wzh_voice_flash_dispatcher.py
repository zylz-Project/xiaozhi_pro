"""Verify dispatcher initialization failure is reported and can be retried."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VoiceFlashDispatcherTest(unittest.TestCase):
    def test_initialization_failure_then_success(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'sdkconfig.h').write_text('#define CONFIG_WZH_VOICE_FLASH_DISPATCH_EXPERIMENT 1\n')
            (p / 'esp_log.h').write_text('#define ESP_LOGE(...) ((void)0)\n')
            (p / 'esp_flash_dispatcher.h').write_text('''#pragma once
using esp_err_t=int;constexpr int ESP_OK=0,tskNO_AFFINITY=-1;
struct esp_flash_dispatcher_config_t {unsigned task_stack_size,task_priority;int task_core_id;unsigned queue_size;};
esp_err_t esp_flash_dispatcher_init(const esp_flash_dispatcher_config_t*);
const char* esp_err_to_name(esp_err_t);
''')
            (p / 'test.cc').write_text('''#include <cassert>
#include "voice_flash_dispatcher.h"
#include "esp_flash_dispatcher.h"
static int calls=0;
esp_err_t esp_flash_dispatcher_init(const esp_flash_dispatcher_config_t* c){
 assert(c->task_stack_size==3072);return ++calls==1 ? 1 : ESP_OK;
}
const char* esp_err_to_name(esp_err_t){return "error";}
int main(){assert(!voice_tts::FlashDispatcherReady());assert(!voice_tts::InitializeFlashDispatcher());
 assert(!voice_tts::FlashDispatcherReady());assert(voice_tts::InitializeFlashDispatcher());
 assert(voice_tts::FlashDispatcherReady());assert(voice_tts::InitializeFlashDispatcher());assert(calls==2);}
''')
            provider = ROOT / 'main/boards/common/wzh/voice_tts'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(p), '-I', str(provider),
                            str(provider / 'voice_flash_dispatcher.cc'), str(p / 'test.cc'),
                            '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
