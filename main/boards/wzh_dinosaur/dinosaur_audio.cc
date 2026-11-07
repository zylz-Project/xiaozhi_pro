#include "dinosaur_audio.h"

#include <algorithm>
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
        ESP_LOGW(TAG, "外部音频 Flash 暂不可用；预置调校音画配套仍可使用");
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
                match = Contains(file.name, "吃") || Contains(file.name, "咀嚼") ||
                        Contains(file.name, "eat") || Contains(file.name, "chew");
                break;
            case DinoAction::kListen:
                match = Contains(file.name, "脚步") || Contains(file.name, "虫鸣") ||
                        Contains(file.name, "鸟叫") || Contains(file.name, "下雨") ||
                        Contains(file.name, "流水") || Contains(file.name, "step") ||
                        Contains(file.name, "foot") || Contains(file.name, "rain");
                break;
            case DinoAction::kSleepy:
                match = Contains(file.name, "入睡") || Contains(file.name, "困") ||
                        Contains(file.name, "sleep");
                break;
            case DinoAction::kStartled:
                match = Contains(file.name, "警觉") || Contains(file.name, "受惊") ||
                        Contains(file.name, "startle") || Contains(file.name, "alert");
                break;
            case DinoAction::kHappy:
                match = Contains(file.name, "雀跃") || Contains(file.name, "开心") ||
                        Contains(file.name, "宝宝") || Contains(file.name, "嘤嘤") ||
                        Contains(file.name, "happy") || Contains(file.name, "joy");
                break;
            case DinoAction::kAffection:
                match = Contains(file.name, "亲近") || Contains(file.name, "撒娇") ||
                        Contains(file.name, "安慰") || Contains(file.name, "comfort") ||
                        Contains(file.name, "nuzzle") || Contains(file.name, "cat") ||
                        Contains(file.name, "Cat");
                break;
            case DinoAction::kDiscover:
                match = Contains(file.name, "发现") || Contains(file.name, "探索") ||
                        Contains(file.name, "好奇") ||
                        (file.duration_ms > 0 && file.duration_ms <= 1500);
                break;
            case DinoAction::kProud:
                match = Contains(file.name, "叫声") || Contains(file.name, "咆哮") ||
                        Contains(file.name, "成年") || Contains(file.name, "威风") ||
                        Contains(file.name, "call") || Contains(file.name, "roar") ||
                        Contains(file.name, "adult");
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

int DinosaurAudio::SelectRandomNonAmbient() const {
    const auto snapshot = store_.Snapshot();
    int selected = -1;
    int matches = 0;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (std::strcmp(snapshot[i].category, "ambient") != 0 && (esp_random() % ++matches) == 0) {
            selected = static_cast<int>(i);
        }
    }
    return selected;
}

bool DinosaurAudio::QueueSelection(int index, DinoSoundSelection* selection) {
    if (index < 0 || !PlayIndex(index)) {
        return false;
    }
    if (selection != nullptr) {
        selection->index = index;
        selection->duration_ms = static_cast<uint32_t>(std::max(0, SoundDurationMs(index)));
    }
    return true;
}

bool DinosaurAudio::PlayForAction(DinoAction action, DinoSoundSelection* selection) {
    return QueueSelection(SelectFile(action), selection);
}

bool DinosaurAudio::PlayRandomSound(const char* category, DinoSoundSelection* selection) {
    const char* target_category = category != nullptr ? category : "ambient";
    int index = store_.RandomInCategory(target_category);
    // 老 PNDA v1 没有 category 字段；请求 animal 时回退到所有非 ambient
    // 条目，确保现场已有 Flash 也能进入“声音先选动作”的同步链路。
    if (index < 0 && std::strcmp(target_category, "animal") == 0) {
        index = SelectRandomNonAmbient();
    }
    return QueueSelection(index, selection);
}
