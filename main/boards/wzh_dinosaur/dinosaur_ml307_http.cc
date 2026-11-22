#include "dinosaur_ml307_http.h"

#include <sdkconfig.h>
#include "config.h"

#if DINOSAUR_ENABLE_4G

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dinosaur_ml307_at.h"

namespace {
constexpr char kTag[] = "DinoMl307Http";
constexpr unsigned kHttpSlotCount = 4;
constexpr size_t kMaxUrlBytes = 2048;
constexpr size_t kMaxPathBytes = 1024;
constexpr size_t kMaxHeaderCount = 24;
// ML307R accepts at most 1024 bytes for one MHTTPHEADER field in command mode.
// Keeping the limit here prevents a long header from producing a CME error
// halfway through request setup.
constexpr size_t kMaxHeaderLineBytes = 1024;
constexpr size_t kMaxContentChunkBytes = 4096;
// ML307R reports the complete response-header length in every header URC. A
// fragmented header therefore arrives as several URCs carrying the same total
// length. Bound reassembly so a malformed or hostile modem cannot grow this
// buffer without limit while Read() is waiting.
constexpr size_t kMaxResponseHeaderBytes = 8192;
// A response is consumed by Read() in normal downloads. Direct buffering is
// reserved for short JSON/control responses; failing explicitly is safer than
// returning a truncated body after exhausting PSRAM.
constexpr size_t kMaxBufferedBodyBytes = 128 * 1024;
constexpr size_t kMaxReadAllBytes = 128 * 1024;
constexpr int kCreateTimeoutMs = 5000;
constexpr int kSyncTimeoutMs = 1500;
constexpr int kSweepTimeoutMs = 1000;
constexpr int kConfigTimeoutMs = 5000;
constexpr int kRequestTimeoutMs = 8000;
constexpr int kIndicatorTimeoutMs = 5000;
constexpr int kTerminateTimeoutMs = 1500;
constexpr int kCloseTimeoutMs = 3000;

// +MHTTPCREATE does not identify the caller. Serialize allocation so only the
// instance with awaiting_create_ set can claim the returned HTTP id.
std::mutex s_http_create_mutex;

// ML307 exposes four HTTP ids. Accounting here prevents a burst of requests
// (for example OTA metadata plus an asset check) from racing allocation.
std::mutex s_http_slot_mutex;
std::condition_variable s_http_slot_cv;
unsigned s_http_slots_in_use = 0;

// A command timeout leaves the modem's HTTP id unknown until the AT parser has
// consumed everything the modem emitted for that command. Track the slots this
// process still owns so a later allocation can safely delete every unowned id.
std::mutex s_http_registry_mutex;
std::set<int> s_active_http_ids;
std::atomic<bool> s_http_reconcile_required{false};

void RegisterHttpId(int id) {
    std::lock_guard<std::mutex> lock(s_http_registry_mutex);
    s_active_http_ids.insert(id);
}

void UnregisterHttpId(int id) {
    std::lock_guard<std::mutex> lock(s_http_registry_mutex);
    s_active_http_ids.erase(id);
}

std::set<int> SnapshotActiveHttpIds() {
    std::lock_guard<std::mutex> lock(s_http_registry_mutex);
    return s_active_http_ids;
}

// Two plain AT probes are intentional. The first can consume a final OK that
// arrived after a command timed out; the second proves that a fresh command
// generated after all of that output can still complete. Only then is it safe
// to associate a later +MHTTPCREATE with a new request.
bool SynchronizeAtParser(AtUart& uart, const char* reason) {
    for (int pass = 0; pass < 2; ++pass) {
        if (!AtCommandSucceeded(uart.SendCommand("AT", kSyncTimeoutMs))) {
            ESP_LOGW(kTag, "AT parser did not resynchronise %s (pass %d)", reason, pass + 1);
            return false;
        }
    }
    return true;
}

// This runs on a normal task, never in a URC callback. New HTTP allocations
// are serialized by s_http_create_mutex, so no other request can claim an id
// while the unowned ones are being removed.
bool DeleteUnownedHttpSlots(AtUart& uart, const char* reason) {
    const std::set<int> active = SnapshotActiveHttpIds();
    bool transport_ok = true;
    for (int id = 0; id < static_cast<int>(kHttpSlotCount); ++id) {
        if (active.find(id) != active.end())
            continue;
        const auto result = uart.SendCommand("AT+MHTTPDEL=" + std::to_string(id), kSweepTimeoutMs);
        if (result)
            continue;
        if (AtCommandCme(result) != 0) {
            // The documented response for deleting an idle id is a CME error.
            // It proves the AT channel answered and the slot was already free.
            ESP_LOGD(kTag, "HTTP slot %d was already free during cleanup (%s)", id, reason);
        } else {
            ESP_LOGW(kTag, "Could not delete stale HTTP slot %d during cleanup (%s)", id, reason);
            transport_ok = false;
        }
    }
    return transport_ok;
}

bool ReconcileHttpSlots(AtUart& uart, const char* reason) {
    if (!SynchronizeAtParser(uart, reason))
        return false;
    return DeleteUnownedHttpSlots(uart, reason);
}

bool IsInt(const AtArgumentValue& value) { return value.type == AtArgumentValue::Type::Int; }

bool IsString(const AtArgumentValue& value) { return value.type == AtArgumentValue::Type::String; }

// The AT parser reports an unquoted all-digit HEX blob shorter than ten
// characters as Int while preserving the original text in string_value.
bool IsHexValue(const AtArgumentValue& value) {
    return value.type == AtArgumentValue::Type::Int || value.type == AtArgumentValue::Type::String;
}

bool IsHexString(const std::string& value) {
    if ((value.size() & 1U) != 0)
        return false;
    for (unsigned char ch : value) {
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}

size_t TimeoutForBytes(const AtUart& uart, size_t bytes, size_t floor_ms) {
    int baud = uart.GetBaudRate();
    if (baud <= 0)
        baud = 115200;
    const uint64_t tx_ms =
        (static_cast<uint64_t>(bytes) * 10ULL * 1000ULL) / static_cast<unsigned>(baud);
    return std::max<size_t>(floor_ms, static_cast<size_t>(tx_ms) + 2000);
}

bool SafeHeaderName(const std::string& name) {
    if (name.empty() || name.size() > 64)
        return false;
    for (unsigned char ch : name) {
        if (!(std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' ||
              ch == '\'' || ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' ||
              ch == '_' || ch == '`' || ch == '|' || ch == '~')) {
            return false;
        }
    }
    return true;
}

bool SafeHeaderValue(const std::string& value) {
    if (value.size() > kMaxHeaderLineBytes)
        return false;
    for (unsigned char ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '"' || ch == 0x7f || (ch < 0x20 && ch != '\t'))
            return false;
    }
    return true;
}

bool SafeAuthority(const std::string& authority) {
    if (authority.empty() || authority.size() > 255 || authority.find('@') != std::string::npos ||
        authority.find('\\') != std::string::npos)
        return false;
    bool bracket = false;
    size_t colon_count = 0;
    for (unsigned char ch : authority) {
        if (ch == '[') {
            if (bracket)
                return false;
            bracket = true;
        } else if (ch == ']') {
            if (!bracket)
                return false;
            bracket = false;
        } else if (ch == ':') {
            ++colon_count;
            if (!bracket && colon_count > 1)
                return false;
        } else if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_')) {
            return false;
        }
    }
    return !bracket;
}

