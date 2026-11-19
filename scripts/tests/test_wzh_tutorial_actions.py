"""Check tutorial feedback against each product's spoken instructions."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class TutorialActionsTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_dinosaur_touch_moves_head_and_tail_then_returns(self):
        source = (ROOT / 'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
        method = queue_function(source, 'void DinosaurController::RunTutorialTouch(').replace(
            'DinosaurController::', 'Controller::')
        program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
enum class MotionEase {kSmooth};
struct Keyframe {int tilt,lean,head,tail_ud,tail_lr;uint16_t duration;MotionEase ease;};
struct MotionClip {const char* name;const Keyframe* frames;uint8_t count;};
struct Center {void Reset(){}};
struct Controller {
 struct Pose{};Pose current_pose_;Center quiet_center_;bool head=false,tail=false,returned=false;
 void PlayClip(const MotionClip& c,Pose,bool,float,float,int){
  for(unsigned i=0;i<c.count;++i){const auto& f=c.frames[i];
   assert(f.tilt>=0&&f.tilt<=180&&f.lean>=0&&f.lean<=180&&f.head>=0&&f.head<=180);
   assert(f.tail_ud>=55&&f.tail_ud<=180&&f.tail_lr>=0&&f.tail_lr<=180&&f.duration>=420);
   head|=f.head!=90;tail|=f.tail_lr!=90;}
  const auto& last=c.frames[c.count-1];returned=last.tilt==70&&last.lean==90&&last.head==90&&last.tail_ud==90&&last.tail_lr==90;
 }
 void PrepareIdleCenter(){}void RunTutorialTouch();
};
''' + method + r'''
int main(){Controller c;c.RunTutorialTouch();assert(c.head&&c.tail&&c.returned);}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cc').write_text(program)
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(path / 'test.cc'), '-o', str(path / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True, timeout=5)

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_teaching_uses_the_promised_body_actions(self):
        cases = [
            ('wzh_crawlPanda', 'panda', 'PandaController', 'Panda', 'kWaveLeft', 0),
            ('wzh_tailRedPanda', 'panda', 'PandaController', 'Panda', 'kWagTail', 0),
            ('wzh_dinosaur', 'dinosaur', 'DinosaurController', 'Dino', 'kLookLeft', 1),
        ]
        for board, stem, cls, prefix, expected, touch_step in cases:
            with self.subTest(board=board), tempfile.TemporaryDirectory() as directory:
                source = (ROOT / 'main/boards' / board / (stem + '_controller.cc')).read_text()
                methods = '\n'.join(queue_function(source, marker).replace(cls + '::', 'Controller::')
                                    for marker in ('bool ' + cls + '::TriggerOnboardingFeedback(',
                                                   'bool ' + cls + '::QueueInteraction('))
                program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include "wzh_pet_behavior.h"
#define ESP_LOGD(...) ((void)0)
#define EXPECTED Body::@EXPECTED@
using Action=int;
using PandaAction=Action;using DinoAction=Action;
enum class Body {kShakeHead,kLookLeft,kLookRight,kWaveLeft,kWaveRight,kRaiseArms,kWagTail};
using PandaBodyAction=Body;using DinoBodyAction=Body;
int64_t esp_timer_get_time(){return 1000000;}
struct Output {unsigned faults(){return 0;}};
struct Request {Action action;bool with_local_sound,is_body;wzh::PetInteraction interaction=wzh::PetInteraction::kNone;uint8_t tutorial_step=0;};
struct Controller {
 using ActionRequest=Request;Request last{};Output servo_output_;
 std::atomic<bool> tutorial_feedback_pending_{false};
 std::atomic<uint32_t> tutorial_feedback_requested_ms_{0};
 bool QueueRequest(ActionRequest r,wzh::PetPriority){last=r;return true;}
 wzh::PetMode pet_mode(){return wzh::PetMode::kNormal;}
 bool QueueInteraction(wzh::PetInteraction,uint8_t=0);
 bool TriggerOnboardingFeedback(uint8_t);
};
''' + methods + r'''
int main(){Controller c;
 assert(c.TriggerOnboardingFeedback(1));assert(c.last.tutorial_step==@TOUCH@);
 assert(c.TriggerOnboardingFeedback(2));assert(c.last.tutorial_step==2);
 assert(c.last.action==static_cast<Action>(EXPECTED));assert(c.last.interaction==wzh::PetInteraction::kShaken);
 assert(c.TriggerOnboardingFeedback(3));assert(c.last.tutorial_step==3);
 assert(c.last.action==static_cast<Action>(Body::kShakeHead));
 assert(c.TriggerOnboardingFeedback(5));assert(c.last.tutorial_step==0);
}
'''
                program = program.replace('@EXPECTED@', expected).replace('@TOUCH@', str(touch_step))
                path = Path(directory)
                (path / 'test.cc').write_text(program)
                result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         '-I', str(ROOT / 'main/boards/common/wzh'),
                                         str(path / 'test.cc'), '-o', str(path / 'test')],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(path / 'test')], check=True, timeout=5)
