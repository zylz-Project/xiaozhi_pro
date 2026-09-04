#include "dinosaur_audio.h"

#include <cstring>

#include <esp_log.h>
#include <esp_random.h>

#include "dinosaur_controller.h"

#define TAG "DinosaurAudio"

namespace {
bool Contains(const char* text, const char* token) {
    return text != nullptr && std::strstr(text, token) != nullptr;
}
}  // namespace

bool DinosaurAudio::Initialize() {
    if (!store_.Init()) {
        ESP_LOGW(TAG, "外部音频 Flash 不可用，离线声音停用（动作不受影响）");
        return false;
    }
    if (!wzh::AudioPlayer::Start(&store_)) {
        return false;
    }
    ESP_LOGI(TAG, "已加载 %d 个离线声音", store_.file_count());
    return true;
}

const char* DinosaurAudio::SoundName(int index) const {
    const auto* file = store_.PeekFile(index);
    return file != nullptr ? file->name : "???";
}

int DinosaurAudio::SoundDurationMs(int index) const {
    const auto* file = store_.PeekFile(index);
    return file != nullptr ? static_cast<int>(file->duration_ms) : 0;
}

int DinosaurAudio::SelectFile(DinoAction action) const {
    // 在目录快照上做蓄水池采样，保证同步任务改写目录时选择逻辑安全。
    const auto snapshot = store_.Snapshot();
    int selected = -1;
    int matches = 0;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& file = snapshot[i];
        if (std::strcmp(file.category, "ambient") == 0) {
            continue;
        }
        bool match = false;
        switch (action) {
            case DinoAction::kEat:
                match = Contains(file.name, "咀嚼") || Contains(file.name, "eat") ||
                        Contains(file.name, "chew");
                break;
            case DinoAction::kListen:
                match = Contains(file.name, "脚步") || Contains(file.name, "step") ||
                        Contains(file.name, "foot");
                break;
            case DinoAction::kSleepy:
                match = Contains(file.name, "入睡") || Contains(file.name, "sleep");
                break;
            case DinoAction::kStartled:
                match = Contains(file.name, "警觉") || Contains(file.name, "startle");
                break;
            case DinoAction::kHappy:
                match = Contains(file.name, "雀跃") || Contains(file.name, "happy") ||
                        Contains(file.name, "joy");
                break;
            case DinoAction::kAffection:
                match = Contains(file.name, "亲近") || Contains(file.name, "comfort") ||
                        Contains(file.name, "nuzzle");
                break;
            case DinoAction::kDiscover:
                match = file.duration_ms > 0 && file.duration_ms <= 1500;
                break;
            case DinoAction::kProud:
                match = Contains(file.name, "叫声") || Contains(file.name, "call") ||
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
        if (std::strcmp(snapshot[i].category, "ambient") != 0 &&
            (esp_random() % ++matches) == 0) {
            selected = static_cast<int>(i);
        }
    }
    return selected;
}

bool DinosaurAudio::PlayForAction(DinoAction action) {
    const int index = SelectFile(action);
    return index >= 0 && PlayIndex(index);
}

bool DinosaurAudio::PlayRandomSound(const char* category) {
    const int index = store_.RandomInCategory(category != nullptr ? category : "ambient");
    return index >= 0 && PlayIndex(index);
}