bool ParseStatusHeader(const std::string& decoded, int* status, bool* chunked,
                       bool* has_content_length, size_t* content_length,
                       std::map<std::string, std::string>* response_headers) {
    if (!status || !chunked || !has_content_length || !content_length || !response_headers)
        return false;
    *status = 0;
    *chunked = false;
    *has_content_length = false;
    *content_length = 0;
    response_headers->clear();

    bool status_seen = false;
    bool transfer_seen = false;
    bool length_seen = false;
    size_t length_value = 0;
    size_t cursor = 0;
    while (cursor <= decoded.size()) {
        const size_t line_end = decoded.find('\n', cursor);
        const size_t end = line_end == std::string::npos ? decoded.size() : line_end;
        size_t last = end;
        if (last > cursor && decoded[last - 1] == '\r')
            --last;
        if (cursor == last)
            break;
        const std::string line = decoded.substr(cursor, last - cursor);
        if (!status_seen) {
            if (line.rfind("HTTP/", 0) != 0)
                return false;
            const size_t version_end = line.find(' ', 5);
            if (version_end == std::string::npos || version_end + 4 > line.size() ||
                !std::isdigit(static_cast<unsigned char>(line[5])) || line[6] != '.' ||
                !std::isdigit(static_cast<unsigned char>(line[7])) ||
                !std::isdigit(static_cast<unsigned char>(line[version_end + 1])) ||
                !std::isdigit(static_cast<unsigned char>(line[version_end + 2])) ||
                !std::isdigit(static_cast<unsigned char>(line[version_end + 3])) ||
                (version_end + 4 < line.size() && line[version_end + 4] != ' '))
                return false;
            *status = (line[version_end + 1] - '0') * 100 + (line[version_end + 2] - '0') * 10 +
                      (line[version_end + 3] - '0');
            if (*status < 100 || *status > 599)
                return false;
            status_seen = true;
        } else {
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0)
                return false;
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();
            size_t first = 0;
            while (first < value.size() && (value[first] == ' ' || value[first] == '\t'))
                ++first;
            value.erase(0, first);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.pop_back();
            if (!SafeHeaderName(name) || !SafeHeaderValue(value))
                return false;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (name == "transfer-encoding") {
                if (transfer_seen)
                    return false;
                transfer_seen = true;
                std::string token = value;
                std::transform(token.begin(), token.end(), token.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                size_t first_token = 0;
                while (first_token < token.size() &&
                       (token[first_token] == ' ' || token[first_token] == '\t'))
                    ++first_token;
                token.erase(0, first_token);
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                    token.pop_back();
                if (token != "chunked")
                    return false;
                *chunked = true;
            } else if (name == "content-length") {
                if (length_seen)
                    return false;
                length_seen = true;
                const auto result =
                    std::from_chars(value.data(), value.data() + value.size(), length_value, 10);
                if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
                    value.empty())
                    return false;
                *has_content_length = true;
                *content_length = length_value;
            }
            const auto existing = response_headers->find(name);
            if (existing == response_headers->end()) {
                response_headers->emplace(name, value);
            } else if (name == "content-length" || name == "transfer-encoding") {
                return false;
            } else {
                existing->second += ", ";
                existing->second += value;
            }
        }
        if (line_end == std::string::npos)
            break;
        cursor = line_end + 1;
    }
    if (status_seen && *chunked && *has_content_length)
        return false;
    // ML307 header URCs may omit the final empty line: the documented
    // header_len counts the status line and fields, not an extra CRLF pair.
    // Header completeness is therefore decided by header_len in
    // AppendHeaderFragment(), not by an HTTP blank-line terminator here.
    return status_seen;
}

}  // namespace

