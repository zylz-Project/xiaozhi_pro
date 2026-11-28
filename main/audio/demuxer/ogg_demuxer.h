#ifndef OGG_DEMUXER_H_
#define OGG_DEMUXER_H_

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include "opus_stream_metadata.h"

class OggDemuxer {
private:
    enum ParseState : int8_t { FIND_PAGE, PARSE_HEADER, PARSE_SEGMENTS, PARSE_DATA };

    struct Opus_t {
        bool head_seen{false};
        bool tags_seen{false};
        int sample_rate{48000};
    };

    // 使用固定大小的缓冲区避免动态分配
    struct context_t {
        bool packet_continued{false};  // 当前包是否跨多个段
        uint8_t header[27];            // Ogg页头
        uint8_t seg_table[255];        // 当前存储的段表
        uint8_t packet_buf[8192];      // 8KB包缓冲区
        size_t packet_len = 0;         // 缓冲区中累计的数据长度
        size_t seg_count = 0;          // 当前页段数
        size_t seg_index = 0;          // 当前处理的段索引
        size_t data_offset = 0;        // 解析当前阶段已读取的字节数
        size_t bytes_needed = 0;       // 解析当前字段还需要读取的字节数
        size_t seg_remaining = 0;      // 当前段剩余需要读取的字节数
        size_t body_size = 0;          // 数据体总大小
        size_t body_offset = 0;        // 数据体已读取的字节数
    };

public:
    explicit OggDemuxer(bool verify_pages = false) : verify_pages_(verify_pages) { Reset(); }

    // Enhanced callback uses the normative 48 kHz Opus timeline, not the
    // informational original input rate stored in OpusHead.
    void OnOpusPacket(std::function<void(const uint8_t*, size_t, const OpusStreamMetadata&)> cb) {
        on_opus_packet_ = std::move(cb);
    }
    bool valid_stream() const { return enhanced_valid_; }
    bool AtVerifiedPageBoundary() const;
    bool saw_end_of_stream() const { return verified_eos_; }
    void Reset();

    size_t Process(const uint8_t* data, size_t size);

    /// @brief 设置解封装完毕后回调处理函数
    /// @param on_demuxer_finished
    void OnDemuxerFinished(
        std::function<void(const uint8_t* data, int sample_rate, size_t len)> on_demuxer_finished) {
        on_demuxer_finished_ = on_demuxer_finished;
    }

private:
    std::function<void(const uint8_t*, size_t, const OpusStreamMetadata&)> on_opus_packet_;
    bool enhanced_valid_ = false, first_audio_packet_ = true;
    uint32_t preskip_remaining_ = 0;
    uint64_t decoded_48k_ = 0;
    int16_t gain_q8_ = 0;
    void ParseOpusHeader();
    void EmitOpusPacket();
    bool BeginVerifiedPage();
    void AccumulateChecksum(const uint8_t* bytes, size_t size);
    void EndVerifiedPage();
    const bool verify_pages_;
    bool page_error_ = false, verified_eos_ = false, saw_page_ = false;
    uint32_t serial_ = 0, next_sequence_ = 0, checksum_ = 0;
    ParseState state_ = ParseState::FIND_PAGE;
    context_t ctx_;
    Opus_t opus_info_;
    std::function<void(const uint8_t*, int, size_t)> on_demuxer_finished_;
};

#endif
