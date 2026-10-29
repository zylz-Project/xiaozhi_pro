#ifndef WZH_TOUCH_INPUT_H_
#define WZH_TOUCH_INPUT_H_

#include <algorithm>
#include <cstdint>

namespace wzh {

// testBoard_wifiweb 使用的四路触摸 R-2R 电阻网络解码与稳定判定。
// code 的 bit0..bit3 分别表示 SW1..SW4，0 表示全部释放，-1 表示电压无效。
class TouchInputFilter {
public:
    struct Event {
        int code;
        int previous_code;
        int voltage_mv;
    };

    explicit TouchInputFilter(int stable_samples) : stable_samples_(stable_samples) {}

    static int Median3(int first, int second, int third) {
        if (first > second) {
            std::swap(first, second);
        }
        if (second > third) {
            std::swap(second, third);
        }
        if (first > second) {
            std::swap(first, second);
        }
        return second;
    }

    static int DecodeMillivolts(int voltage_mv) {
        // 阈值与 testBoard_wifiweb 保持一致。边界按表顺序归入前一档。
        static constexpr Range kRanges[] = {
            {0, 130, 0x00},     {130, 290, 0x01},   {290, 480, 0x02},   {480, 670, 0x03},
            {670, 870, 0x04},   {870, 1070, 0x05},  {1070, 1280, 0x06}, {1280, 1490, 0x07},
            {1490, 1690, 0x08}, {1690, 1890, 0x09}, {1890, 2100, 0x0A}, {2100, 2310, 0x0B},
            {2310, 2510, 0x0C}, {2510, 2710, 0x0D}, {2710, 2910, 0x0E}, {2910, 3150, 0x0F},
        };
        for (const auto& range : kRanges) {
            if (voltage_mv >= range.min_mv && voltage_mv <= range.max_mv) {
                return range.code;
            }
        }
        return -1;
    }

    // 仅在某个编码连续 stable_samples 次出现且与上次发布值不同时返回 true。
    bool Update(int voltage_mv, Event& event) {
        const int code = DecodeMillivolts(voltage_mv);
        if (code != candidate_code_) {
            candidate_code_ = code;
            candidate_count_ = 1;
        } else if (candidate_count_ < stable_samples_) {
            ++candidate_count_;
        }

        if (candidate_count_ < stable_samples_ || code == published_code_) {
            return false;
        }
        event = {code, published_code_, voltage_mv};
        published_code_ = code;
        return true;
    }

    // A nonzero startup/fault-recovery level is not a new touch. Require a
    // verified release before forwarding presses; invalid input cancels contact.
    bool AcceptInteraction(const Event& event) {
        if (event.code < 0) {
            armed_ = false;
            return false;
        }
        if (event.code == 0)
            armed_ = true;
        return armed_;
    }

private:
    struct Range {
        int min_mv;
        int max_mv;
        int code;
    };

    static constexpr int kUninitialized = -2;
    const int stable_samples_;
    int candidate_code_ = kUninitialized;
    int candidate_count_ = 0;
    int published_code_ = kUninitialized;
    bool armed_ = false;
};

}  // namespace wzh

#endif  // WZH_TOUCH_INPUT_H_
