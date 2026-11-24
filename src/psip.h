// hibiki ATSC PSIP decoder (A/65).
//
// FFmpeg's mpegts demuxer ignores PSIP, so the front-end taps the raw
// transport stream and feeds it here. From the base PID 0x1FFB we read the
// MGT (which lists the EIT/ETT PIDs), the terrestrial/cable VCT (virtual
// channel numbers and names, mapped to MPEG program_number), and the STT
// (GPS system time). EIT-k tables give each virtual channel's events and the
// ETTs their extended descriptions.
//
// Text uses the A/65 multiple_string_structure. Uncompressed Latin-1 / UTF-16
// segments are decoded; Huffman-compressed segments (compression_type 1/2,
// A/65 Annex C) are not yet supported and come back as "[COMPRESSED]".

#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace psip {

struct VirtualChannel {
    int major = 0, minor = 0;
    std::string name;          // short_name (7 UTF-16 chars), trimmed
    int program_number = 0;    // MPEG program carrying this channel
    int source_id = 0;         // key into the EIT/ETT
    int tsid = 0;
    int service_type = 0;      // 2 = ATSC digital TV, 3 = audio, 4 = data
    bool hidden = false, hide_guide = false;
    std::string description;   // channel ETT, if broadcast
};

struct Event {
    int source_id = 0;
    int event_id = 0;
    int64_t start = 0;         // UTC seconds since the Unix epoch
    int duration = 0;          // seconds
    std::string title;
    std::string description;   // from the event ETT
    std::string rating;        // content advisory (abbreviated), e.g. "TV-PG"
    int64_t end() const { return start + duration; }
};

class Decoder {
public:
    // Feed raw MPEG-TS bytes (any chunking, sync is recovered).
    void feed(const uint8_t* data, size_t n);
    // Forget everything (call after a retune).
    void reset();

    // Virtual channels, sorted by major/minor.
    const std::vector<VirtualChannel>& channels() const { return channels_; }
    const VirtualChannel* by_program(int program_number) const;
    const VirtualChannel* by_source(int source_id) const;
    // Events for one source, sorted by start, optionally dropping ones that
    // ended before `after`.
    std::vector<Event> events(int source_id, int64_t after = 0) const;
    // The event airing at `t` (nullptr if unknown) and the one after it.
    const Event* at(int source_id, int64_t t) const;
    const Event* next_after(int source_id, int64_t t) const;
    size_t event_count() const { return events_.size(); }

    // Broadcast time, if an STT has been seen.
    bool have_time() const { return stt_seen_; }
    int64_t now_utc() const;   // STT time advanced by the wall clock since
    int gps_utc_offset() const { return gps_utc_offset_; }

    // Bumped whenever decoded data changes (cheap change detection for UI).
    uint64_t generation() const { return generation_; }
    uint64_t sections_ok() const { return sections_ok_; }
    uint64_t crc_errors() const { return crc_errors_; }

    // Merge previously cached data (see cache_* in the front-end).
    void add_channel(const VirtualChannel& vc);
    void add_event(const Event& e);
    // Drop events that ended before `t`.
    void prune(int64_t t);

private:
    struct PidState {
        std::vector<uint8_t> buf;
        int cc = -1;
        bool active = false;
    };
    void packet(const uint8_t* p);
    void section_bytes(uint16_t pid, PidState& ps, const uint8_t* d, size_t n, bool start);
    void drain(uint16_t pid, PidState& ps);
    void section(uint16_t pid, const uint8_t* s, size_t n);
    void parse_mgt(const uint8_t* s, size_t n);
    void parse_vct(const uint8_t* s, size_t n);
    void parse_stt(const uint8_t* s, size_t n);
    void parse_eit(const uint8_t* s, size_t n);
    void parse_ett(const uint8_t* s, size_t n);
    void sort_channels();

    std::vector<uint8_t> buf_;                // unconsumed TS bytes between feeds
    bool synced_ = false;
    std::map<uint16_t, PidState> pids_;
    std::set<uint16_t> eit_pids_, ett_pids_;
    // (table_id, table_id_extension, section_number) -> version, to skip repeats
    std::map<std::tuple<int, int, int>, int> seen_;
    std::vector<VirtualChannel> channels_;
    std::map<std::pair<int, int>, Event> events_;  // (source_id, event_id)
    // ETT text that arrived before its event, keyed like events_.
    std::map<std::pair<int, int>, std::string> pending_ett_;
    bool stt_seen_ = false;
    int64_t stt_utc_ = 0;
    int64_t stt_wall_ = 0;
    int gps_utc_offset_ = 0;
    uint64_t generation_ = 0, sections_ok_ = 0, crc_errors_ = 0;
};

// MPEG-2 CRC-32 (poly 0x04C11DB7), as used by PSI/PSIP sections.
uint32_t crc32_mpeg(const uint8_t* d, size_t n);
// Decode an A/65 multiple_string_structure; prefers English ("eng").
std::string decode_mss(const uint8_t* d, size_t n);
// GPS seconds (since 1980-01-06) to Unix UTC.
int64_t gps_to_utc(uint32_t gps, int gps_utc_offset);

// Synthetic PSIP for --demo and tests: MGT, TVCT, STT, EIT-0..1 and ETTs for
// a few virtual channels on RF `rf`, with events around `now`. Returned as
// 188-byte TS packets.
std::vector<uint8_t> build_demo_ts(int rf, int64_t now);

} // namespace psip
