#ifndef WZH_DINOSAUR_ML307_HTTP_H_
#define WZH_DINOSAUR_ML307_HTTP_H_

#include <atomic>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <at_uart.h>
#include <http.h>

// ML307 HTTP transport owned by the dinosaur board.
//
// The component driver keeps the complete response in RAM, indexes URC
// arguments without checking them, and closes the modem slot from the AT
// receive callback on FIFO overflow. This implementation keeps callbacks
// side-effect free, validates every URC, bounds response buffering, and lets
// OTA/assets callers stream a response with Read().
class DinosaurMl307Http final : public Http {
public:
    explicit DinosaurMl307Http(std::shared_ptr<AtUart> at_uart);
    ~DinosaurMl307Http() override;

    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    void SetKeepAlive(bool enable) override;
    NetworkResult<> Open(const std::string& method, const std::string& url) override;
    void Close() override;
    NetworkResult<int> Read(char* buffer, size_t buffer_size) override;
    NetworkResult<int> Write(const char* buffer, size_t buffer_size) override;
    NetworkResult<int> GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;

private:
    enum class HeaderFragmentResult { kInvalid, kNeedMore, kComplete };

    struct HeaderEntry {
        std::string key;
        std::string value;
    };

    bool ParseUrl(const std::string& url);
    bool BuildRequestHeaders(std::vector<HeaderEntry>* headers);
    HeaderFragmentResult AppendHeaderFragment(const std::string& decoded, int status,
                                              size_t header_length, std::string* complete_headers);
    bool WaitForHeadersLocked(std::unique_lock<std::mutex>& lock);
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments);
    void FailLocked(int error);
    NetworkError CurrentError() const;
    void ResetResponseLocked();
    void ReleaseSlot();

    std::shared_ptr<AtUart> at_uart_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    std::mutex open_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::map<std::string, std::string> request_headers_;
    std::optional<std::string> content_;
    std::string method_;
    std::string origin_;
    std::string path_;
    bool secure_ = false;
    bool request_chunked_ = false;
    bool response_chunked_ = false;
    bool has_content_length_ = false;
    bool headers_received_ = false;
    bool body_forbidden_ = false;
    bool eof_ = false;
    std::atomic<bool> error_{false};
    bool ind_received_ = false;
    bool awaiting_create_ = false;
    int timeout_ms_ = 30000;
    int status_code_ = -1;
    std::atomic<int> last_error_{0};
    size_t content_length_ = 0;
    size_t body_offset_ = 0;
    std::string body_;
    std::map<std::string, std::string> response_headers_;
    bool header_fragment_active_ = false;
    int header_fragment_status_ = 0;
    size_t header_fragment_length_ = 0;
    std::string header_fragment_data_;
    std::atomic<int> http_id_{-1};
    std::atomic<bool> instance_active_{false};
    std::atomic<bool> slot_acquired_{false};
    std::atomic<bool> cancelled_{false};
};

#endif  // WZH_DINOSAUR_ML307_HTTP_H_
