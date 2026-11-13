"""Exercise parser allocation and cleanup in the production audio startup."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]

class AudioParserMemoryTest(unittest.TestCase):
    def test_start_fallback_and_failure_cleanup(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source = (ROOT/'main/boards/common/wzh/wzh_audio_player.cc').read_text()
        start = queue_function(source, 'bool AudioPlayer::Start(')
        code = r'''
#include <cassert>
#include <cstdlib>
#include <new>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define ESP_LOGE(...) ((void)0)
int allocs=0,frees=0,constructed=0,destroyed=0,queues=0,deleted=0;
bool psram_fail=false, all_fail=false, queue_fail=false, task_fail=false;
void* heap_caps_malloc(size_t n,int caps){++allocs;if(all_fail || ((caps&1)&&psram_fail))return nullptr;return malloc(n);}
void heap_caps_free(void* p){++frees;free(p);}
struct OggDemuxer {OggDemuxer(){++constructed;}~OggDemuxer(){++destroyed;}};
void* xQueueCreate(int,int){++queues;return queue_fail?nullptr:reinterpret_cast<void*>(1);}
void vQueueDelete(void*){++deleted;}
int xTaskCreate(void (*)(void*),const char*,int,void*,int,void*){return task_fail?0:pdPASS;}
struct AudioStore {};
struct AudioPlayer {struct PlayRequest{};AudioStore* store_=nullptr;OggDemuxer* demuxer_=nullptr;
 void* play_queue_=nullptr;static void TaskEntry(void*){};bool Start(AudioStore*);};
''' + start + r'''
int main(){AudioStore store;
 {AudioPlayer p;all_fail=true;assert(!p.Start(&store));assert(!p.demuxer_&&!p.play_queue_);all_fail=false;}
 {AudioPlayer p;queue_fail=true;assert(!p.Start(&store));assert(!p.demuxer_&&!p.play_queue_);queue_fail=false;}
 {AudioPlayer p;task_fail=true;assert(!p.Start(&store));assert(!p.demuxer_&&!p.play_queue_);task_fail=false;}
 assert(constructed==2&&destroyed==2&&deleted==1);
 {AudioPlayer p;psram_fail=true;assert(p.Start(&store));assert(!p.Start(&store));
  p.demuxer_->~OggDemuxer();heap_caps_free(p.demuxer_);vQueueDelete(p.play_queue_);}
 assert(constructed==3&&destroyed==3&&frees==3);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src=Path(temp)/'test.cc';exe=Path(temp)/'test';src.write_text(code)
            subprocess.run(['g++','-std=c++17',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
