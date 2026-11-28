import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class BleRxStreamTest(unittest.TestCase):
    def test_external_storage_capacity_and_fallbacks(self):
        if not shutil.which("g++"):
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "freertos").mkdir()
            (root / "freertos/FreeRTOS.h").write_text(
                "#pragma once\n#define configSUPPORT_STATIC_ALLOCATION 1\n")
            (root / "esp_heap_caps.h").write_text("""#pragma once
#include <cstddef>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void* heap_caps_malloc(size_t, int);
void heap_caps_free(void*);
""")
            (root / "freertos/message_buffer.h").write_text("""#pragma once
#include <cstddef>
#include <cstdint>
struct StaticStreamBuffer_t { int lock; };
using MessageBufferHandle_t = void*;
MessageBufferHandle_t xMessageBufferCreateStatic(size_t, uint8_t*, StaticStreamBuffer_t*);
MessageBufferHandle_t xMessageBufferCreate(size_t);
""")
            (root / "test.cc").write_text("""#include <cassert>
#include <cstdlib>
#include <vector>
#define CONFIG_SPIRAM 1
#include "wzh_ble_rx_stream.h"
static int fail = 0, calls = 0, frees = 0, dynamic_calls = 0;
static std::vector<void*> live;
void* heap_caps_malloc(size_t size, int caps) {
    ++calls;
    assert((calls != 1 || (size == 8193 && caps == 3)) &&
           (calls != 2 || (size == sizeof(StaticStreamBuffer_t) && caps == 6)));
    if (calls == fail || (fail == 4 && calls == 1)) return nullptr;
    void* p = malloc(size); live.push_back(p); return p;
}
void heap_caps_free(void* p) {
    ++frees; for (auto& v : live) if (v == p) v = nullptr; free(p);
}
MessageBufferHandle_t xMessageBufferCreateStatic(size_t size, uint8_t* p, StaticStreamBuffer_t* c) {
    assert(size == 8193 && p && c);
    return fail == 3 ? nullptr : c;
}
MessageBufferHandle_t xMessageBufferCreate(size_t size) {
    assert(size == 8192); ++dynamic_calls;
    return fail == 4 ? nullptr : reinterpret_cast<void*>(1);
}
int main() {
    for (int mode = 0; mode < 5; ++mode) {
        fail = mode; calls = frees = dynamic_calls = 0; live.clear();
        bool external = false;
        auto result = wzh::CreateBleRxBuffer(8192, external);
        assert((result != nullptr) == (mode != 4));
        assert(external == (mode == 0));
        assert(dynamic_calls == (mode != 0));
        assert(frees == (mode == 2 ? 1 : mode == 3 ? 2 : 0));
        for (void* p : live) free(p);
    }
}
""")
            executable = root / "test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root), "-I", str(ROOT / "main/boards/common/wzh"),
                            str(root / "test.cc"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_partial_multi_frame_errors_and_reset_with_reused_storage(self):
        if not shutil.which("g++"):
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "frame.cc").write_text(r'''#include <cassert>
#include <string>
#include <string_view>
#include <vector>
#include "wzh_ble_frame.h"
int main() {
    using Frame = wzh::BleFrame;
    char storage[Frame::kMaxBytes + 1];
    Frame frame(storage);
    std::vector<std::string> frames;
    size_t rejected = 0;
    auto feed = [&](std::string_view bytes) {
        for (auto byte : bytes) {
            auto result = frame.Feed(byte);
            if (result == Frame::Result::kReady) frames.emplace_back(storage);
            if (result == Frame::Result::kRejected) ++rejected;
        }
    };
    feed(R"({"id":1,"data")");
    assert(frames.empty());
    feed(":{}}\r\n{}\n\n");
    assert((frames == std::vector<std::string>{R"({"id":1,"data":{}})", "{}"}));
    feed(std::string(Frame::kMaxBytes + 1, ' ') + "{}\n{}\n");
    assert(rejected == 1 && frames.size() == 3);
    feed(std::string_view("{}\0{}\n", 6));
    feed("[[[[[[[[[0]]]]]]]]]\n");
    assert(rejected == 3 && frames.size() == 3);
    feed(R"({"data":"escaped \" [ [{"})" "\n");
    assert(frames.size() == 4);
    feed("{\"old\":");
    frame.Reset();
    feed("{}\n");
    assert(frames.size() == 5 && frames.back() == "{}");
    feed("{" + std::string(Frame::kMaxBytes - 2, ' ') + "}\n");
    assert(frames.size() == 6 && frames.back().size() == Frame::kMaxBytes);
}
''')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/common/wzh'), str(root / 'frame.cc'),
                            '-o', str(root / 'frame')], check=True)
            subprocess.run([str(root / 'frame')], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
