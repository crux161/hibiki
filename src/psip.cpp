// hibiki ATSC PSIP decoder (see psip.h). Table layouts follow ATSC A/65.

#include "psip.h"

#include <algorithm>
#include <cstring>

namespace psip {

namespace {

constexpr uint16_t kBasePid = 0x1FFB;
constexpr int64_t kGpsEpochUnix = 315964800;  // 1980-01-06T00:00:00Z
constexpr size_t kMaxSection = 4096;          // A/65 private sections

enum : uint8_t {
    TID_MGT = 0xC7, TID_TVCT = 0xC8, TID_CVCT = 0xC9, TID_EIT = 0xCB,
    TID_ETT = 0xCC, TID_STT = 0xCD,
};

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

// One multiple_string_structure segment.
std::string decode_segment(int compression, int mode, const uint8_t* d, size_t n) {
    if (compression != 0) return "[COMPRESSED]";
    std::string out;
    if (mode == 0x3F) {                       // UTF-16BE
        for (size_t i = 0; i + 1 < n; i += 2) put_utf8(out, be16(d + i));
    } else if (mode <= 0x33) {                // byte = low 8 bits of UCS-2, mode = high
        for (size_t i = 0; i < n; ++i) put_utf8(out, ((uint32_t)mode << 8) | d[i]);
    }
    // 0x3E (SCSU) and reserved modes are skipped.
    return out;
}

} // namespace

uint32_t crc32_mpeg(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ d[i]) & 0xFF];
    return crc;
}

int64_t gps_to_utc(uint32_t gps, int gps_utc_offset) {
    return kGpsEpochUnix + (int64_t)gps - gps_utc_offset;
}

std::string decode_mss(const uint8_t* d, size_t n) {
    if (n < 1) return "";
    size_t p = 1;
    int nstr = d[0];
    std::string first, eng;
    for (int s = 0; s < nstr && p + 4 <= n; ++s) {
        std::string lang((const char*)d + p, 3);
        int nseg = d[p + 3];
        p += 4;
        std::string text;
        for (int g = 0; g < nseg && p + 3 <= n; ++g) {
            int comp = d[p], mode = d[p + 1], nb = d[p + 2];
            p += 3;
            if (p + nb > n) return trim(eng.empty() ? first : eng);
            text += decode_segment(comp, mode, d + p, (size_t)nb);
            p += nb;
        }
        if (s == 0) first = text;
        if (eng.empty() && (lang == "eng" || lang == "ENG")) eng = text;
    }
    return trim(eng.empty() ? first : eng);
}

// ---------------------------------------------------------------------------
void Decoder::reset() { *this = Decoder(); }

void Decoder::feed(const uint8_t* data, size_t n) {
    // Packets are 188 bytes starting with 0x47. Once in sync, trust the grid
    // and only check each sync byte; out of sync, require two consecutive
    // sync bytes so a 0x47 inside a payload (an ASCII 'G') is not taken.
    buf_.insert(buf_.end(), data, data + n);
    size_t i = 0;
    while (buf_.size() - i >= 188) {
        if (buf_[i] != 0x47) { synced_ = false; ++i; continue; }
        if (!synced_) {
            if (buf_.size() - i < 189) break;     // wait for the next sync byte
            if (buf_[i + 188] != 0x47) { ++i; continue; }
            synced_ = true;
        }
        packet(buf_.data() + i);
        i += 188;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (long)i);
}

void Decoder::packet(const uint8_t* p) {
    if (p[1] & 0x80) return;  // transport_error_indicator
    bool pusi = p[1] & 0x40;
    uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
    if (pid != kBasePid && !eit_pids_.count(pid) && !ett_pids_.count(pid)) return;
    int afc = (p[3] >> 4) & 3, cc = p[3] & 0x0F;
    if (!(afc & 1)) return;  // no payload
    size_t off = 4;
    if (afc & 2) off += 1 + p[4];
    if (off >= 188) return;

    PidState& ps = pids_[pid];
    if (ps.cc >= 0) {
        if (cc == ps.cc) return;                       // duplicate packet
        if (cc != ((ps.cc + 1) & 0x0F)) { ps.buf.clear(); ps.active = false; }
    }
    ps.cc = cc;
    section_bytes(pid, ps, p + off, 188 - off, pusi);
}