DinosaurMl307Http::DinosaurMl307Http(std::shared_ptr<AtUart> at_uart)
    : at_uart_(std::move(at_uart)) {
    if (!at_uart_) {
        ESP_LOGE(kTag, "HTTP transport has no AT UART");
        return;
    }
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            HandleUrc(command, arguments);
        });
}

DinosaurMl307Http::~DinosaurMl307Http() {
    cancelled_.store(true);
    if (at_uart_) {
        at_uart_->UnregisterUrcCallback(urc_callback_it_);
    }
    // Close after unregistering: MHTTPDEL is then safe even if the object is
    // being destroyed while the AT task is still delivering other URCs.
    Close();
    ReleaseSlot();
}

void DinosaurMl307Http::SetTimeout(int timeout_ms) {
    timeout_ms_ = std::clamp(timeout_ms, 1000, 120000);
}

void DinosaurMl307Http::SetHeader(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!SafeHeaderName(key) || !SafeHeaderValue(value)) {
        ESP_LOGE(kTag, "Rejected unsafe HTTP header");
        return;
    }
    request_headers_[key] = value;
}

void DinosaurMl307Http::SetContent(std::string&& content) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    content_ = std::move(content);
}

void DinosaurMl307Http::SetKeepAlive(bool enable) {
    (void)enable;
    ESP_LOGW(kTag, "HTTP keep-alive is not supported by the ML307 AT transport");
}

bool DinosaurMl307Http::ParseUrl(const std::string& url) {
    if (url.empty() || url.size() > kMaxUrlBytes)
        return false;
    for (unsigned char ch : url) {
        if (ch < 0x20 || ch == 0x7f || ch == '"' || ch == '\\')
            return false;
    }
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return false;
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https")
        return false;
    if (url.find('#', scheme_end + 3) != std::string::npos)
        return false;
    const size_t authority_start = scheme_end + 3;
    const size_t path_start = url.find_first_of("/?", authority_start);
    const std::string authority = path_start == std::string::npos
                                      ? url.substr(authority_start)
                                      : url.substr(authority_start, path_start - authority_start);
    if (!SafeAuthority(authority))
        return false;
    if (path_start == std::string::npos) {
        path_ = "/";
    } else if (url[path_start] == '?') {
        path_ = "/" + url.substr(path_start);
    } else {
        path_ = url.substr(path_start);
    }
    if (path_.size() > kMaxPathBytes)
        return false;
    secure_ = scheme == "https";
    origin_ = scheme + "://" + authority;
    return true;
}

bool DinosaurMl307Http::BuildRequestHeaders(std::vector<HeaderEntry>* headers) {
    if (!headers)
        return false;
    headers->clear();
    const bool supports_content = method_ == "POST" || method_ == "PUT";
    bool content_length_seen = false;
    std::set<std::string> seen_names;
    for (const auto& item : request_headers_) {
        if (!SafeHeaderName(item.first) || !SafeHeaderValue(item.second) ||
            item.first.size() + item.second.size() + 2 > kMaxHeaderLineBytes)
            return false;
        std::string lower = item.first;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        // Header names are case-insensitive. std::map keys by the exact
        // spelling, so reject "Content-Length" plus "content-length" rather
        // than handing the modem two contradictory framing headers.
        if (!seen_names.insert(lower).second)
            return false;
        if (lower == "transfer-encoding") {
            // Streaming callers (camera/video upload) set this header while
            // MHTTPCFG also enables modem framing. Accept only that exact
            // mode; any other transfer coding or a length-framed request is
            // ambiguous and must not reach the modem.
            std::string coding = item.second;
            std::transform(coding.begin(), coding.end(), coding.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!request_chunked_ || coding != "chunked")
                return false;
        }
        if (lower == "content-length") {
            if (content_length_seen)
                return false;
            content_length_seen = true;
            size_t declared = 0;
            const auto parsed = std::from_chars(
                item.second.data(), item.second.data() + item.second.size(), declared, 10);
            if (item.second.empty() || parsed.ec != std::errc() ||
                parsed.ptr != item.second.data() + item.second.size() || !supports_content ||
                !content_.has_value() || declared != content_->size())
                return false;
        }
        headers->push_back({item.first, item.second});
        if (headers->size() > kMaxHeaderCount)
            return false;
    }

    if (supports_content && content_.has_value()) {
        if (!content_length_seen) {
            headers->push_back({"Content-Length", std::to_string(content_->size())});
        }
    } else if (supports_content) {
        if (content_length_seen)
            return false;
    }
    return headers->size() <= kMaxHeaderCount;
}

