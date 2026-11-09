#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <mutex>
#include <string>
#include <vector>

struct SsidItem {
    std::string ssid;
    std::string password;
};

class SsidManager {
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string& ssid, const std::string& password);
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    bool AddSsidChecked(const std::string& ssid, const std::string& password);
    bool ClearChecked();
    bool RestoreListChecked(const std::vector<SsidItem>& snapshot);
    std::vector<SsidItem> GetSsidList() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ssid_list_;
    }

private:
    SsidManager();
    ~SsidManager();

    void LoadFromNvs();
    void SaveToNvs();
    bool SaveToNvsChecked();
    bool WritePortalBackup(const std::vector<SsidItem>& snapshot);
    bool ClearPortalBackup();
    bool RecoverPortalBackup();
    bool SavePortalTransaction(const std::vector<SsidItem>& fallback);

    mutable std::mutex mutex_;
    std::vector<SsidItem> ssid_list_;
};

#endif  // SSID_MANAGER_H
