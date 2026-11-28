import pathlib
import subprocess
import tempfile
import unittest


class VoiceAtBufferTest(unittest.TestCase):
    def test_wrap_overflow_and_drain(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        source = r'''
#include "main/boards/common/wzh/voice_tts/voice_at_buffer.h"
#include <cassert>
#include <cstring>
int main() {
    voice_tts::VoiceAtBuffer queue;
    unsigned char storage[8];
    char out[16] = {};
    assert(!queue.Push("x", 1));
    queue.Init(storage, sizeof(storage));
    assert(queue.Push("abcdef", 6));
    assert(queue.Pop(out, 4) == 4 && !memcmp(out, "abcd", 4));
    assert(queue.Push("GHIJKL", 6));
    assert(!queue.Push("!", 1));
    assert(queue.Size() == 8);
    assert(queue.Pop(out, sizeof(out)) == 8 && !memcmp(out, "efGHIJKL", 8));
    assert(queue.Size() == 0 && queue.Pop(out, 1) == 0);
    assert(queue.Push("z", 1));
    assert(queue.Pop(out, 1) == 1 && out[0] == 'z');
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.cc").write_text(source)
            subprocess.run(["c++", "-std=c++17", "-I", str(root), str(path / "test.cc"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