void Decoder::section_bytes(uint16_t pid, PidState& ps, const uint8_t* d, size_t n, bool start) {
    if (!start) {
        if (!ps.active) return;
        ps.buf.insert(ps.buf.end(), d, d + n);
        drain(pid, ps);
        return;
    }
    size_t ptr = d[0];
    if (1 + ptr > n) { ps.buf.clear(); ps.active = false; return; }
    if (ps.active && ptr) {   // tail of the previous section
        ps.buf.insert(ps.buf.end(), d + 1, d + 1 + ptr);
        drain(pid, ps);
    }
    ps.buf.assign(d + 1 + ptr, d + n);
    ps.active = true;
    drain(pid, ps);
}

void Decoder::drain(uint16_t pid, PidState& ps) {
    while (ps.active && ps.buf.size() >= 3) {
        if (ps.buf[0] == 0xFF) { ps.buf.clear(); ps.active = false; return; }  // stuffing
        size_t total = 3 + (size_t)(((ps.buf[1] & 0x0F) << 8) | ps.buf[2]);
        if (total > kMaxSection) { ps.buf.clear(); ps.active = false; return; }
        if (ps.buf.size() < total) return;
        std::vector<uint8_t> sec(ps.buf.begin(), ps.buf.begin() + (long)total);
        ps.buf.erase(ps.buf.begin(), ps.buf.begin() + (long)total);
        section(pid, sec.data(), sec.size());
    }
}

void Decoder::section(uint16_t pid, const uint8_t* s, size_t n) {
    if (n < 12 || !(s[1] & 0x80)) return;           // PSIP uses long-form sections
    if (crc32_mpeg(s, n) != 0) { ++crc_errors_; return; }
    if (!(s[5] & 1)) return;                         // not current_next
    ++sections_ok_;
    uint8_t tid = s[0];
    int ext = be16(s + 3), version = (s[5] >> 1) & 0x1F, secno = s[6];
    if (tid == TID_STT) { if (pid == kBasePid) parse_stt(s, n); return; }
    if (tid == TID_ETT && n >= 17) ext = (int)be32(s + 9);  // key ETTs by ETM_id
    auto key = std::make_tuple((int)pid * 256 + tid, ext, secno);
    auto it = seen_.find(key);
    if (it != seen_.end() && it->second == version) return;
    seen_[key] = version;
    switch (tid) {
    case TID_MGT:  if (pid == kBasePid) parse_mgt(s, n); break;
    case TID_TVCT:
    case TID_CVCT: if (pid == kBasePid) parse_vct(s, n); break;
    case TID_EIT:  if (eit_pids_.count(pid)) parse_eit(s, n); break;
    case TID_ETT:  parse_ett(s, n); break;
    default: break;
    }
}

void Decoder::parse_mgt(const uint8_t* s, size_t n) {
    const uint8_t* end = s + n - 4;
    const uint8_t* p = s + 9;            // after protocol_version
    if (p + 2 > end) return;
    int tables = be16(p);
    p += 2;
    std::set<uint16_t> eit, ett;
    for (int i = 0; i < tables && p + 11 <= end; ++i) {
        int type = be16(p);
        uint16_t pid = (uint16_t)(((p[2] & 0x1F) << 8) | p[3]);
        int dlen = ((p[9] & 0x0F) << 8) | p[10];
        if (type >= 0x0100 && type <= 0x017F) eit.insert(pid);
        else if ((type >= 0x0200 && type <= 0x027F) || type == 0x0004) ett.insert(pid);
        p += 11 + dlen;
    }
    if (eit != eit_pids_ || ett != ett_pids_) {
        for (uint16_t old : eit_pids_) if (!eit.count(old)) pids_.erase(old);
        for (uint16_t old : ett_pids_) if (!ett.count(old)) pids_.erase(old);
        eit_pids_ = eit;
        ett_pids_ = ett;
        ++generation_;
    }
}

