// PSIP decoder tests: feed the synthetic PSIP stream through the decoder in
// awkward chunk sizes (with leading garbage and a corrupted section) and check
// what comes out. Run via `ctest` or ./build/psip_test.

#include "../src/psip.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    const int64_t now = 1795600000;   // 2026-11-24-ish, fixed for repeatability
    std::vector<uint8_t> ts = psip::build_demo_ts(11, now);
    CHECK(ts.size() % 188 == 0);

    // CRC: a valid section checks to 0 including its CRC.
    const uint8_t pat[] = {0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00,
                           0x00, 0x01, 0xE1, 0x00, 0, 0, 0, 0};
    uint8_t buf[sizeof(pat)];
    std::memcpy(buf, pat, sizeof(pat));
    uint32_t c = psip::crc32_mpeg(buf, 12);
    buf[12] = (uint8_t)(c >> 24); buf[13] = (uint8_t)(c >> 16);
    buf[14] = (uint8_t)(c >> 8);  buf[15] = (uint8_t)c;
    CHECK(psip::crc32_mpeg(buf, 16) == 0);

    // Stream: garbage, the fixture, then feed it in uneven chunks.
    std::vector<uint8_t> stream = {0x12, 0x47, 0x00, 0x99};
    stream.insert(stream.end(), ts.begin(), ts.end());
    psip::Decoder d;
    size_t pos = 0, step = 1;
    while (pos < stream.size()) {
        size_t n = std::min(step, stream.size() - pos);
        d.feed(stream.data() + pos, n);
        pos += n;
        step = step * 7 % 401 + 1;
    }

    CHECK(d.crc_errors() == 0);
    CHECK(d.have_time());
    CHECK(d.gps_utc_offset() == 18);
    CHECK(d.now_utc() >= now && d.now_utc() <= now + 5);

    const auto& ch = d.channels();
    CHECK(ch.size() == 3);
    if (ch.size() == 3) {
        CHECK(ch[0].major == 11 && ch[0].minor == 1);
        CHECK(ch[0].name == "WBLU-DT");
        CHECK(ch[2].name == "RETRO");
        CHECK(ch[1].program_number == 2);
        CHECK(ch[0].description == "HIBIKI COMMUNITY TELEVISION");
    }
    CHECK(d.by_program(3) && d.by_program(3)->source_id == 3);

    CHECK(d.event_count() == 24);
    auto ev = d.events(1);
    CHECK(ev.size() == 8);
    for (size_t i = 1; i < ev.size(); ++i) CHECK(ev[i].start == ev[i - 1].end());
    const psip::Event* on = d.at(1, now);
    CHECK(on != nullptr);
    if (on) {
        CHECK(on->title == "WEATHER WATCH");
        CHECK(on->description.rfind("WEATHER WATCH. A DEMO", 0) == 0);
        CHECK(on->rating == "TV-PG");
        CHECK(on->start <= now && now < on->end());
    }
    const psip::Event* nx = on ? d.next_after(1, on->end()) : nullptr;
    CHECK(nx && nx->title == "QUIZ NIGHT");

    // Re-feeding the same data changes nothing (versions unchanged).
    uint64_t gen = d.generation();
    psip::Decoder d2 = d;
    d2.feed(ts.data(), ts.size());
    CHECK(d2.event_count() == 24);
    (void)gen;

    // A corrupted section is counted and ignored.
    std::vector<uint8_t> bad = psip::build_demo_ts(12, now);
    bad[188 + 20] ^= 0xFF;   // inside the TVCT section (2nd packet)
    psip::Decoder d3;
    d3.feed(bad.data(), bad.size());
    CHECK(d3.crc_errors() >= 1);
    CHECK(d3.channels().empty());

    // Compressed (Huffman) text is flagged, not garbled.
    const uint8_t huff[] = {1, 'e', 'n', 'g', 1, 0x01, 0x00, 2, 0xAB, 0xCD};
    CHECK(psip::decode_mss(huff, sizeof(huff)) == "[COMPRESSED]");
    // UTF-16 mode 0x3F.
    const uint8_t u16[] = {1, 'e', 'n', 'g', 1, 0x00, 0x3F, 4, 0x00, 'H', 0x00, 'I'};
    CHECK(psip::decode_mss(u16, sizeof(u16)) == "HI");

    // prune drops finished events.
    d.prune(now + 4 * 3600);
    CHECK(d.events(1).size() < 8);

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("psip_test: all checks passed\n");
    return 0;
}
