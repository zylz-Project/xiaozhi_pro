#ifndef WZH_AUDIO_STORE_H_
#define WZH_AUDIO_STORE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "wzh_flash.h"
#include "wzh_product.h"

namespace wzh {

// PNDA 目录的板端表示。offset 相对数据区起始地址（不是整颗 Flash）。
struct AudioFileInfo {
    char name[64];
    uint32_t offset;
    uint32_t size;
    uint32_t sample_rate;
    uint32_t duration_ms;
    char category[16];  // "animal" / "ambient"
};

// PNDA 音频仓库：三产品共用的外部 Flash 目录与数据管理。
//
// 兼容性：读取支持老工程的 TOC v1/v2（单份、无 CRC，位于 TOC 区第 0 槽），
// 现场已烧录默认声音的设备可直接升级。
// 写入使用 TOC v3：32 字节头（magic + seq + crc32）+ 96 字节条目；NAND 在
// TOC 块内按 4KB 槽滚动提交，新目录整页回读校验通过前旧目录始终有效，
// 写入中途断电不会丢失全部声音。NOR 只有单槽（每次擦写 TOC 扇区），
// 断电窗口由 CRC 检出，可通过擦除 + 重新同步恢复。
//
// 并发：目录查询与快照线程安全；写入/删除/擦除串行化（writing_ 标志）。
// 已提交文件的数据区一旦写入就不再原地改写（替换=追加新文件），因此
// 播放方可以不经锁直接按 offset 读取。
class AudioStore {
public:
    enum class Layout {
        kEmpty,   // 工厂状态（无有效目录）
        kLegacy,  // 老工程 v1/v2 目录
        kV3,      // 本工程 v3 目录
    };

    // 初始化 Flash（JEDEC 探测）并加载目录。失败返回 false（设备仍可
    // 无声运行，动作引擎不受影响）。
    bool Init();

    int file_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(files_.size());
    }
    // 在互斥锁内拷贝条目，避免同步任务改写目录时读到半新半旧的数据。
    bool GetFile(int index, AudioFileInfo* out) const;
    // 返回目录内部条目指针（不拷贝）。底层存储在 Init 时一次性预留、
    // 之后原地替换，因此指针长期有效；内容可能被并发的目录更新改写
    // （最坏情况是日志里出现一次 torn name）。供 SoundName 这类需要
    // 返回 const char* 的接口使用。
    const AudioFileInfo* PeekFile(int index) const;
    std::vector<AudioFileInfo> Snapshot() const;
    int FindByName(const std::string& name) const;
    int RandomInCategory(const std::string& category) const;
    int CountInCategory(const std::string& category) const;

    uint32_t data_start() const { return data_start_; }
    uint64_t capacity() const { return capacity_; }
    uint64_t used_bytes() const;
    uint64_t free_bytes() const;
    Layout layout() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return layout_;
    }
    bool IsWriting() const { return writing_.load(); }

    // 流式写入一个文件。reader 逐次填充缓冲并返回实际字节数：0 表示
    // 正常结束，SIZE_MAX 表示失败中止。任何失败都不会提交目录（数据
    // 成为无害孤儿，下次写操作覆盖）。成功后立即刷新目录。
    // 同名文件以追加新数据、提交时替换条目的方式更新，失败保留旧条目。
    // 网络读取期间不持有目录锁，查询/触摸无需等待整次下载。
    // 头部会校验 OggS + OpusHead 标记，拒绝非 OGG/Opus 内容。
    bool WriteFile(const std::string& name, uint32_t size, uint32_t sample_rate,
                   uint32_t duration_ms, const std::string& category,
                   const std::function<size_t(uint8_t*, size_t)>& reader,
                   std::string* error = nullptr);
    // 删除目录条目（数据成为孤儿，不做空间回收）。
    bool DeleteFile(const std::string& name, std::string* error = nullptr);
    // 擦除 TOC 区与全部数据区（NAND 上可能耗时 30 秒以上，请用异步包装）。
    bool EraseAll(std::string* error = nullptr);
    // 在独立任务中执行 EraseAll，立即返回。on_done 在任务末尾被调用。
    bool EraseAllAsync(std::function<void(bool)> on_done);

private:
    static constexpr int kMaxFiles = 32;
    static constexpr size_t kEntrySize = 96;
    static constexpr uint32_t kTocSlotSize = 4096;
    static constexpr uint32_t kTocV3HeaderSize = 32;
    // 'W''Z''H''3' 的小端 u32，与老工程 0x41444E50 ("PNDA") 区分开。
    static constexpr uint32_t kTocV3Magic = 0x33485A57;

    struct TocHeaderV3 {
        uint32_t magic;
        uint32_t version;
        uint32_t seq;
        uint32_t count;
        uint32_t crc;
    };

    bool ReadFlash(uint32_t addr, void* data, size_t size);
    uint64_t UsedBytesLocked() const;
    // 解析一个 4KB TOC 槽；成功时填充条目并返回 seq。
    bool TryParseV3(const uint8_t* slot, uint32_t* seq, std::vector<AudioFileInfo>* out) const;
    bool TryParseLegacy(const uint8_t* slot, std::vector<AudioFileInfo>* out) const;
    bool LoadToc();
    // 用 entries 的内容重写目录（v3 双槽/单槽提交）。成功后更新 files_。
    bool CommitToc(const std::vector<AudioFileInfo>& entries);
    bool SlotIsVirgin(uint32_t slot_index);
    bool ProgramWithVerify(uint32_t addr, const uint8_t* data, size_t size);

    mutable std::mutex mutex_;
    std::vector<AudioFileInfo> files_;  // Init 时 reserve(kMaxFiles)，之后不扩容
    Layout layout_ = Layout::kEmpty;
    uint32_t toc_slot_count_ = 1;
    int32_t current_slot_ = -1;  // 当前有效 v3 槽
    uint32_t toc_seq_ = 0;
    uint32_t data_start_ = 0;
    uint64_t capacity_ = 0;
    uint32_t erase_size_ = 0;
    uint32_t program_size_ = 0;
    std::atomic<bool> writing_{false};
};

}  // namespace wzh

#endif  // WZH_AUDIO_STORE_H_
