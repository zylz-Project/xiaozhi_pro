#include "ali_stream_protocol.h"

#include <cstring>
#include <memory>
#include <utility>

#include "cJSON.h"

namespace voice_tts {
namespace {
constexpr size_t kMaxEvent = 8192;
constexpr unsigned kMaxJsonDepth = 32;
struct JsonDeleter {
    void operator()(cJSON* value) const { cJSON_Delete(value); }
};
const char* StringItem(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}
bool BoundedDepth(const char* data, size_t size) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > kMaxJsonDepth)
                return false;
        } else if (c == '}' || c == ']') {
            if (!depth)
                return false;
            --depth;
        }
    }
    return !quoted && !depth;
}
}  // namespace

AliStreamProtocol::AliStreamProtocol(std::string task_id) : task_id_(std::move(task_id)) {}

AliStreamProtocol::Event AliStreamProtocol::Fail() {
    state_ = State::kFailed;
    return Event::kError;
}

AliStreamProtocol::Event AliStreamProtocol::OnText(const char* data, size_t size) {
    if (!data || !size || size > kMaxEvent || state_ == State::kFailed ||
        state_ == State::kFinished || audio_expected_ || std::memchr(data, 0, size) ||
        !BoundedDepth(data, size))
        return Fail();
    const char* end = nullptr;
    std::unique_ptr<cJSON, JsonDeleter> root(cJSON_ParseWithLengthOpts(data, size, &end, false));
    if (!root || !end)
        return Fail();
    while (end < data + size && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        ++end;
    if (end != data + size)
        return Fail();
    const cJSON* header = cJSON_GetObjectItemCaseSensitive(root.get(), "header");
    const char* task = StringItem(header, "task_id");
    const char* event = StringItem(header, "event");
    if (!task || task_id_ != task || !event)
        return Fail();
    if (std::strcmp(event, "task-failed") == 0)
        return Fail();
    if (std::strcmp(event, "task-started") == 0) {
        if (state_ != State::kStarting)
            return Fail();
        state_ = State::kRunning;
        return Event::kStarted;
    }
    if (std::strcmp(event, "task-finished") == 0) {
        if (state_ != State::kFinishing)
            return Fail();
        state_ = State::kFinished;
        return Event::kFinished;
    }
    if (state_ != State::kRunning && state_ != State::kFinishing)
        return Fail();
    if (std::strcmp(event, "result-generated") != 0)
        return Fail();
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root.get(), "payload");
    const cJSON* output = cJSON_GetObjectItemCaseSensitive(payload, "output");
    const char* type = StringItem(output, "type");
    if (!type)
        return Fail();
    if (std::strcmp(type, "sentence-synthesis") == 0) {
        audio_expected_ = true;
        return Event::kAudioFollows;
    }
    if (std::strcmp(type, "sentence-end") == 0)
        return Event::kSentenceEnd;
    if (std::strcmp(type, "sentence-begin") == 0)
        return Event::kOther;
    return Fail();
}

bool AliStreamProtocol::OnAudioMessage() {
    if (!audio_expected_ || (state_ != State::kRunning && state_ != State::kFinishing)) {
        Fail();
        return false;
    }
    audio_expected_ = false;
    return true;
}

bool AliStreamProtocol::MarkFinishSent() {
    if (state_ != State::kRunning)
        return false;
    state_ = State::kFinishing;
    return true;
}
}  // namespace voice_tts
