"""Exercise actual sentence admission limits and preservation on overflow."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class VoiceBacklogTest(unittest.TestCase):
    def test_overflow_keeps_existing_sentences(self):
        if not shutil.which('g++'):
            self.skipTest('C++ compiler required')
        source=(ROOT/'main/boards/common/wzh/voice_tts/voice_session.cc').read_text()
        method=queue_function(source,'bool VoiceSession::Enqueue(')
        code=r'''
#include <cassert>
#include <deque>
#include <mutex>
#include <string>
#include "voice_text_filter.h"
#include "voice_text_batch.h"
using voice_tts::IsToolStatus;
using voice_tts::IsMusicStart;
using voice_tts::IsPostMusicAnnouncement;
using voice_tts::PopTextBatch;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
void xTaskNotifyGive(void*){}
constexpr size_t kMaxPendingSentences=64,kMaxPendingBytes=16*1024;
struct VoiceSession {
 std::mutex mutex_;std::deque<std::string> queue_;void* task_=this;
 bool finished_=false,failed_=false,overflow_=false,had_text_=false;size_t queued_bytes_=0;
 bool Enqueue(const std::string&);
};
''' + method + r'''
int main(){
 std::deque<std::string> pending={"Hello,", "friend!",std::string(230,'x')};
 size_t pending_bytes=6+7+230;
 assert(PopTextBatch(pending,pending_bytes,true)=="Hello, friend!");
 assert(pending.size()==1 && pending_bytes==230);
 assert(PopTextBatch(pending,pending_bytes,true)==std::string(230,'x'));
 assert(pending.empty() && pending_bytes==0);
 pending={"one","two"};pending_bytes=6;
 assert(PopTextBatch(pending,pending_bytes,false)=="one" && pending_bytes==3);
 assert(IsToolStatus("% search_music..."));
 assert(IsToolStatus(" \t% self.panda.react_to_user_emotion...\n"));
 assert(IsToolStatus("% play_music..."));
 assert(IsMusicStart("% play_music..."));
 assert(!IsMusicStart("% search_music..."));
 assert(!IsMusicStart("% play_music... ordinary text"));
 assert(IsPostMusicAnnouncement("好嘞，给你放了《星宵的河》，希望你喜欢。"));
 assert(IsPostMusicAnnouncement("《远方》已经播放完啦，这首歌是不是很有激情？"));
 assert(IsPostMusicAnnouncement("《远方》放完啦，这首歌是不是很有力量？"));
 assert(IsPostMusicAnnouncement("《奔向自由》已经放完啦，这首歌是不是很有力量？"));
 assert(!IsPostMusicAnnouncement("《远方》让我想起了你"));
 assert(!IsPostMusicAnnouncement("它说冰封的旷野之下"));
 assert(!IsToolStatus("电量剩余 50%"));
 assert(!IsToolStatus("% search_music... 是一个工具提示"));
 assert(!IsToolStatus("% 折扣"));
 assert(!IsToolStatus("% search_music"));
 VoiceSession s;
 assert(s.Enqueue("% search_music..."));
 assert(s.Enqueue("% self.panda.react_to_user_emotion..."));
 assert(s.had_text_ && s.queue_.empty());
 for(int i=0;i<64;++i)assert(s.Enqueue(std::to_string(i)));
 auto bytes=s.queued_bytes_;
 assert(!s.Enqueue("overflow"));assert(s.overflow_ && !s.failed_);
 assert(s.queue_.size()==64 && s.queued_bytes_==bytes);
 for(int i=0;i<64;++i){assert(s.queue_.front()==std::to_string(i));s.queue_.pop_front();}
 VoiceSession large;
 for(int i=0;i<32;++i)assert(large.Enqueue(std::string(512,'a')));
 assert(!large.Enqueue("a") && large.queue_.size()==32 && large.queued_bytes_==16*1024);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);(path/'test.cc').write_text(code)
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'main/boards/common/wzh/voice_tts'),
                str(path/'test.cc'),'-o',str(path/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(path/'test')],check=True,timeout=10)
