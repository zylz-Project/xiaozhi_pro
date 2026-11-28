#pragma once

#include <cstddef>
#include <cstdint>

namespace wzh {

// Worker-owned framing state. The caller supplies one reusable buffer so
// incomplete input and bursts never cause allocations proportional to traffic.
class BleFrame {
public:
    static constexpr size_t kMaxBytes = 4096;
    enum class Result { kPending, kReady, kRejected };

    explicit BleFrame(char* storage) : storage_(storage) {}

    void Reset() {
        size_ = 0;
        rejected_ = false;
        in_string_ = false;
        escaped_ = false;
        depth_ = 0;
    }

    Result Feed(uint8_t byte) {
        if (byte != '\n') {
            if (!rejected_) {
                rejected_ = size_ == kMaxBytes || byte == 0 || !TrackNesting(byte);
                if (!rejected_)
                    storage_[size_++] = static_cast<char>(byte);
            }
            return Result::kPending;
        }
        const bool rejected = rejected_ || depth_ != 0 || in_string_;
        if (size_ != 0 && storage_[size_ - 1] == '\r')
            --size_;
        storage_[size_] = '\0';
        const size_t size = size_;
        Reset();
        return rejected ? Result::kRejected : size != 0 ? Result::kReady : Result::kPending;
    }

private:
    bool TrackNesting(uint8_t byte) {
        constexpr int kMaxDepth = 8;
        if (in_string_) {
            if (escaped_)
                escaped_ = false;
            else if (byte == '\\')
                escaped_ = true;
            else if (byte == '"')
                in_string_ = false;
        } else if (byte == '"') {
            in_string_ = true;
        } else if (byte == '{' || byte == '[') {
            return ++depth_ <= kMaxDepth;
        } else if (byte == '}' || byte == ']') {
            return --depth_ >= 0;
        }
        return true;
    }

    char* storage_;
    size_t size_ = 0;
    int depth_ = 0;
    bool rejected_ = false;
    bool in_string_ = false;
    bool escaped_ = false;
};

}  // namespace wzh