DinosaurMl307Http::HeaderFragmentResult DinosaurMl307Http::AppendHeaderFragment(
    const std::string& decoded, int status, size_t header_length, std::string* complete_headers) {
    if (!complete_headers || decoded.empty() || header_length == 0 ||
        header_length > kMaxResponseHeaderBytes) {
        return HeaderFragmentResult::kInvalid;
    }
    if (!header_fragment_active_) {
        header_fragment_active_ = true;
        header_fragment_status_ = status;
        header_fragment_length_ = header_length;
        header_fragment_data_.clear();
    } else if (header_fragment_status_ != status || header_fragment_length_ != header_length) {
        // Interleaved responses or a changed total length cannot belong to
        // the same header block. Reject instead of concatenating unrelated
        // bytes into a value that might later parse as valid HTTP.
        return HeaderFragmentResult::kInvalid;
    }

    if (header_fragment_data_.size() > header_fragment_length_ ||
        decoded.size() > header_fragment_length_ - header_fragment_data_.size())
        return HeaderFragmentResult::kInvalid;
    header_fragment_data_.append(decoded);
    if (header_fragment_data_.size() < header_fragment_length_)
        return HeaderFragmentResult::kNeedMore;

    *complete_headers = std::move(header_fragment_data_);
    header_fragment_active_ = false;
    header_fragment_status_ = 0;
    header_fragment_length_ = 0;
    header_fragment_data_.clear();
    return HeaderFragmentResult::kComplete;
}

