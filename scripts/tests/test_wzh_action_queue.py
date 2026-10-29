"""Execute production queue admission with a single-slot task queue substitute."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def queue_function(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class WzhActionQueueTest(unittest.TestCase):
    def check_product(self, board, filename, controller):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ is required")
        source = (ROOT / "main/boards" / board / filename).read_text()
        method = queue_function(source, "bool " + controller + "::QueueRequest(")
        method = method.replace(controller + "::", "Controller::")
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include "wzh_pet_behavior.h"
enum class Action { kListen, kHappy };
struct Request {
    Action action;
    bool with_local_sound = false, is_body = false;
    int test_gpio = -1;
    uint32_t created_ms = 0, generation = 0;
    wzh::PetPriority priority = wzh::PetPriority::kExplicit;
    bool speech_bound = false;
};
static int writes = 0;
static Request queued{Action::kListen};
static int64_t fake_now = 10000000;
constexpr int pdTRUE = 1;
int64_t esp_timer_get_time() { return fake_now; }
int xQueueOverwrite(void*, const Request* request) { queued = *request; ++writes; return pdTRUE; }
struct Output { bool locked = false; bool blocked() const { return locked; } };
struct Controller {
    using ActionRequest = Request;
    std::mutex request_mutex_;
    void* action_queue_ = this;
    Output servo_output_;
    std::atomic<bool> shutdown_requested_{false}, request_active_{false};
    std::atomic<wzh::PetPriority> running_priority_{wzh::PetPriority::kSensor};
    wzh::PetBehavior pet_;
    Action last_action_ = Action::kListen;
    int64_t last_action_time_us_ = 0;
    uint32_t last_action_generation_ = 0;
    std::atomic<uint32_t> intent_generation_{0};
    std::atomic<int64_t> last_request_us_{0};
    bool QueueRequest(ActionRequest, wzh::PetPriority);
};
'''
        program += method
        program += r'''
int main() {
    Controller controller;
    const auto explicit_intent = wzh::PetPriority::kExplicit;
    assert(controller.QueueRequest(Request{Action::kHappy}, explicit_intent));
    assert(writes == 1 && queued.created_ms == 10000);
    fake_now += 10000;
    assert(controller.QueueRequest(Request{Action::kHappy}, explicit_intent));
    assert(writes == 1); // A still-current emotion can coalesce.
    assert(controller.QueueRequest(Request{Action::kHappy, false, true}, explicit_intent));
    assert(writes == 2);
    fake_now += 10000;
    assert(controller.QueueRequest(Request{Action::kHappy}, explicit_intent));
    assert(writes == 3); // A body command did not masquerade as this emotion.
    controller.intent_generation_.fetch_add(1); // A stop/maintenance event invalidated the old intent.
    assert(controller.QueueRequest(Request{Action::kHappy}, explicit_intent));
    assert(writes == 4); // An invalidated request cannot suppress the replacement.
    controller.request_active_.store(true);
    controller.running_priority_.store(explicit_intent);
    assert(!controller.QueueRequest(Request{Action::kListen}, wzh::PetPriority::kSensor));
    assert(writes == 4);
    controller.request_active_.store(false);
    fake_now = 14000000;
    assert(controller.QueueRequest(Request{Action::kListen}, explicit_intent));
    assert(writes == 5);
'''
        if board == "wzh_dinosaur":
            program += r'''
    fake_now += 10000;
    assert(controller.QueueRequest(Request{Action::kListen, false, false, 17}, explicit_intent));
    assert(writes == 6 && queued.test_gpio == 17); // Assembly diagnostics are never emotion duplicates.
'''
        program += r'''
    fake_now = 20000000;
    controller.request_active_.store(true);
    controller.running_priority_.store(wzh::PetPriority::kEmotion);
    assert(!controller.QueueRequest(Request{Action::kHappy}, wzh::PetPriority::kEmotion));
    assert(!controller.QueueRequest(Request{Action::kListen}, wzh::PetPriority::kSensor));
    assert(controller.QueueRequest(Request{Action::kHappy, false, true}, explicit_intent));
    controller.request_active_.store(false);
    const int previous_writes = writes;
    controller.servo_output_.locked = true;
    assert(!controller.QueueRequest(Request{Action::kHappy}, explicit_intent));
    controller.servo_output_.locked = false;
    controller.pet_.Control(wzh::PetControl::kRest, fake_now / 1000);
    assert(!controller.QueueRequest(Request{Action::kHappy}, explicit_intent));
    assert(writes == previous_writes);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            cpp = directory / "queue.cc"
            cpp.write_text(program)
            executable = directory / "queue"
            result = subprocess.run(
                [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main/boards/common/wzh"), str(cpp), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(executable)], check=True, capture_output=True)

    def test_dinosaur_latest_intent_and_diagnostics(self):
        self.check_product("wzh_dinosaur", "dinosaur_controller.cc", "DinosaurController")

    def test_crawl_panda_latest_intent(self):
        self.check_product("wzh_crawlPanda", "panda_controller.cc", "PandaController")

    def test_tail_red_panda_latest_intent(self):
        self.check_product("wzh_tailRedPanda", "panda_controller.cc", "PandaController")


if __name__ == "__main__":
    unittest.main()
