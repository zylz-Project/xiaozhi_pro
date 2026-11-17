"""Exercise the experimental offline worker's real allocation/fallback path."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class OfflinePsramWorkerTest(unittest.TestCase):
    def test_dma_and_stack_allocation_fallback(self):
        if not shutil.which("g++"):
            self.skipTest("C++ compiler required")
        source = (ROOT / "main/boards/common/wzh/wzh_audio_player.cc").read_text()
        start = queue_function(source, "bool AudioPlayer::Start(")
        code = r'''
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <new>
#define CONFIG_WZH_OFFLINE_AUDIO_PSRAM_EXPERIMENT 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define pdPASS 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr size_t kReadChunk=4096;
bool dma_fail=false,external_fail=false,internal_fail=false,queue_fail=false;
int dma_frees=0,external_creates=0,internal_creates=0;
void* dma_ptr=nullptr;
void* heap_caps_malloc(size_t size,int caps){
    if(caps&MALLOC_CAP_DMA){
        assert((caps&(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        if(dma_fail)return nullptr;
        dma_ptr=std::malloc(size);return dma_ptr;
    }
    return std::malloc(size);
}
void heap_caps_free(void* p){if(p==dma_ptr){++dma_frees;dma_ptr=nullptr;}std::free(p);}
struct OggDemuxer {};
void* xQueueCreate(int,int){return queue_fail?nullptr:reinterpret_cast<void*>(1);}
void vQueueDelete(void*){}
int xTaskCreateWithCaps(void(*)(void*),const char*,int size,void*,int,void*,int caps){
    assert(size==14336 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    ++external_creates;return external_fail?0:pdPASS;
}
int xTaskCreate(void(*)(void*),const char*,int size,void*,int,void*){
    assert(size==14336);++internal_creates;return internal_fail?0:pdPASS;
}
struct AudioStore {};
struct AudioPlayer {
    struct PlayRequest{};AudioStore* store_=nullptr;OggDemuxer* demuxer_=nullptr;
    void* play_queue_=nullptr;uint8_t* read_buffer_=nullptr;
    static void TaskEntry(void*){};bool Start(AudioStore*);
};
''' + start + r'''
void cleanup(AudioPlayer& p){
    if(p.read_buffer_)heap_caps_free(p.read_buffer_);
    if(p.demuxer_){p.demuxer_->~OggDemuxer();heap_caps_free(p.demuxer_);}
}
int main(){AudioStore store;
    {AudioPlayer p;assert(p.Start(&store));assert(external_creates==1&&internal_creates==0&&p.read_buffer_);cleanup(p);}
    {AudioPlayer p;dma_fail=true;assert(p.Start(&store));assert(internal_creates==1&&!p.read_buffer_);cleanup(p);dma_fail=false;}
    {AudioPlayer p;external_fail=true;assert(p.Start(&store));assert(internal_creates==2&&!p.read_buffer_);cleanup(p);}
    {AudioPlayer p;internal_fail=true;assert(!p.Start(&store));assert(!p.demuxer_&&!p.read_buffer_&&!p.play_queue_);}
    assert(dma_frees==3);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cc"
            exe = Path(directory) / "test"
            cpp.write_text(code)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
