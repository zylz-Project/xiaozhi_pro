#include "wzh_audio_player.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "wzh_pet_behavior.h"

#include "application.h"
#include "audio_service.h"
#include "ogg_demuxer.h"

#define TAG "WzhAudioPlayer"

namespace wzh {
namespace {

constexpr size_t kReadChunk = 4096;

}  // namespace

bool AudioPlayer::Start(AudioStore* store) {
    store_ = store;
    // SPI 读取和 OGG 解复用可能持续数百毫秒，必须放到独立任务；20 KiB
    // 栈主要供 OggDemuxer 的包缓存和 4 KiB Flash 读取缓冲区使用。
    play_queue_ = xQueueCreate(3, sizeof(PlayRequest));
    if (play_queue_ == nullptr ||
        xTaskCreate(TaskEntry, "wzh_audio", 20480, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start offline audio task");
        return false;
    }
    return true;
}

void AudioPlayer::StopPlayback() {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    {
        playback_generation_.fetch_add(1);
        playback_pending_ = false;
        if (play_queue_ != nullptr) {
            PlayRequest discarded = {};
            while (xQueueReceive(play_queue_, &discarded, 0) == pdTRUE) {
            }
        }
    }
    is_playing_.store(false);
    current_name_.fill(0);
    local_pcm_start_us_.store(0);
    motion_level_.store(0.0f);
    motion_attack_.store(0.0f);
    // 本地声音的输出也会喂 tts_level_，进入对话时若不清零，speaking
    // 状态会带着上一段本地声音的包络立刻“舞动”。
    tts_updated_us_.store(0);
    tts_level_.store(0.0f);
    tts_attack_.store(0.0f);
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void AudioPlayer::SetMaintenance(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        maintenance_.store(enabled);
    }
    if (enabled)
        StopPlayback();
}

std::string AudioPlayer::CurrentName() const {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    return current_name_.data();
}

bool AudioPlayer::PlayIndex(int index) {
    if (const auto* pair = UserPairAtIndex(index)) {
        AudioFileInfo file{};
        std::snprintf(file.name, sizeof(file.name), "%s", pair->name);
        std::snprintf(file.category, sizeof(file.category), "animal");
        file.size = pair->audio_size;
        file.duration_ms = pair->audio_ms;
        file.sample_rate = 48000;
        return QueueFile(index, file);
    }
    AudioFileInfo file{};
    if (store_ == nullptr || !store_->GetFile(index, &file))
        return false;
    return QueueFile(index, file);
}

int AudioPlayer::FindSoundIndex(const char* name) const {
    const int paired = FindUserPair(name);
    return paired >= 0 ? paired : store_ && name ? store_->FindByName(name) : -1;
}

std::vector<AudioFileInfo> AudioPlayer::PairedFiles() const {
    std::vector<AudioFileInfo> files;
    files.reserve(user_pair_count);
    for (size_t i = 0; i < user_pair_count; ++i) {
        const auto& pair = user_pairs[i];
        AudioFileInfo file{};
        std::snprintf(file.name, sizeof(file.name), "%s", pair.name);
        std::snprintf(file.category, sizeof(file.category), "animal");
        file.size = pair.audio_size;
        file.duration_ms = pair.audio_ms;
        file.sample_rate = 48000;
        files.push_back(file);
    }
    return files;
}

bool AudioPlayer::QueueFile(int index, const AudioFileInfo& file) {
    if (play_queue_ == nullptr || store_ == nullptr || index < 0 || file.size == 0 ||
        store_->IsWriting() || maintenance_.load()) {
        return false;
    }
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking || !app.GetAudioService().IsPlaybackIdle()) {
        return false;
    }
    // Keep the selected metadata with the request: Hub deletions may shift directory indexes.
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (playback_pending_ || maintenance_.load()) {
        return false;
    }
    const PlayRequest request{index, playback_generation_.load(), file};
    if (xQueueSend(play_queue_, &request, 0) != pdTRUE) {
        return false;
    }
    playback_pending_ = true;
    return true;
}

bool AudioPlayer::PlayInteractionSound(PetInteraction interaction) {
    if (store_ == nullptr || interaction == PetInteraction::kNone || store_->IsWriting()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(reaction_mutex_);
    const bool paired = user_pair_count > 0;
    const auto files = paired ? PairedFiles() : store_->Snapshot();
    const int index = interaction_sounds_.Select(files, interaction, esp_random());
    if (index < 0) {
        ESP_LOGD(TAG, "Interaction=%s: no external animal sound available",
                 InteractionName(interaction));
        return false;
    }
    if (!QueueFile(paired ? kUserPairIndexBase + index : index, files[index])) {
        return false;
    }
    interaction_sounds_.Remember(files[index].name);
    ESP_LOGI(TAG, "Interaction=%s external Flash sound: %s", InteractionName(interaction),
             files[index].name);
    return true;
}

bool AudioPlayer::PlayAutonomousSound(bool include_ambient, uint32_t ambient_percent) {
    if (!store_ || store_->IsWriting())
        return false;
    std::lock_guard<std::mutex> lock(reaction_mutex_);
    // Three accepted showcases use the exact user pair, then Flash coverage
    // gets a turn. Busy/rejected enqueues consume no preference.
    if (user_pair_count && (autonomous_turn_ % 4 != 3 || store_->file_count() == 0)) {
        const auto pairs = PairedFiles();
        const int selected = paired_coverage_.Select(pairs, esp_random(), false);
        if (selected >= 0 && QueueFile(kUserPairIndexBase + selected, pairs[selected])) {
            paired_coverage_.Remember(pairs, pairs[selected].name, false);
            ++autonomous_turn_;
            return true;
        }
        return false;
    }
    const auto files = store_->Snapshot();
    const uint32_t random = esp_random();
    // Animal sounds should dominate the dinosaur's exhibition. Ambient still
    // gets a fair turn, but only when the weighted draw selects it.
    const bool ambient_only =
        include_ambient && ambient_percent > 0 && (random % 100U) < ambient_percent;
    const bool animal_only = !include_ambient || (include_ambient && !ambient_only);
    const int index = ambient_only  ? sound_coverage_.Select(files, random, true, true)
                      : animal_only ? sound_coverage_.Select(files, random, false, false)
                                    : sound_coverage_.Select(files, random, true, false);
    if (index < 0) {
        if (!user_pair_count)
            return false;
        const auto pairs = PairedFiles();
        const int fallback = paired_coverage_.Select(pairs, random, false);
        if (fallback < 0 || !QueueFile(kUserPairIndexBase + fallback, pairs[fallback]))
            return false;
        paired_coverage_.Remember(pairs, pairs[fallback].name, false);
        ++autonomous_turn_;
        return true;
    }
    auto selected = files[index];
    if (!selected.duration_ms) {
        if (const auto* score = FindSoundScore(selected.name))
            selected.duration_ms = score->duration_ms;
    }
    // Exhibition cadence: every dinosaur ambience gets a turn, as a 12 s scene.
    // Manual/MCP playback continues to play the complete recording.
    if (ClassifySoundKind(selected.name, selected.category) == SoundKind::kAmbient)
        selected.duration_ms =
            selected.duration_ms ? std::min<uint32_t>(selected.duration_ms, 12000) : 12000;
    if (!QueueFile(index, selected))
        return false;
    sound_coverage_.Remember(files, files[index].name, include_ambient, ambient_only);
    ++autonomous_turn_;
    return true;
}

MotionData AudioPlayer::GetMotionData() const {
    MotionData data;
    data.playing = is_playing_.load();
    data.level = tts_level_.load();
    data.attack = tts_attack_.load();
    data.duration_ms = motion_duration_ms_.load();
    data.sound_index = motion_sound_index_.load();
    data.score = motion_score_.load();
    data.kind = motion_kind_.load();
    data.sequence = motion_sequence_.load();
    const int64_t start = local_pcm_start_us_.load();
    data.audible = start > 0;
    data.elapsed_ms =
        start > 0
            ? static_cast<uint32_t>(std::max<int64_t>(0, (esp_timer_get_time() - start) / 1000))
            : 0;
    return data;
}

bool AudioPlayer::LocalCommandEchoRisk() {
    {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (playback_pending_ || is_playing_.load())
            return true;
    }
    const int64_t now = esp_timer_get_time();
    const int64_t updated = tts_updated_us_.load();
    return (updated > 0 && now >= updated && now - updated < 350000) ||
           !Application::GetInstance().GetAudioService().IsPlaybackIdle();
}

void AudioPlayer::FeedTtsLevel(float level, float attack) {
    if (is_playing_.load()) {
        int64_t expected = 0;
        local_pcm_start_us_.compare_exchange_strong(
            expected,
            std::max<int64_t>(1, esp_timer_get_time() - local_packet_ms_.load() * 1000LL));
    }
    tts_level_.store(level);
    tts_attack_.store(attack);
    tts_updated_us_.store(esp_timer_get_time());
}

MotionData AudioPlayer::GetTtsMotionData() const {
    MotionData data = {};
    const bool fresh = RecentAudioOutput(esp_timer_get_time(), tts_updated_us_.load());
    data.level = fresh ? tts_level_.load() : 0.0f;
    data.attack = fresh ? tts_attack_.load() : 0.0f;
    // Fresh PCM includes word gaps; the voice helper smooths their energy
    // instead of restarting its pose on every quiet 60 ms packet.
    data.playing = fresh;
    data.audible = fresh;
    data.sound_index = -1;
    return data;
}

void AudioPlayer::TaskEntry(void* arg) { static_cast<AudioPlayer*>(arg)->Task(); }

void AudioPlayer::Task() {
    PlayRequest request = {};
    while (true) {
        if (xQueueReceive(play_queue_, &request, portMAX_DELAY) == pdTRUE) {
            {
                std::lock_guard<std::mutex> lock(playback_mutex_);
                if (maintenance_.load() || request.generation != playback_generation_.load())
                    continue;
                worker_active_.store(true);
            }
            PlayFile(request);
            std::lock_guard<std::mutex> lock(playback_mutex_);
            worker_active_.store(false);
            if (request.generation == playback_generation_.load()) {
                playback_pending_ = false;
                is_playing_.store(false);
            }
        }
    }
}

bool AudioPlayer::PlayFile(const PlayRequest& request) {
    const auto generation = request.generation;
    const auto& file = request.file;
    const auto* pair = UserPairAtIndex(request.index);
    if (generation != playback_generation_.load() || store_ == nullptr || store_->IsWriting()) {
        return false;
    }
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking || !app.GetAudioService().IsPlaybackIdle()) {
        return false;
    }
    const auto* score = pair && pair->score ? pair->score : FindSoundScore(file.name);
    const uint32_t duration_ms = file.duration_ms ? file.duration_ms
                                 : score          ? score->duration_ms
                                                  : 0;
    uint32_t decoder_generation = 0;
    {
        // Stop may have run after the initial idle check. Publish metadata and
        // reserve the decoder session atomically with respect to StopPlayback.
        std::lock_guard<std::mutex> lock(playback_mutex_);
        if (generation != playback_generation_.load() || store_->IsWriting() || maintenance_.load())
            return false;
        std::snprintf(current_name_.data(), current_name_.size(), "%s", file.name);
        motion_score_.store(score);
        motion_kind_.store(score ? score->kind : ClassifySoundKind(file.name, file.category));
        local_pcm_start_us_.store(0);
        motion_sequence_.fetch_add(1);
        motion_sound_index_.store(request.index);
        motion_elapsed_ms_.store(0);
        motion_duration_ms_.store(duration_ms);
        motion_level_.store(0.0f);
        motion_attack_.store(0.0f);
        is_playing_.store(true);
        decoder_generation = app.GetAudioService().GetPlaybackGeneration();
    }

    // OggDemuxer 只拆出 Opus 包；真正的解码和 I2S 输出仍由 AudioService 完成。
    bool ok = true;
    uint32_t submitted_ms = 0;
    OggDemuxer demuxer;
    demuxer.OnDemuxerFinished(
        [this, &app, &ok, &submitted_ms, &file, generation, decoder_generation](
            const uint8_t* data, int sample_rate, size_t size) {
            if (playback_generation_.load() != generation) {
                ok = false;
                return;
            }
            if (file.duration_ms && submitted_ms >= file.duration_ms)
                return;
            const int packet_ms = OpusPacketDurationMs(data, size);
            if (packet_ms == 0) {
                ok = false;
                return;
            }
            local_packet_ms_.store(packet_ms);
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = packet_ms;
            submitted_ms += packet_ms;
            packet->payload.assign(data, data + size);
            if (!app.GetAudioService().PushPacketToDecodeQueue(std::move(packet), true,
                                                               decoder_generation)) {
                ok = false;
            }
        });

    // 分块顺序读取，避免为完整音频文件申请大块 RAM。
    uint8_t buffer[kReadChunk];
    ESP_LOGI(TAG, "Offline sound: %s (%lu ms), source=%s index=%d", file.name,
             static_cast<unsigned long>(duration_ms), pair ? "user-pair" : "external-flash",
             request.index);
    if (pair) {
        ESP_LOGI(
            TAG,
            "Pair timeline: recorded=%lu ms audio=%lu ms; continuous filtering and smooth settling",
            static_cast<unsigned long>(pair->motion_ms),
            static_cast<unsigned long>(pair->audio_ms));
    }
    const uint32_t base = store_->data_start() + file.offset;
    for (uint32_t offset = 0; ok && playback_generation_.load() == generation &&
                              !store_->IsWriting() && offset < file.size &&
                              (!file.duration_ms || submitted_ms < file.duration_ms);) {
        const size_t chunk = std::min<size_t>(sizeof(buffer), file.size - offset);
        if (pair) {
            if (offset + chunk > pair->audio_size) {
                ok = false;
                break;
            }
            std::memcpy(buffer, pair->audio + offset, chunk);
        } else if (wzh_flash_read(base + offset, buffer, chunk) != ESP_OK) {
            ok = false;
            break;
        }
        demuxer.Process(buffer, chunk);
        offset += chunk;
    }
    // Reading/decode-ahead is not speaker time. Keep the exact score alive until
    // the last PCM buffer has actually drained. Never reset a newer generation.
    const int64_t drain_deadline = esp_timer_get_time() + 10000000;
    while (playback_generation_.load() == generation && !store_->IsWriting() &&
           !app.GetAudioService().IsPlaybackIdle() && esp_timer_get_time() < drain_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (playback_generation_.load() == generation) {
        is_playing_.store(false);
        motion_level_.store(0.0f);
        motion_attack_.store(0.0f);
    }
    return ok && playback_generation_.load() == generation && !store_->IsWriting();
}

}  // namespace wzh