void Decoder::parse_vct(const uint8_t* s, size_t n) {
    const uint8_t* end = s + n - 4;
    const uint8_t* p = s + 9;            // after protocol_version
    if (p + 1 > end) return;
    int count = *p++;
    for (int i = 0; i < count && p + 32 <= end; ++i) {
        VirtualChannel vc;
        std::string name;
        for (int c = 0; c < 7; ++c) {
            uint16_t u = be16(p + c * 2);
            if (!u) break;
            put_utf8(name, u);
        }
        vc.name = trim(name);
        vc.major = ((p[14] & 0x0F) << 6) | (p[15] >> 2);
        vc.minor = ((p[15] & 0x03) << 8) | p[16];
        vc.tsid = be16(p + 22);
        vc.program_number = be16(p + 24);
        vc.hidden = p[26] & 0x10;
        vc.hide_guide = p[26] & 0x02;
        vc.service_type = p[27] & 0x3F;
        vc.source_id = be16(p + 28);
        int dlen = ((p[30] & 0x03) << 8) | p[31];
        p += 32 + dlen;
        add_channel(vc);
    }
}

void Decoder::parse_stt(const uint8_t* s, size_t n) {
    if (n < 9 + 6 + 4) return;
    uint32_t gps = be32(s + 9);
    int offset = s[13];
    bool changed = !stt_seen_ || offset != gps_utc_offset_;
    gps_utc_offset_ = offset;
    stt_utc_ = gps_to_utc(gps, offset);
    stt_wall_ = (int64_t)std::time(nullptr);
    stt_seen_ = true;
    if (changed) ++generation_;
}

int64_t Decoder::now_utc() const {
    int64_t wall = (int64_t)std::time(nullptr);
    return stt_seen_ ? stt_utc_ + (wall - stt_wall_) : wall;
}

void Decoder::parse_eit(const uint8_t* s, size_t n) {
    const uint8_t* end = s + n - 4;
    int source = be16(s + 3);
    const uint8_t* p = s + 9;            // after protocol_version
    if (p + 1 > end) return;
    int count = *p++;
    for (int i = 0; i < count && p + 10 <= end; ++i) {
        Event e;
        e.source_id = source;
        e.event_id = ((p[0] & 0x3F) << 8) | p[1];
        e.start = gps_to_utc(be32(p + 2), gps_utc_offset_);
        e.duration = ((p[6] & 0x0F) << 16) | (p[7] << 8) | p[8];
        int tlen = p[9];
        if (p + 10 + tlen + 2 > end) return;
        e.title = decode_mss(p + 10, (size_t)tlen);
        const uint8_t* q = p + 10 + tlen;
        int dlen = ((q[0] & 0x0F) << 8) | q[1];
        const uint8_t* d = q + 2;
        const uint8_t* dend = std::min(end, d + dlen);
        while (d + 2 <= dend) {
            int tag = d[0], len = d[1];
            if (d + 2 + len > dend) break;
            if (tag == 0x87 && len >= 1) {   // content_advisory_descriptor
                const uint8_t* r = d + 2;
                const uint8_t* rend = d + 2 + len;
                int regions = r[0] & 0x3F;
                ++r;
                for (int k = 0; k < regions && r + 2 <= rend; ++k) {
                    int dims = r[1];
                    r += 2 + dims * 2;
                    if (r >= rend) break;
                    int rlen = (*r++) & 0x1F;   // reserved(3) + length(5)
                    if (r + rlen > rend) break;
                    std::string txt = decode_mss(r, (size_t)rlen);
                    if (!txt.empty() && e.rating.empty()) e.rating = txt;
                    r += rlen;
                }
            }
            d += 2 + len;
        }
        p = q + 2 + dlen;
        add_event(e);
    }
}

void Decoder::parse_ett(const uint8_t* s, size_t n) {
    if (n < 9 + 5 + 4) return;
    uint32_t etm = be32(s + 9);
    std::string text = decode_mss(s + 13, n - 4 - 13);
    int source = (int)(etm >> 16);
    if ((etm & 0x3) == 0x2) {            // event ETT
        int ev = (int)((etm >> 2) & 0x3FFF);
        auto it = events_.find({source, ev});
        if (it != events_.end()) {
            if (it->second.description != text) { it->second.description = text; ++generation_; }
        } else {
            pending_ett_[{source, ev}] = text;
        }
    } else if ((etm & 0xFFFF) == 0) {    // channel ETT
        for (auto& vc : channels_)
            if (vc.source_id == source && vc.description != text) { vc.description = text; ++generation_; }
    }
}

