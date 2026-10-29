#include "panda_audio.h"

#include <cstring>

#include <esp_log.h>
#include <esp_random.h>

#include "panda_controller.h"

#define TAG "PandaAudio"

namespace {
bool Contains(const char* text, const char* token) {
    return text != nullptr && std::strstr(text, token) != nullptr;
}
}  // namespace

bool PandaAudio::Initialize() {
    if (!store_.Init()) {
        ESP_LOGW(TAG, "外部音频 Flash 暂不可用；预置调校音画配套仍可使用");
    }
    if (!wzh::AudioPlayer::Start(&store_)) {
        return false;
    }
    ESP_LOGI(TAG, "已加载 %d 个离线声音", store_.file_count());
    return true;
}

const char* PandaAudio::SoundName(int index) const {
    const auto* file = store_.PeekFile(index);
    return file != nullptr ? file->name : "???";
}

int PandaAudio::SoundDurationMs(int index) const {
    const auto* file = store_.PeekFile(index);
    return file != nullptr ? static_cast<int>(file->duration_ms) : 0;
}

const char* PandaAudio::SoundCategory(int index) const {
    const auto* file = store_.PeekFile(index);
    return file != nullptr ? file->category : "???";
}

int PandaAudio::SelectFile(PandaAction action) const {
    // 在目录快照上做蓄水池采样，保证同步任务改写目录时选择逻辑安全。
    const auto snapshot = store_.Snapshot();
    int selected = -1;
    int matches = 0;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& file = snapshot[i];
        if (std::strcmp(file.category, "ambient") == 0 && action != PandaAction::kListen) {
            continue;
        }
        bool match = false;
        switch (action) {
            case PandaAction::kEat:
                match = Contains(file.name, "吃竹") || Contains(file.name, "咀嚼") ||
                        Contains(file.name, "eat") || Contains(file.name, "chew");
                break;
            case PandaAction::kListen:
                match = Contains(file.name, "虫鸣") || Contains(file.name, "鸟叫") ||
                        Contains(file.name, "下雨") || Contains(file.name, "流水");
                break;
            case PandaAction::kSleepy:
                match = Contains(file.name, "入睡") || Contains(file.name, "sleep") ||
                        Contains(file.name, "困") || Contains(file.name, "睡觉");
                break;
            case PandaAction::kStartled:
                match = Contains(file.name, "喷嚏") || Contains(file.name, "startle");
                break;
            case PandaAction::kHappy:
                match = Contains(file.name, "嘤嘤") || Contains(file.name, "宝宝") ||
                        Contains(file.name, "happy") || Contains(file.name, "joy");
                break;
            case PandaAction::kAffection:
                match = Contains(file.name, "撒娇") || Contains(file.name, "亲近") ||
                        Contains(file.name, "nuzzle") || Contains(file.name, "comfort");
                break;
            case PandaAction::kDiscover:
                match = file.duration_ms > 0 && file.duration_ms <= 1500;
                break;
            case PandaAction::kProud:
                match = Contains(file.name, "成年") || Contains(file.name, "威风") ||
                        Contains(file.name, "叫声") || Contains(file.name, "call") ||
                        Contains(file.name, "roar");
                break;
        }
        if (match && (esp_random() % ++matches) == 0) {
            selected = static_cast<int>(i);
        }
    }
    if (selected >= 0) {
        return selected;
    }
    // 没有动作专属声音时，从非 ambient 文件中随机回退一个，保证兼容旧音频包。
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (std::strcmp(snapshot[i].category, "ambient") != 0 && (esp_random() % ++matches) == 0) {
            selected = static_cast<int>(i);
        }
    }
    return selected;
}

bool PandaAudio::PlayForAction(PandaAction action) {
    const int index = SelectFile(action);
    return index >= 0 && PlayIndex(index);
}

bool PandaAudio::PlayRandomSound(const char* category) {
    const int index = store_.RandomInCategory(category != nullptr ? category : "animal");
    return index >= 0 && PlayIndex(index);
}