NetworkResult<> DinosaurMl307Http::Open(const std::string& method, const std::string& url) {
    std::lock_guard<std::mutex> open_lock(open_mutex_);
    if (!at_uart_) {
        last_error_.store(-1);
        return Fail(NetworkError::NotInitialized());
    }
    if (instance_active_.load() || http_id_.load() >= 0 || slot_acquired_.load()) {
        // Open() begins a new request. Reusing an adapter without Close() must
        // not leave the previous modem slot allocated.
        ESP_LOGW(kTag, "Closing the previous HTTP request before reopening");
        Close();
    }

    method_ = method;
    std::transform(method_.begin(), method_.end(), method_.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    int method_value = 0;
    if (method_ == "GET")
        method_value = 1;
    else if (method_ == "POST")
        method_value = 2;
    else if (method_ == "PUT")
        method_value = 3;
    else if (method_ == "DELETE")
        method_value = 4;
    else if (method_ == "HEAD")
        method_value = 5;
    if (method_value == 0) {
        ESP_LOGE(kTag, "Unsupported HTTP method");
        last_error_.store(-1);
        return Fail(NetworkError::InvalidArgument());
    }
    if (!ParseUrl(url)) {
        ESP_LOGE(kTag, "Invalid or unsafe HTTP URL");
        last_error_.store(-1);
        return Fail(NetworkError::InvalidArgument());
    }

    cancelled_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ResetResponseLocked();
        last_error_.store(0);
    }

    std::unique_lock<std::mutex> slot_lock(s_http_slot_mutex);
    const bool slot_available = s_http_slot_cv.wait_for(
        slot_lock, std::chrono::milliseconds(std::min(timeout_ms_, 30000)),
        [this] { return s_http_slots_in_use < kHttpSlotCount || cancelled_.load(); });
    if (!slot_available || cancelled_.load()) {
        ESP_LOGE(kTag, "Timed out waiting for a free ML307 HTTP slot");
        last_error_.store(-1);
        return Fail(NetworkError::Timeout());
    }
    ++s_http_slots_in_use;
    slot_acquired_.store(true);
    slot_lock.unlock();

    {
        std::lock_guard<std::mutex> create_lock(s_http_create_mutex);
        if (s_http_reconcile_required.load()) {
            // A previous allocation timed out. Resolve it before sending a new
            // MHTTPCREATE, otherwise its delayed URC could be mistaken for the
            // id of this request.
            if (!ReconcileHttpSlots(*at_uart_, "before allocation")) {
                last_error_.store(-1);
                ReleaseSlot();
                return Fail(NetworkError::Timeout());
            }
            s_http_reconcile_required.store(false);
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            awaiting_create_ = true;
            http_id_.store(-1);
            instance_active_.store(false);
        }
        const auto create_result =
            at_uart_->SendCommand("AT+MHTTPCREATE=\"" + origin_ + "\"", kCreateTimeoutMs);
        const bool command_ok = AtCommandSucceeded(create_result);
        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool created = state_cv_.wait_for(
            lock, std::chrono::milliseconds(kCreateTimeoutMs),
            [this] { return http_id_.load() >= 0 || error_ || cancelled_.load(); });
        awaiting_create_ = false;
        const int id = http_id_.load();
        if ((!command_ok && id < 0) || !created || id < 0 ||
            id >= static_cast<int>(kHttpSlotCount) || error_ || cancelled_.load()) {
            ESP_LOGE(kTag, "Failed to allocate an ML307 HTTP slot (command=%s id=%d)",
                     command_ok ? "ok" : "failed", id);
            const bool uncertain = command_ok || AtCommandCme(create_result) == 0;
            const NetworkError error =
                command_ok ? NetworkError::Timeout() : create_result.error().ToNetworkError();
            lock.unlock();
            Close();
            last_error_.store(-1);
            if (uncertain) {
                // Keep the flag set even when this immediate cleanup succeeds:
                // a URC can still be emitted after the failed command. The next
                // allocation repeats the barrier before creating a new slot.
                s_http_reconcile_required.store(true);
                static_cast<void>(ReconcileHttpSlots(*at_uart_, "after failed allocation"));
            }
            return Fail(error);
        }
        ESP_LOGI(kTag, "HTTP slot %d allocated for %s://%s", id, secure_ ? "https" : "http",
                 origin_.c_str() + (secure_ ? 8 : 7));
    }

    const int id = http_id_.load();
    auto configure = [this, id](const std::string& command) {
        return at_uart_->SendCommand(command, kConfigTimeoutMs);
    };
    auto fail_open = [this, id](const char* reason, NetworkError error) -> NetworkResult<> {
        ESP_LOGE(kTag, "HTTP open failed: %s (id=%d)", reason, id);
        last_error_.store(-1);
        Close();
        return Fail(error);
    };

    if (secure_ && !configure("AT+MHTTPCFG=\"ssl\"," + std::to_string(id) + ",1,0"))
        return fail_open("TLS configuration", NetworkError::AtCommandFailed());

    const bool supports_content = method_ == "POST" || method_ == "PUT";
    request_chunked_ = supports_content && !content_.has_value();
    if (request_chunked_ && !configure("AT+MHTTPCFG=\"chunked\"," + std::to_string(id) + ",1"))
        return fail_open("chunked request configuration", NetworkError::AtCommandFailed());
    if (!configure("AT+MHTTPCFG=\"encoding\"," + std::to_string(id) + ",0,0"))
        return fail_open("request encoding configuration", NetworkError::AtCommandFailed());

    std::vector<HeaderEntry> headers;
    if (!BuildRequestHeaders(&headers))
        return fail_open("invalid request headers", NetworkError::InvalidArgument());
    for (size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        const std::string line = header.key + ": " + header.value;
        // <eof>=0 marks the end of this request's header input. The modem will
        // not associate later fields with this request until it sees that
        // marker, so only the final field may use 0.
        const std::string command = "AT+MHTTPHEADER=" + std::to_string(id) +
                                    (i + 1 == headers.size() ? ",0," : ",1,") +
                                    std::to_string(line.size()) + ",\"" + line + "\"";
        if (!configure(command))
            return fail_open("header upload", NetworkError::AtCommandFailed());
    }

    if (content_.has_value() && supports_content && !content_->empty()) {
        size_t offset = 0;
        while (offset < content_->size()) {
            const size_t chunk = std::min(kMaxContentChunkBytes, content_->size() - offset);
            const bool final_chunk = offset + chunk == content_->size();
            // <eof>=0 closes content input. A fixed-length body is staged in
            // full before MHTTPREQUEST, so every piece except the last must
            // announce that more content follows.
            const std::string command = "AT+MHTTPCONTENT=" + std::to_string(id) +
                                        (final_chunk ? ",0," : ",1,") + std::to_string(chunk);
            const size_t timeout = TimeoutForBytes(*at_uart_, command.size() + chunk + 8, 5000);
            if (!at_uart_->SendCommandWithData(command, timeout, true, content_->data() + offset,
                                               chunk)) {
                return fail_open("request body upload", NetworkError::TransmitFailed());
            }
            offset += chunk;
            taskYIELD();
        }
    }
    content_.reset();

    if (!configure("AT+MHTTPCFG=\"encoding\"," + std::to_string(id) + ",1,1"))
        return fail_open("response encoding configuration", NetworkError::AtCommandFailed());

    const std::string request = "AT+MHTTPREQUEST=" + std::to_string(id) + "," +
                                std::to_string(method_value) + ",0," + at_uart_->EncodeHex(path_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        request_started_ = true;
    }
    if (!at_uart_->SendCommand(request, kRequestTimeoutMs))
        return fail_open("request submission", NetworkError::AtCommandFailed());

    if (request_chunked_) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool indicator =
            state_cv_.wait_for(lock, std::chrono::milliseconds(kIndicatorTimeoutMs),
                               [this] { return ind_received_ || error_ || cancelled_.load(); });
        if (!indicator || !ind_received_ || error_ || cancelled_.load()) {
            lock.unlock();
            return fail_open("chunked upload indicator", NetworkError::Timeout());
        }
    }
    ESP_LOGI(kTag, "HTTP %s %s started on slot %d", method_.c_str(), path_.c_str(), id);
    return {};
}

