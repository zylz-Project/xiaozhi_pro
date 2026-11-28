#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace voice_tts {
// The caller provides storage and synchronizes access between modem and TLS tasks.
class VoiceAtBuffer {
public:
    void Init(unsigned char* storage, size_t capacity) {
        storage_ = storage;
        capacity_ = capacity;
        head_ = tail_ = size_ = 0;
    }
    size_t Size() const { return size_; }
    bool Push(const void* data, size_t count) {
        if (!storage_ || !capacity_ || count > capacity_ - size_)
            return false;
        if (!count)
            return true;
        const size_t first = std::min(count, capacity_ - tail_);
        std::memcpy(storage_ + tail_, data, first);
        std::memcpy(storage_, static_cast<const unsigned char*>(data) + first, count - first);
        tail_ = (tail_ + count) % capacity_;
        size_ += count;
        return true;
    }
    size_t Pop(void* output, size_t count) {
        count = std::min(count, size_);
        if (!count)
            return 0;
        const size_t first = std::min(count, capacity_ - head_);
        std::memcpy(output, storage_ + head_, first);
        std::memcpy(static_cast<unsigned char*>(output) + first, storage_, count - first);
        head_ = (head_ + count) % capacity_;
        size_ -= count;
        return count;
    }

private:
    unsigned char* storage_ = nullptr;
    size_t capacity_ = 0, head_ = 0, tail_ = 0, size_ = 0;
};
}  // namespace voice_tts
