#ifndef WZH_USER_WEB_POLICY_H_
#define WZH_USER_WEB_POLICY_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace wzh {
// Caller serializes access. Tokens live in RAM only and are tied to an AP client.
class UserWebPolicy {
public:
    enum class LoginResult { kOk, kBadPassword, kRateLimited };
    enum class Network { kUnprovisioned, kConnecting, kOffline, kOnline };
    enum class Route {
        kSetupFirst,
        kSetupOffline,
        kSetupConnecting,
        kSetupSwitch,
        kLogin,
        kDetails,
        kManual,
        kDone,
        kRoot
    };
    static Route RouteFor(std::string_view path, Network network, bool session, bool completion) {
        if (path == "/done" || path == "/done.html")
            return completion ? Route::kDone : Route::kRoot;
        if (network != Network::kOnline) {
            if (network == Network::kUnprovisioned)
                return Route::kSetupFirst;
            return network == Network::kConnecting ? Route::kSetupConnecting : Route::kSetupOffline;
        }
        if (!session)
            return Route::kLogin;
        if (path == "/setup")
            return Route::kSetupSwitch;
        if (path == "/manual")
            return Route::kManual;
        return Route::kDetails;
    }
    static constexpr uint64_t kSessionMs = 30ULL * 60 * 1000;
    static constexpr uint64_t kCompletionMs = 10ULL * 60 * 1000;
    static constexpr uint64_t kLockoutMs = 60ULL * 1000;

    struct SetupError {
        const char* code;
        const char* message;
    };
    // Maps the station disconnect reason collected during a credential trial to
    // the user-facing setup error contract. Reason 0 means the trial timed out
    // without a disconnect event (or the driver rejected the request).
    static SetupError SetupFailure(int reason) {
        if (reason == -1)
            return {"SETUP_SAVE_FAILED", "保存网络配置失败，请重试"};
        if (reason == -2)
            return {"SETUP_TIMEOUT", "网络连接超时，请确认路由器已开启 2.4GHz 并重试"};
        switch (reason) {
            case 14:   // MIC_FAILURE
            case 15:   // 4WAY_HANDSHAKE_TIMEOUT
            case 23:   // 802_1X_AUTH_FAILED
            case 24:   // CIPHER_SUITE_REJECTED
            case 202:  // AUTH_FAIL
                return {"SETUP_BAD_PASSWORD", "密码不正确，请重新输入"};
            case 200:  // BEACON_TIMEOUT
            case 201:  // NO_AP_FOUND
                return {"SETUP_NOT_FOUND", "没有找到这个网络，请靠近路由器或刷新"};
            case 203:  // ASSOC_FAIL
            case 205:  // CONNECTION_FAIL
                return {"SETUP_REFUSED", "路由器拒绝了连接，请检查黑名单或连接数"};
            default:
                return {"SETUP_TIMEOUT", "连接超时，请确认路由器已开启 2.4GHz 并重试"};
        }
    }
    static bool IsApClient(uint32_t ipv4_host_order) {
        return (ipv4_host_order & 0xffffff00U) == 0xc0a80400U;
    }
    static bool ValidOrigin(std::string_view host, std::string_view origin) {
        return (host == "192.168.4.1" || host == "192.168.4.1:80") &&
               (origin == "http://192.168.4.1" || origin == "http://192.168.4.1:80");
    }
    static std::string Password(uint8_t ap_mac_4, uint8_t ap_mac_5) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string password = "zylz0000";
        password[4] = hex[ap_mac_4 >> 4];
        password[5] = hex[ap_mac_4 & 15];
        password[6] = hex[ap_mac_5 >> 4];
        password[7] = hex[ap_mac_5 & 15];
        return password;
    }
    LoginResult Login(uint32_t ip, std::string_view supplied, std::string_view expected,
                      const std::array<uint8_t, 16>& random, uint64_t now, std::string& token) {
        auto& attempt = FindAttempt(ip, now);
        if (attempt.locked_until > now)
            return LoginResult::kRateLimited;
        if (!Equal(supplied, expected)) {
            if (++attempt.failures >= 5) {
                attempt.failures = 0;
                attempt.locked_until = now + kLockoutMs;
                return LoginResult::kRateLimited;
            }
            return LoginResult::kBadPassword;
        }
        attempt.failures = 0;
        attempt.locked_until = 0;
        token = AddSession(ip, random, now);
        return LoginResult::kOk;
    }
    bool CheckSession(uint32_t ip, std::string_view token, uint64_t now) const {
        for (const auto& entry : sessions_)
            if (entry.active && entry.ip == ip && now < entry.expires && Equal(token, entry.token))
                return true;
        return false;
    }
    void Logout(uint32_t ip, std::string_view token) {
        for (auto& entry : sessions_)
            if (entry.active && entry.ip == ip && Equal(token, entry.token))
                entry.active = false;
    }
    std::string CreateCompletion(uint32_t ip, const std::array<uint8_t, 16>& random, uint64_t now) {
        completion_ = {ip, now + kCompletionMs, Hex(random), true};
        return completion_.token;
    }
    bool CheckCompletion(uint32_t ip, std::string_view token, uint64_t now) const {
        return completion_.active && completion_.ip == ip && now < completion_.expires &&
               Equal(token, completion_.token);
    }
    bool Enter(uint32_t ip, std::string_view completion, const std::array<uint8_t, 16>& random,
               uint64_t now, bool online, std::string& session) {
        if (!online || !CheckCompletion(ip, completion, now))
            return false;
        completion_.active = false;
        session = AddSession(ip, random, now);
        return true;
    }

private:
    struct Entry {
        uint32_t ip = 0;
        uint64_t expires = 0;
        std::string token;
        bool active = false;
    };
    struct Attempt {
        uint32_t ip = 0;
        unsigned failures = 0;
        uint64_t locked_until = 0;
        uint64_t used = 0;
    };
    std::array<Entry, 4> sessions_{};
    std::array<Attempt, 4> attempts_{};
    Entry completion_{};
    static bool Equal(std::string_view a, std::string_view b) {
        size_t diff = a.size() ^ b.size();
        const size_t count = a.size() > b.size() ? a.size() : b.size();
        for (size_t i = 0; i < count; ++i)
            diff |= (i < a.size() ? static_cast<unsigned char>(a[i]) : 0) ^
                    (i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
        return diff == 0;
    }
    static std::string Hex(const std::array<uint8_t, 16>& random) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string value(32, '0');
        for (size_t i = 0; i < random.size(); ++i) {
            value[2 * i] = hex[random[i] >> 4];
            value[2 * i + 1] = hex[random[i] & 15];
        }
        return value;
    }
    Attempt& FindAttempt(uint32_t ip, uint64_t now) {
        Attempt* oldest = &attempts_[0];
        for (auto& entry : attempts_) {
            if (entry.ip == ip) {
                entry.used = now;
                return entry;
            }
            if (entry.used < oldest->used)
                oldest = &entry;
        }
        *oldest = {ip, 0, 0, now};
        return *oldest;
    }
    std::string AddSession(uint32_t ip, const std::array<uint8_t, 16>& random, uint64_t now) {
        Entry* slot = &sessions_[0];
        for (auto& entry : sessions_) {
            if (!entry.active || entry.expires <= now) {
                slot = &entry;
                break;
            }
            if (entry.expires < slot->expires)
                slot = &entry;
        }
        *slot = {ip, now + kSessionMs, Hex(random), true};
        return slot->token;
    }
};
}  // namespace wzh
#endif