void DinosaurMl307Http::Close() {
    cancelled_.store(true);
    int id = -1;
    bool terminate = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        instance_active_.store(false);
        id = http_id_.exchange(-1);
        terminate = request_started_ && (!eof_ || (error_ && last_error_.load() < 0));
        request_started_ = false;
        eof_ = true;
        error_ = false;
        body_.clear();
        response_headers_.clear();
        headers_received_ = false;
        ind_received_ = false;
        header_fragment_active_ = false;
        header_fragment_status_ = 0;
        header_fragment_length_ = 0;
        header_fragment_data_.clear();
        state_cv_.notify_all();
    }
    if (id >= 0) {
        const std::string slot = std::to_string(id);
        bool uart_ready = true;
        if (terminate) {
            // The modem supports aborting an unfinished transfer while keeping
            // the instance allocated. Release that instance separately.
            const auto result = at_uart_->SendCommand("AT+MHTTPTERM=" + slot, kTerminateTimeoutMs);
            if (!result && result.error().code == AtErrc::Timeout)
                uart_ready = RecoverMl307AtParser(*at_uart_, kSyncTimeoutMs);
        }
        if (uart_ready) {
            const auto result = at_uart_->SendCommand("AT+MHTTPDEL=" + slot, kCloseTimeoutMs);
            if (!result && AtCommandCme(result) == 0) {
                // The next allocation must sweep the slot if the modem did
                // not release it. Drain a late response before returning the
                // shared UART to the caller, which may start MQTT immediately.
                s_http_reconcile_required.store(true);
                const auto& error = result.error();
                ESP_LOGW(kTag, "HTTP slot %d delete failed (code=%d cme=%d esp=%d)", id,
                         static_cast<int>(error.code), error.cme, static_cast<int>(error.esp));
                if (error.code == AtErrc::Timeout)
                    uart_ready = RecoverMl307AtParser(*at_uart_, kSyncTimeoutMs);
            }
        }
        if (!uart_ready) {
            s_http_reconcile_required.store(true);
            ESP_LOGW(kTag, "AT parser still unavailable after closing HTTP slot %d", id);
        }
        UnregisterHttpId(id);
    }
    ReleaseSlot();
}

void DinosaurMl307Http::ReleaseSlot() {
    if (!slot_acquired_.exchange(false))
        return;
    std::lock_guard<std::mutex> lock(s_http_slot_mutex);
    if (s_http_slots_in_use > 0)
        --s_http_slots_in_use;
    s_http_slot_cv.notify_one();
}

NetworkResult<int> DinosaurMl307Http::Read(char* buffer, size_t buffer_size) {
    if (buffer_size == 0)
        return 0;
    if (!buffer)
        return FailValue<int>(NetworkError::InvalidArgument());
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (error_.load())
        return FailValue<int>(CurrentError());
    if (eof_ && body_.empty())
        return 0;
    const bool ready = state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms_),
                                          [this] { return !body_.empty() || eof_ || error_; });
    if (!ready) {
        ESP_LOGE(kTag, "Timed out waiting for HTTP body (received=%u eof=%d)", body_offset_,
                 eof_ ? 1 : 0);
        return FailValue<int>(NetworkError::Timeout());
    }
    if (error_.load())
        return FailValue<int>(CurrentError());
    const size_t count = std::min(buffer_size, body_.size());
    if (count > 0) {
        std::memcpy(buffer, body_.data(), count);
        body_.erase(0, count);
    }
    return static_cast<int>(count);
}

NetworkResult<int> DinosaurMl307Http::Write(const char* buffer, size_t buffer_size) {
    if (!request_chunked_ || !instance_active_.load())
        return FailValue<int>(NetworkError::NotInitialized());
    const int id = http_id_.load();
    if (id < 0)
        return FailValue<int>(NetworkError::NotInitialized());

    if (buffer_size == 0) {
        auto result = at_uart_->SendCommand(
            "AT+MHTTPCONTENT=" + std::to_string(id) + ",0,2,\"0D0A\"", kConfigTimeoutMs);
        if (!result)
            return FailValue<int>(result.error().ToNetworkError());
        return 0;
    }
    if (!buffer)
        return FailValue<int>(NetworkError::InvalidArgument());

    size_t sent = 0;
    while (sent < buffer_size) {
        const size_t chunk = std::min(kMaxContentChunkBytes, buffer_size - sent);
        const std::string command =
            "AT+MHTTPCONTENT=" + std::to_string(id) + ",1," + std::to_string(chunk);
        const size_t timeout = TimeoutForBytes(*at_uart_, command.size() + chunk + 8, 5000);
        auto result = at_uart_->SendCommandWithData(command, timeout, true, buffer + sent, chunk);
        if (!result)
            return FailValue<int>(result.error().ToNetworkError());
        sent += chunk;
        taskYIELD();
    }
    return static_cast<int>(sent);
}

bool DinosaurMl307Http::WaitForHeadersLocked(std::unique_lock<std::mutex>& lock) {
    if (error_.load() || cancelled_.load())
        return false;
    if (headers_received_)
        return true;
    const bool ready = state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms_), [this] {
        return headers_received_ || error_ || cancelled_.load();
    });
    return ready && headers_received_ && !error_.load() && !cancelled_.load();
}

NetworkResult<int> DinosaurMl307Http::GetStatusCode() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (!WaitForHeadersLocked(lock)) {
        if (error_.load())
            return FailValue<int>(CurrentError());
        if (cancelled_.load())
            return FailValue<int>(NetworkError::NotInitialized());
        return FailValue<int>(NetworkError::Timeout());
    }
    return status_code_;
}

