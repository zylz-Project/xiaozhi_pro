#ifndef WZH_CONVERSATION_TOUCH_H_
#define WZH_CONVERSATION_TOUCH_H_

#include <array>
#include <string>
#include <string_view>

#include "wzh_pet_behavior.h"

namespace wzh {

inline std::string ConversationTouchText(const TouchEvent& event,
                                         const std::array<std::string_view, 4>& positions) {
    if (event.response == TouchResponse::kStrokeForward)
        return "设备感应到用户从靠近脖子的位置向尾巴方向顺摸了你的背。请自然回应。";
    if (event.response == TouchResponse::kStrokeReverse)
        return "设备感应到用户从靠近尾巴的位置向脖子方向逆摸了你的背。请自然回应。";
    if ((event.response != TouchResponse::kAffection &&
         event.response != TouchResponse::kPlayful) ||
        !(event.mask & 0x0f) || (event.mask & 0xf0)) {
        return {};
    }
    std::string text = "设备感应到用户摸了你的";
    bool first = true;
    for (size_t index = 0; index < positions.size(); ++index) {
        if (!(event.mask & (1U << index)))
            continue;
        if (!first)
            text += "、";
        text += positions[index];
        first = false;
    }
    text += "。请自然回应。";
    return text;
}

}  // namespace wzh
#endif
