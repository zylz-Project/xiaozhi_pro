"""Exercise offline-player startup allocation and failure cleanup."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class OfflineAudioMemoryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_psram_preferred_fallback_and_failure_cleanup(self):
        source = (ROOT / 'main/boards/common/wzh/wzh_audio_player.cc').read_text()
        start = queue_function(source, 'bool AudioPlayer::Start(')
        program = r'''
#include <cassert>
#include <cstdlib>
#include <new>
#include <cstddef>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_INTERNAL=4, pdPASS=1;
int heap_mode=0, queue_deletes=0, frees=0, task_status=pdPASS, stack_bytes=0;
int order[2], calls=0;
void* heap_caps_malloc(size_t n, int caps) {
 order[calls++]=caps;
 if (heap_mode==2 || (heap_mode==1 && (caps & MALLOC_CAP_SPIRAM))) return nullptr;
 return std::malloc(n);
}
void heap_caps_free(void* p) { ++frees; std::free(p); }
void* xQueueCreate(int, size_t) { return std::malloc(1); }
void vQueueDelete(void* q) { ++queue_deletes; std::free(q); }
int xTaskCreate(void(*)(void*), const char*, int size, void*, int, void*) {
 stack_bytes=size; return task_status;
}
struct OggDemuxer { char buffer[8192]; };
struct AudioStore {};
struct AudioPlayer {
 struct PlayRequest { int n; };
 AudioStore* store_=nullptr; void* play_queue_=nullptr; OggDemuxer* demuxer_=nullptr;
 static void TaskEntry(void*) {}
 bool Start(AudioStore* store);
};
''' + start + r'''
int main() {
 AudioStore store;
 { AudioPlayer p; assert(p.Start(&store)); assert(calls==1 && (order[0]&MALLOC_CAP_SPIRAM));
   assert(stack_bytes==12288 && !p.Start(&store));
   p.demuxer_->~OggDemuxer(); heap_caps_free(p.demuxer_); vQueueDelete(p.play_queue_); }
 calls=0; heap_mode=1;
 { AudioPlayer p; assert(p.Start(&store)); assert(calls==2 && (order[1]&MALLOC_CAP_INTERNAL));
   p.demuxer_->~OggDemuxer(); heap_caps_free(p.demuxer_); vQueueDelete(p.play_queue_); }
 calls=0; heap_mode=2;
 { AudioPlayer p; int before=queue_deletes; assert(!p.Start(&store));
   assert(p.play_queue_==nullptr && p.demuxer_==nullptr && queue_deletes==before+1); }
 calls=0; heap_mode=0; task_status=0;
 { AudioPlayer p; int before=queue_deletes, old=frees; assert(!p.Start(&store));
   assert(!p.play_queue_ && !p.demuxer_ && queue_deletes==before+1 && frees==old+1); }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'test.cc').write_text(program)
            subprocess.run(['g++', '-std=c++17', str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
