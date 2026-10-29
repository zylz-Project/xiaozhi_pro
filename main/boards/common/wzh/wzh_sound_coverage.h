#ifndef WZH_SOUND_COVERAGE_H_
#define WZH_SOUND_COVERAGE_H_
#include <array>
#include <cstdint>
#include <cstring>
#include "wzh_sound_score.h"
namespace wzh {
// Same 32-entry bound as the Flash TOC. Commit only after enqueue succeeds;
// stable filename hashes prevent reordering from losing coverage or variety.
class SoundCoverage {
public:
    template <class Files>
    int Select(const Files& files, uint32_t random, bool include_ambient = true,
               bool ambient_only = false) const {
        std::array<int, 32> unseen{}, all{};
        unsigned fresh = 0, count = 0;
        for (size_t i = 0; i < files.size() && count < all.size(); ++i) {
            const auto& f = files[i];
            if (!f.name[0] || !f.size)
                continue;
            const bool ambient = ClassifySoundKind(f.name, f.category) == SoundKind::kAmbient;
            if (ambient_only ? !ambient : (!include_ambient && ambient))
                continue;
            const uint32_t hash = Hash(f.name);
            bool seen = false;
            for (unsigned j = 0; j < used_; ++j)
                if (visited_[j] == hash)
                    seen = true;
            all[count++] = static_cast<int>(i);
            if (!seen)
                unseen[fresh++] = static_cast<int>(i);
        }
        if (!count)
            return -1;
        auto& pool = fresh ? unseen : all;
        unsigned n = fresh ? fresh : count;
        if (n > 1) {
            for (unsigned j = 0; j < n; ++j)
                if (Hash(files[pool[j]].name) == last_) {
                    pool[j] = pool[--n];
                    break;
                }
        }
        return pool[random % n];
    }
    template <class Files>
    void Remember(const Files& files, const char* name, bool include_ambient = true,
                  bool ambient_only = false) {
        bool all_seen = true;
        for (const auto& f : files) {
            if (!f.name[0] || !f.size)
                continue;
            const bool ambient = ClassifySoundKind(f.name, f.category) == SoundKind::kAmbient;
            if (ambient_only ? !ambient : (!include_ambient && ambient))
                continue;
            bool seen = false;
            for (unsigned j = 0; j < used_; ++j)
                if (visited_[j] == Hash(f.name))
                    seen = true;
            if (!seen)
                all_seen = false;
        }
        if (all_seen || used_ == visited_.size())
            used_ = 0;
        last_ = Hash(name);
        for (unsigned j = 0; j < used_; ++j)
            if (visited_[j] == last_)
                return;
        visited_[used_++] = last_;
    }

private:
    static uint32_t Hash(const char* s) {
        uint32_t h = 2166136261U;
        for (; *s; ++s)
            h = (h ^ static_cast<uint8_t>(*s)) * 16777619U;
        return h;
    }
    std::array<uint32_t, 32> visited_{};
    unsigned used_ = 0;
    uint32_t last_ = 0;
};
}  // namespace wzh
#endif