void Decoder::add_channel(const VirtualChannel& vc) {
    for (auto& c : channels_) {
        if ((c.major == vc.major && c.minor == vc.minor) || c.source_id == vc.source_id) {
            std::string desc = c.description;
            c = vc;
            if (c.description.empty()) c.description = desc;
            ++generation_;
            sort_channels();
            return;
        }
    }
    channels_.push_back(vc);
    ++generation_;
    sort_channels();
}

void Decoder::add_event(const Event& e) {
    Event ev = e;
    auto key = std::make_pair(e.source_id, e.event_id);
    auto it = events_.find(key);
    if (it != events_.end() && ev.description.empty()) ev.description = it->second.description;
    auto pend = pending_ett_.find(key);
    if (pend != pending_ett_.end()) { ev.description = pend->second; pending_ett_.erase(pend); }
    events_[key] = ev;
    ++generation_;
}

void Decoder::prune(int64_t t) {
    for (auto it = events_.begin(); it != events_.end();) {
        if (it->second.end() < t) it = events_.erase(it);
        else ++it;
    }
}

void Decoder::sort_channels() {
    std::sort(channels_.begin(), channels_.end(), [](const VirtualChannel& a, const VirtualChannel& b) {
        return a.major != b.major ? a.major < b.major : a.minor < b.minor;
    });
}

const VirtualChannel* Decoder::by_program(int program_number) const {
    for (const auto& c : channels_) if (c.program_number == program_number) return &c;
    return nullptr;
}

const VirtualChannel* Decoder::by_source(int source_id) const {
    for (const auto& c : channels_) if (c.source_id == source_id) return &c;
    return nullptr;
}

