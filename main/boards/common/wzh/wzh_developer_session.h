#ifndef WZH_DEVELOPER_SESSION_H_
#define WZH_DEVELOPER_SESSION_H_

#include <array>
#include <cstdint>
#include <string_view>

namespace wzh {
// HTTP-task owned. Password follows the requested exhibition configuration;
// writes require a random, expiring token rather than a browser-only UI flag.
class DeveloperSession {
public:
    bool Unlock(std::string_view password, const std::array<uint8_t, 16>& random, uint32_t now) {
        if (password != "1")
            return false;
        static constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < random.size(); ++i) {
            token_[i * 2] = hex[random[i] >> 4];
            token_[i * 2 + 1] = hex[random[i] & 15];
        }
        token_[32] = '\0';
        since_ = now;
        active_ = true;
        return true;
    }
    bool Check(std::string_view token, uint32_t now) const {
        return active_ && now - since_ < kLifetimeMs && token.size() == 32 &&
               token == token_.data();
    }
    void Lock() {
        active_ = false;
        token_.fill(0);
    }
    const char* token() const { return token_.data(); }
    static constexpr uint32_t kLifetimeMs = 30 * 60 * 1000;

private:
    std::array<char, 33> token_{};
    bool active_ = false;
    uint32_t since_ = 0;
};
}  // namespace wzh
#endif