std::string DinosaurMl307Http::GetResponseHeader(const std::string& key) const {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto it = response_headers_.find(lower);
    return it == response_headers_.end() ? std::string() : it->second;
}

size_t DinosaurMl307Http::GetBodyLength() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (!WaitForHeadersLocked(lock))
        return 0;
    return content_length_;
}

std::string DinosaurMl307Http::ReadAll() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    const bool ready = state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms_),
                                          [this] { return eof_ || error_ || cancelled_.load(); });
    if (!ready) {
        ESP_LOGW(kTag, "Timed out waiting for complete HTTP response (received=%zu expected=%zu)",
                 body_offset_, content_length_);
        return {};
    }
    if (error_.load() || cancelled_.load())
        return {};
    if (body_.size() > kMaxReadAllBytes) {
        ESP_LOGE(kTag, "HTTP response exceeded ReadAll limit");
        return {};
    }
    return body_;
}

NetworkError DinosaurMl307Http::CurrentError() const {
    const int code = last_error_.load();
    if (code > 0)
        return NetworkError::FromMl307Http(code);
    return NetworkError::ReceiveFailed();
}

void DinosaurMl307Http::FailLocked(int error) {
    error_ = true;
    eof_ = true;
    last_error_.store(error);
    body_.clear();
    response_headers_.clear();
    header_fragment_active_ = false;
    header_fragment_status_ = 0;
    header_fragment_length_ = 0;
    header_fragment_data_.clear();
    state_cv_.notify_all();
}

void DinosaurMl307Http::FailBodyLocked(const char* reason) {
    ESP_LOGW(kTag, "Invalid HTTP body on slot %d: %s (received=%zu expected=%zu)", http_id_.load(),
             reason, body_offset_, content_length_);
    FailLocked(-1);
}

void DinosaurMl307Http::ResetResponseLocked() {
    status_code_ = -1;
    content_length_ = 0;
    body_offset_ = 0;
    body_.clear();
    response_headers_.clear();
    response_chunked_ = false;
    has_content_length_ = false;
    headers_received_ = false;
    body_forbidden_ = false;
    eof_ = false;
    error_ = false;
    ind_received_ = false;
    header_fragment_active_ = false;
    header_fragment_status_ = 0;
    header_fragment_length_ = 0;
    header_fragment_data_.clear();
}