std::vector<Event> Decoder::events(int source_id, int64_t after) const {
    std::vector<Event> v;
    for (auto it = events_.lower_bound({source_id, 0});
         it != events_.end() && it->first.first == source_id; ++it)
        if (it->second.end() > after) v.push_back(it->second);
    std::sort(v.begin(), v.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
    return v;
}

const Event* Decoder::at(int source_id, int64_t t) const {
    for (auto it = events_.lower_bound({source_id, 0});
         it != events_.end() && it->first.first == source_id; ++it)
        if (it->second.start <= t && t < it->second.end()) return &it->second;
    return nullptr;
}

const Event* Decoder::next_after(int source_id, int64_t t) const {
    const Event* best = nullptr;
    for (auto it = events_.lower_bound({source_id, 0});
         it != events_.end() && it->first.first == source_id; ++it)
        if (it->second.start >= t && (!best || it->second.start < best->start)) best = &it->second;
    return best;
}

// ---------------------------------------------------------------------------
// Demo / test fixture: real PSIP sections, packetised into TS.
namespace {

struct Writer {
    std::vector<uint8_t> b;
    void u8(int v) { b.push_back((uint8_t)v); }
    void u16(int v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16((int)(v >> 16)); u16((int)(v & 0xFFFF)); }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

std::vector<uint8_t> mss(const std::string& s) {
    Writer w;
    w.u8(1);
    w.u8('e'); w.u8('n'); w.u8('g');
    w.u8(1);              // one segment
    w.u8(0); w.u8(0);     // uncompressed, Latin-1
    size_t n = std::min<size_t>(s.size(), 255);
    w.u8((int)n);
    for (size_t i = 0; i < n; ++i) w.u8((uint8_t)s[i]);
    return w.b;
}

std::vector<uint8_t> make_section(int tid, int ext, int version, const std::vector<uint8_t>& body) {
    Writer w;
    int len = 5 + (int)body.size() + 4;
    w.u8(tid);
    w.u8(0xF0 | ((len >> 8) & 0x0F));
    w.u8(len & 0xFF);
    w.u16(ext);
    w.u8(0xC1 | ((version & 0x1F) << 1));
    w.u8(0); w.u8(0);     // section 0 of 0
    w.bytes(body);
    uint32_t crc = crc32_mpeg(w.b.data(), w.b.size());
    w.u32(crc);
    return w.b;
}

void packetise(std::vector<uint8_t>& out, uint16_t pid, const std::vector<uint8_t>& sec, int& cc) {
    size_t pos = 0;
    bool first = true;
    while (pos < sec.size()) {
        uint8_t pkt[188];
        std::memset(pkt, 0xFF, sizeof(pkt));
        pkt[0] = 0x47;
        pkt[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        pkt[2] = (uint8_t)(pid & 0xFF);
        pkt[3] = (uint8_t)(0x10 | (cc & 0x0F));
        cc = (cc + 1) & 0x0F;
        size_t off = 4;
        if (first) pkt[off++] = 0;  // pointer_field
        size_t take = std::min(sec.size() - pos, 188 - off);
        std::memcpy(pkt + off, sec.data() + pos, take);
        pos += take;
        first = false;
        out.insert(out.end(), pkt, pkt + 188);
    }
}

} // namespace

std::vector<uint8_t> build_demo_ts(int rf, int64_t now) {
    const uint16_t EIT0 = 0x1D00, EIT1 = 0x1D01, ETT0 = 0x1E00, ETT1 = 0x1E01, CETT = 0x1E80;
    const int offset = 18;
    auto gps = [&](int64_t utc) { return (uint32_t)(utc - kGpsEpochUnix + offset); };

    struct Ch { int minor; const char* name; const char* desc; };
    static const char* kCalls[] = {"KHBK", "KVCR", "WTAP", "KOSD", "WBLU", "KPLY", "KRWD"};
    std::string call = kCalls[rf % 7];
    const Ch chans[] = {
        {1, "-DT", "HIBIKI COMMUNITY TELEVISION"},
        {2, "-SD", "CLASSIC REPEATS ALL DAY"},
        {3, "RETRO", "VINTAGE MOVIES AND SERIALS"},
    };
    static const char* kTitles[][8] = {
        {"EVENING NEWS", "WEATHER WATCH", "QUIZ NIGHT", "THE LATE SHOW",
         "LOCAL SPORTS", "MIDNIGHT MOVIE", "OVERNIGHT NEWS", "TEST PATTERN"},
        {"SITCOM REPEATS", "SITCOM REPEATS", "DETECTIVE HOUR", "WESTERN THEATER",
         "COOKING CLASSICS", "GAME SHOW GOLD", "INFOMERCIAL", "SIGN OFF"},
        {"SATURDAY SERIAL", "CREATURE FEATURE", "SPACE PATROL", "FILM NOIR",
         "CARTOON CLASSICS", "SCI-FI DOUBLE", "SILENT CINEMA", "STATION ID"},
    };
    static const char* kRatings[] = {"TV-G", "TV-PG", "TV-14"};

    std::vector<uint8_t> ts;
    int cc_base = 0, cc_eit0 = 0, cc_eit1 = 0, cc_ett0 = 0, cc_ett1 = 0, cc_cett = 0;

    // MGT
    {
        Writer w;
        w.u8(0);            // protocol_version
        struct T { int type; uint16_t pid; } tables[] = {
            {0x0000, kBasePid}, {0x0100, EIT0}, {0x0101, EIT1},
            {0x0200, ETT0}, {0x0201, ETT1}, {0x0004, CETT}};
        w.u16(6);
        for (const auto& t : tables) {
            w.u16(t.type);
            w.u16(0xE000 | t.pid);
            w.u8(0xE0);     // version 0
            w.u32(0);       // number_bytes (unused here)
            w.u16(0xF000);  // no descriptors
        }
        w.u16(0xF000);      // no global descriptors
        packetise(ts, kBasePid, make_section(TID_MGT, 0, 0, w.b), cc_base);
    }
    // TVCT
    {
        Writer w;
        w.u8(0);
        w.u8(3);
        for (int i = 0; i < 3; ++i) {
            std::string name = chans[i].name[0] == '-' ? call + chans[i].name : chans[i].name;
            for (int c = 0; c < 7; ++c) w.u16(c < (int)name.size() ? (uint8_t)name[c] : 0);
            int major = rf, minor = chans[i].minor;
            w.u8(0xF0 | ((major >> 6) & 0x0F));
            w.u8(((major & 0x3F) << 2) | ((minor >> 8) & 0x03));
            w.u8(minor & 0xFF);
            w.u8(0x04);                 // 8-VSB
            w.u32(0);                   // carrier_frequency (deprecated)
            w.u16(0x0800 + rf);         // channel_TSID
            w.u16(i + 1);               // program_number
            w.u8(0x40 | 0x0D);          // ETM_location=1, reserved bits
            w.u8(0xC0 | 0x02);          // service_type 2 (digital TV)
            w.u16(i + 1);               // source_id
            w.u16(0xFC00);              // descriptors_length 0
        }
        w.u16(0xFC00);
        packetise(ts, kBasePid, make_section(TID_TVCT, 0x0800 + rf, 0, w.b), cc_base);
    }
    // STT
    {
        Writer w;
        w.u8(0);
        w.u32(gps(now));
        w.u8(offset);
        w.u16(0x6000);
        packetise(ts, kBasePid, make_section(TID_STT, 0, 0, w.b), cc_base);
    }
    // Channel ETT for the first channel
    {
        Writer w;
        w.u8(0);
        w.u32((uint32_t)1 << 16);
        w.bytes(mss(chans[0].desc));
        packetise(ts, CETT, make_section(TID_ETT, 0, 0, w.b), cc_cett);
    }
    // EIT-0 (now .. +3 h) and EIT-1 (+3 h .. +6 h), half-hour grid from -30 min.
    int64_t slot0 = now - (now % 1800) - 1800;
    for (int table = 0; table < 2; ++table) {
        for (int i = 0; i < 3; ++i) {
            int source = i + 1;
            Writer w;
            w.u8(0);
            w.u8(4);
            for (int k = 0; k < 4; ++k) {
                int idx = table * 4 + k;
                int eid = 100 * source + idx;
                int64_t start = slot0 + idx * 1800 + (idx > 3 ? 1800 * (idx - 3) : 0);
                int dur = idx >= 3 ? 3600 : 1800;
                w.u16(0xC000 | eid);
                w.u32(gps(start));
                w.u8(0xC0 | (1 << 4) | ((dur >> 16) & 0x0F));   // ETM in PTC
                w.u16(dur & 0xFFFF);
                std::vector<uint8_t> t = mss(kTitles[i][idx]);
                w.u8((int)t.size());
                w.bytes(t);
                // content_advisory_descriptor: one region, no dims, text rating
                std::vector<uint8_t> r = mss(kRatings[(idx + i) % 3]);
                Writer d;
                d.u8(0x87);
                d.u8(1 + 2 + 1 + (int)r.size());
                d.u8(0xC1);         // 1 region
                d.u8(1);            // rating_region (US)
                d.u8(0);            // rated_dimensions
                d.u8(0xE0 | (int)r.size());
                d.bytes(r);
                w.u16(0xF000 | (int)d.b.size());
                w.bytes(d.b);
            }
            packetise(ts, table ? EIT1 : EIT0, make_section(TID_EIT, source, 0, w.b),
                      table ? cc_eit1 : cc_eit0);
            // ETTs for the same events
            for (int k = 0; k < 4; ++k) {
                int idx = table * 4 + k;
                int eid = 100 * source + idx;
                Writer e;
                e.u8(0);
                e.u32(((uint32_t)source << 16) | ((uint32_t)eid << 2) | 0x2);
                std::string desc = std::string(kTitles[i][idx]) +
                    ". A DEMO LISTING GENERATED BY HIBIKI TO EXERCISE THE PSIP"
                    " EIT AND ETT DECODER, SHOWN IN THE VCR GUIDE.";
                e.bytes(mss(desc));
                packetise(ts, table ? ETT1 : ETT0, make_section(TID_ETT, eid, 0, e.b),
                          table ? cc_ett1 : cc_ett0);
            }
        }
    }
    return ts;
}

} // namespace psip