void DinosaurMl307Http::HandleUrc(const std::string& command,
                                  const std::vector<AtArgumentValue>& arguments) {
    if (command == "FIFO_OVERFLOW") {
        // This callback runs on the AT receive task. Sending MHTTPDEL here
        // would wait for a response that this same task must parse.
        std::lock_guard<std::mutex> lock(state_mutex_);
        ESP_LOGW(kTag, "AT FIFO overflow invalidated HTTP slot %d", http_id_.load());
        FailLocked(-1);
        return;
    }

    if (command == "MHTTPCREATE") {
        int id = -1;
        if (arguments.size() != 1 || !IsInt(arguments[0]) || (id = arguments[0].int_value) < 0 ||
            id >= static_cast<int>(kHttpSlotCount)) {
            ESP_LOGW(kTag, "Malformed MHTTPCREATE URC");
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (awaiting_create_)
                FailLocked(-1);
            return;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!awaiting_create_ || instance_active_.load() || http_id_.load() >= 0) {
            ESP_LOGW(kTag, "Ignoring unexpected MHTTPCREATE id=%d", id);
            return;
        }
        RegisterHttpId(id);
        http_id_.store(id);
        instance_active_.store(true);
        state_cv_.notify_all();
        return;
    }

    if (command != "MHTTPURC" || arguments.size() < 2 || !IsString(arguments[0]) ||
        !IsInt(arguments[1]))
        return;
    const int id = arguments[1].int_value;
    if (id < 0 || id != http_id_.load())
        return;

    const std::string& type = arguments[0].string_value;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (http_id_.load() != id || error_.load())
        return;

    if (type == "header") {
        int status = 0;
        int header_length = -1;
        const std::string* encoded = nullptr;
        if (arguments.size() != 5 || !IsInt(arguments[2]) || !IsInt(arguments[3]) ||
            !IsHexValue(arguments[4])) {
            ESP_LOGE(kTag, "Malformed MHTTPURC header for slot %d", id);
            FailLocked(-1);
            return;
        }
        status = arguments[2].int_value;
        header_length = arguments[3].int_value;
        encoded = &arguments[4].string_value;
        if (status < 100 || status > 599 || header_length < 0 || !IsHexString(*encoded)) {
            ESP_LOGE(kTag, "Invalid MHTTPURC header fields for slot %d", id);
            FailLocked(-1);
            return;
        }
        std::string decoded;
        at_uart_->DecodeHexAppend(decoded, encoded->data(), encoded->size());
        std::string complete_headers;
        const auto fragment_result = AppendHeaderFragment(
            decoded, status, static_cast<size_t>(header_length), &complete_headers);
        if (fragment_result == HeaderFragmentResult::kInvalid) {
            ESP_LOGE(kTag, "Invalid or interleaved HTTP header fragment for slot %d", id);
            FailLocked(-1);
            return;
        }
        if (fragment_result == HeaderFragmentResult::kNeedMore) {
            ESP_LOGD(kTag, "Buffering fragment for HTTP header on slot %d", id);
            return;
        }
        int parsed_status = 0;
        bool chunked = false;
        bool has_length = false;
        size_t length = 0;
        std::map<std::string, std::string> parsed_headers;
        if (!ParseStatusHeader(complete_headers, &parsed_status, &chunked, &has_length, &length,
                               &parsed_headers) ||
            parsed_status != status) {
            ESP_LOGE(kTag, "Invalid HTTP response headers for slot %d", id);
            FailLocked(-1);
            return;
        }
        if (status >= 100 && status < 200) {
            // 100-continue and similar interim responses precede the final
            // header block. Keep waiting for the final response.
            if (status == 101) {
                FailLocked(-1);
                return;
            }
            response_chunked_ = false;
            has_content_length_ = false;
            content_length_ = 0;
            body_offset_ = 0;
            body_.clear();
            headers_received_ = false;
            body_forbidden_ = false;
            eof_ = false;
            state_cv_.notify_all();
            return;
        }

        status_code_ = status;
        response_chunked_ = chunked;
        has_content_length_ = has_length;
        content_length_ = length;
        response_headers_ = std::move(parsed_headers);
        headers_received_ = true;
        body_forbidden_ = method_ == "HEAD" || status == 204 || status == 304;
        if (body_forbidden_ || (has_length && length == 0))
            eof_ = true;
        state_cv_.notify_all();
        ESP_LOGI(kTag, "HTTP status %d on slot %d (chunked=%d length=%u)", status, id,
                 chunked ? 1 : 0, static_cast<unsigned>(length));
        return;
    }

    if (type == "content") {
        if (!headers_received_) {
            FailBodyLocked("content before headers");
            return;
        }
        if ((arguments.size() != 5 && arguments.size() != 6) || !IsInt(arguments[2]) ||
            !IsInt(arguments[3]) || !IsInt(arguments[4])) {
            FailBodyLocked("malformed content URC");
            return;
        }
        const int content_length = arguments[2].int_value;
        const int received_length = arguments[3].int_value;
        const int current_length = arguments[4].int_value;
        if (content_length < 0 || received_length < 0 || current_length < 0 ||
            current_length > received_length ||
            static_cast<size_t>(received_length) !=
                body_offset_ + static_cast<size_t>(current_length) ||
            (!response_chunked_ && has_content_length_ &&
             static_cast<size_t>(received_length) > content_length_)) {
            FailBodyLocked("inconsistent content length or offset");
            return;
        }
        std::string decoded;
        if (current_length > 0) {
            if (arguments.size() != 6 || !IsHexValue(arguments[5]) ||
                !IsHexString(arguments[5].string_value) ||
                arguments[5].string_value.size() != static_cast<size_t>(current_length) * 2) {
                FailBodyLocked("invalid hex payload");
                return;
            }
            at_uart_->DecodeHexAppend(decoded, arguments[5].string_value.data(),
                                      arguments[5].string_value.size());
            if (decoded.size() != static_cast<size_t>(current_length)) {
                FailBodyLocked("decoded payload length mismatch");
                return;
            }
        } else if (arguments.size() != 5 &&
                   (!IsString(arguments[5]) || !arguments[5].string_value.empty())) {
            FailBodyLocked("unexpected payload on terminator");
            return;
        }
        if (body_forbidden_ && current_length != 0) {
            FailBodyLocked("response cannot carry a body");
            return;
        }
        if (!body_forbidden_ && !response_chunked_ && has_content_length_ && current_length == 0 &&
            body_offset_ < content_length_) {
            FailBodyLocked("response ended before Content-Length");
            return;
        }
        if (!decoded.empty()) {
            if (decoded.size() >
                kMaxBufferedBodyBytes - std::min(body_.size(), kMaxBufferedBodyBytes)) {
                ESP_LOGE(kTag, "HTTP response buffer limit exceeded");
                FailLocked(-1);
                return;
            }
            body_.append(decoded);
        }
        body_offset_ = static_cast<size_t>(received_length);
        if (body_forbidden_) {
            eof_ = true;
        } else if (response_chunked_) {
            eof_ = current_length == 0;
        } else if (has_content_length_) {
            eof_ = body_offset_ >= content_length_;
        } else {
            eof_ = current_length == 0;
        }
        state_cv_.notify_all();
        return;
    }

    if (type == "err") {
        int modem_error = 0;
        if (arguments.size() != 3 || !IsInt(arguments[2])) {
            FailLocked(-1);
            return;
        }
        modem_error = arguments[2].int_value;
        ESP_LOGE(kTag, "HTTP request error on slot %d: %d", id, modem_error);
        FailLocked(modem_error);
        return;
    }

    if (type == "ind") {
        if (arguments.size() != 2) {
            FailLocked(-1);
            return;
        }
        ind_received_ = true;
        state_cv_.notify_all();
        return;
    }

    ESP_LOGW(kTag, "Unknown MHTTPURC type on slot %d", id);
}

#endif  // DINOSAUR_ENABLE_4G
