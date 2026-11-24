// hibiki SDL front-end - live ATSC TV with video + audio + a VCR-style OSD.
//
// Pipeline: hibiki::Tuner (async TS reader) -> custom FFmpeg AVIO -> libavformat
// demux -> libavcodec decode -> SDL3 YUV texture (video) / libswresample ->
// SDL3 audio stream (audio). The on-screen display (osd.h) draws a VHS-style
// playback HUD and a blue VCR set-up menu over the picture.
//
// Playback lives in player.{h,cpp}: a demux thread feeds packet queues and
// the main loop decodes, keeps audio as the master clock and shows each video
// frame when its PTS comes due. The screen is redrawn per frame or at least
// every ~33 ms, so menus stay responsive with no picture. The raw TS is also
// handed to the PSIP decoder for channel names and the guide.
//
// Remote-style controls (see usage()):
//   Enter menu, Esc back/quit, Left/Right or PgUp/PgDn channel, Up/Down volume,
//   0-9 direct channel entry, C channel list, G guide, A audio track,
//   Z screen scale, I signal info, H basic HUD, F fullscreen, M mute, S scan.

#include "hibiki/tuner.h"
#include "hibiki/firmware.h"
#include "hibiki/app_menu.h"
#include "osd.h"
#include "player.h"
#include "psip.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* DEFAULT_FONT = "/System/Library/Fonts/Supplemental/Arial.ttf";
const char* VERSION = HIBIKI_VERSION;

// Optional raw-TS capture of exactly what the demuxer receives (diagnostics).
FILE* g_dump = nullptr;

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// Field order token from the player -> short HUD label.
std::string field_tag(const std::string& order) {
    if (order == "tt" || order == "tb") return "TFF";
    if (order == "bb" || order == "bt") return "BFF";
    if (order == "progressive") return "PROG";
    if (order == "mixed") return "MIXED";
    return "---";
}

// Broadcast text -> what the VCR OSD font can draw: uppercase ASCII, with
// Latin-1 accents folded to their base letter and anything else as '?'.
std::string vcr_text(const std::string& in, size_t max_len = std::string::npos) {
    static const char* kFold =  // U+00C0..U+00FF
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTS" "AAAAAAACEEEEIIII" "DNOOOOO/OUUUUYTY";
    std::string out;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) { cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < in.size()) {
            cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F); len = 3;
        } else { cp = '?'; len = 1; }
        i += (size_t)len;
        char o;
        if (cp >= 0x20 && cp < 0x7F) o = (char)std::toupper((int)cp);
        else if (cp >= 0xC0 && cp <= 0xFF) o = kFold[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019) o = '\'';
        else if (cp == 0x201C || cp == 0x201D) o = '"';
        else if (cp == 0x2013 || cp == 0x2014) o = '-';
        else if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) o = ' ';
        else if (cp < 0x20) continue;
        else o = '?';
        out += o;
    }
    if (out.size() > max_len) out.resize(max_len);
    return out;
}

// Word-wrap to `width` columns.
std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> lines;
    std::string line, word;
    std::stringstream ss(text);
    while (ss >> word) {
        while (word.size() > width) {   // hard-split very long words
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            lines.push_back(word.substr(0, width));
            word = word.substr(width);
        }
        if (line.empty()) line = word;
        else if (line.size() + 1 + word.size() <= width) line += " " + word;
        else { lines.push_back(line); line = word; }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// Stream indices of `prog` (or of the whole file when prog is null).
std::vector<int> stream_indices(const AVFormatContext* fmt, const AVProgram* prog) {
    std::vector<int> v;
    if (prog) for (unsigned j = 0; j < prog->nb_stream_indexes; ++j) v.push_back((int)prog->stream_index[j]);
    else for (unsigned i = 0; i < fmt->nb_streams; ++i) v.push_back((int)i);
    return v;
}

std::vector<int> streams_of_type(const AVFormatContext* fmt, const AVProgram* prog,
                                 AVMediaType type) {
    std::vector<int> v;
    for (int i : stream_indices(fmt, prog))
        if (fmt->streams[i]->codecpar->codec_type == type) v.push_back(i);
    return v;
}

const AVProgram* find_program(const AVFormatContext* fmt, int id) {
    for (unsigned i = 0; i < fmt->nb_programs; ++i)
        if (fmt->programs[i]->id == id) return fmt->programs[i];
    return nullptr;
}

// Programs that carry video = the sub-channels a viewer can pick.
std::vector<const AVProgram*> video_programs(const AVFormatContext* fmt) {
    std::vector<const AVProgram*> v;
    for (unsigned i = 0; i < fmt->nb_programs; ++i)
        if (!streams_of_type(fmt, fmt->programs[i], AVMEDIA_TYPE_VIDEO).empty())
            v.push_back(fmt->programs[i]);
    return v;
}

std::string audio_label(const AVStream* st) {
    const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
    return fmt("%s %s %dCH", lang ? upper(lang->value).c_str() : "---",
               upper(avcodec_get_name(st->codecpar->codec_id)).c_str(),
               st->codecpar->ch_layout.nb_channels);
}

// ---------------------------------------------------------------------------
bool file_exists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

// Firmware is not distributable, so it is not bundled. Locate it at runtime:
// explicit --fw, $HIBIKI_FW, next to / above the executable (dev tree), the
// bundle Resources, CWD, or a per-user config dir.
std::string resolve_fw(const std::string& explicit_path, bool explicit_given) {
    const std::string name = "mbin_mxl_eagle_fw.bin";
    if (explicit_given) return explicit_path;
    std::vector<std::string> cands;
    if (const char* env = std::getenv("HIBIKI_FW")) cands.push_back(env);
    if (const char* home = std::getenv("HOME"))
        cands.push_back(std::string(home) + "/Library/Application Support/hibiki/fw/" + name);
    const char* base = SDL_GetBasePath();
    std::string b = base ? base : "";
    cands.push_back(b + "../Resources/fw/" + name);
#ifdef HIBIKI_SOURCE_DIR
    cands.push_back(std::string(HIBIKI_SOURCE_DIR) + "/fw/" + name);
#endif
#ifdef HIBIKI_MAIN_CHECKOUT_DIR   // git worktree builds: fw/ lives in the main checkout
    cands.push_back(std::string(HIBIKI_MAIN_CHECKOUT_DIR) + "/fw/" + name);
#endif
    std::string up = b;
    for (int i = 0; i < 6; ++i) { cands.push_back(up + "fw/" + name); up += "../"; }
    cands.push_back(std::string("fw/") + name);
    if (const char* home = std::getenv("HOME")) {
        cands.push_back(std::string(home) + "/Library/Application Support/hibiki/fw/" + name);
        cands.push_back(std::string(home) + "/.config/hibiki/fw/" + name);
    }
    for (const auto& c : cands) if (file_exists(c)) {
        std::error_code ec;
        auto path = std::filesystem::weakly_canonical(c, ec);
        return ec ? c : path.string();
    }
    return std::string("fw/") + name;
}

// Prefer the VCR OSD font (bundled / res/font) for the on-screen display;
// fall back to a system font if it is unavailable.
std::string resolve_font(const std::string& explicit_path, bool explicit_given) {
    const std::string name = "VCR_OSD_MONO_1.001.ttf";
    if (explicit_given) return explicit_path;
    std::vector<std::string> cands;
    const char* base = SDL_GetBasePath();
    std::string b = base ? base : "";
    cands.push_back(b + "../Resources/font/" + name);
    std::string up = b;
    for (int i = 0; i < 6; ++i) { cands.push_back(up + "res/font/" + name); up += "../"; }
    cands.push_back(std::string("res/font/") + name);
    for (const auto& c : cands) if (file_exists(c)) return c;
    return DEFAULT_FONT;
}

std::string pref_dir() {
    char* p = SDL_GetPrefPath("hibiki", "Hibiki");
    std::string dir = p ? p : "";
    if (p) SDL_free(p);
    if (dir.empty())
        dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config/hibiki/";
    return dir;
}

// ---------------------------------------------------------------------------
// Scanned-channel memory. The app scans the (US) channel plan once, remembers
// the channels that lock, and thereafter tunes / navigates only those.
struct ChannelEntry { int ch; uint32_t freq; float snr; };

std::string channels_path() { return pref_dir() + "channels.txt"; }

std::vector<ChannelEntry> load_channels() {
    std::vector<ChannelEntry> v;
    FILE* f = std::fopen(channels_path().c_str(), "r");
    if (!f) return v;
    int ch; unsigned freq; float snr;
    while (std::fscanf(f, "%d %u %f", &ch, &freq, &snr) == 3)
        v.push_back({ch, (uint32_t)freq, snr});
    std::fclose(f);
    return v;
}

void save_channels(const std::vector<ChannelEntry>& v) {
    FILE* f = std::fopen(channels_path().c_str(), "w");
    if (!f) return;
    for (const auto& e : v) std::fprintf(f, "%d %u %.1f\n", e.ch, e.freq, e.snr);
    std::fclose(f);
}

// PSIP cache: one file per RF channel (psip/<rf>.txt) so channel names and
// listings are there as soon as a channel is tuned, before the carousel
// comes round again. Tab-separated: C = virtual channel, E = event.
std::string psip_cache_path(int rf) { return pref_dir() + fmt("psip/%d.txt", rf); }

std::string cache_field(const std::string& s) {
    std::string o = s;
    for (auto& c : o) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return o;
}

void save_psip_cache(int rf, const psip::Decoder& d, int64_t now) {
    SDL_CreateDirectory((pref_dir() + "psip").c_str());
    std::string path = psip_cache_path(rf);
    FILE* f = std::fopen((path + ".tmp").c_str(), "w");
    if (!f) return;
    for (const auto& c : d.channels())
        std::fprintf(f, "C\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n", c.major, c.minor,
                     c.program_number, c.source_id, c.tsid, c.service_type, (int)c.hidden,
                     (int)c.hide_guide, cache_field(c.name).c_str(), cache_field(c.description).c_str());
    for (const auto& c : d.channels())
        for (const auto& e : d.events(c.source_id, now))
            std::fprintf(f, "E\t%d\t%d\t%lld\t%d\t%s\t%s\t%s\n", e.source_id, e.event_id,
                         (long long)e.start, e.duration, cache_field(e.rating).c_str(),
                         cache_field(e.title).c_str(), cache_field(e.description).c_str());
    std::fclose(f);
    std::rename((path + ".tmp").c_str(), path.c_str());
}

// Load cached data for `rf` into `d` (only_channels skips events).
bool load_psip_cache(int rf, psip::Decoder& d, int64_t now, bool only_channels = false) {
    FILE* f = std::fopen(psip_cache_path(rf).c_str(), "r");
    if (!f) return false;
    std::string line;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        std::vector<std::string> v;
        size_t a = 0, b;
        while ((b = line.find('\t', a)) != std::string::npos) { v.push_back(line.substr(a, b - a)); a = b + 1; }
        v.push_back(line.substr(a));
        if (v[0] == "C" && v.size() >= 11) {
            psip::VirtualChannel c;
            c.major = std::atoi(v[1].c_str()); c.minor = std::atoi(v[2].c_str());
            c.program_number = std::atoi(v[3].c_str()); c.source_id = std::atoi(v[4].c_str());
            c.tsid = std::atoi(v[5].c_str()); c.service_type = std::atoi(v[6].c_str());
            c.hidden = v[7] == "1"; c.hide_guide = v[8] == "1";
            c.name = v[9]; c.description = v[10];
            d.add_channel(c);
        } else if (v[0] == "E" && v.size() >= 8 && !only_channels) {
            psip::Event e;
            e.source_id = std::atoi(v[1].c_str()); e.event_id = std::atoi(v[2].c_str());
            e.start = std::atoll(v[3].c_str()); e.duration = std::atoi(v[4].c_str());
            e.rating = v[5]; e.title = v[6]; e.description = v[7];
            if (e.end() > now) d.add_event(e);
        }
    }
    std::fclose(f);
    return true;
}

// User preferences (settings.txt, key=value).
enum Scale { SCALE_FIT, SCALE_FULL, SCALE_STRETCH, SCALE_COUNT };
const char* kScaleNames[SCALE_COUNT] = {"FIT", "FULL", "STRETCH"};
enum Scan { SCAN_OFF, SCAN_MENU, SCAN_ALL, SCAN_COUNT };
const char* kScanNames[SCAN_COUNT] = {"OFF", "MENU", "ALL"};

struct Settings {
    float volume = 1.0f;
    int scale = SCALE_FIT;
    bool hud = false;          // persistent channel/show banner and clock
    bool info = false;         // signal and playback diagnostics (bottom-right)
    int scanlines = SCAN_MENU;
    bool clock24 = false;
    bool fav_only = false;     // channel surfing skips non-favourites
    int scan_max = hibiki::ATSC_CH_MAX_POST_REPACK;
    int last_channel = 0;
    int last_program = -1;     // MPEG program (sub-channel) last watched on it
    // Favourite virtual channels, keyed rf * 100000 + program (a bare RF
    // number from older settings marks every sub-channel on that RF).
    std::set<int> favs;
};

std::string settings_path() { return pref_dir() + "settings.txt"; }

Settings load_settings() {
    Settings s;
    FILE* f = std::fopen(settings_path().c_str(), "r");
    if (!f) return s;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        auto eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        int iv = std::atoi(v.c_str());
        if (k == "volume") s.volume = std::clamp((float)std::atof(v.c_str()), 0.0f, 1.5f);
        else if (k == "scale") s.scale = std::clamp(iv, 0, SCALE_COUNT - 1);
        else if (k == "hud") s.hud = iv != 0;
        else if (k == "info") s.info = iv != 0;
        else if (k == "scanlines") s.scanlines = std::clamp(iv, 0, SCAN_COUNT - 1);
        else if (k == "clock24") s.clock24 = iv != 0;
        else if (k == "fav_only") s.fav_only = iv != 0;
        else if (k == "scan_max")
            s.scan_max = iv >= hibiki::ATSC_CH_MAX ? hibiki::ATSC_CH_MAX
                                                   : hibiki::ATSC_CH_MAX_POST_REPACK;
        else if (k == "last_channel") s.last_channel = iv;
        else if (k == "last_program") s.last_program = iv;
        else if (k == "favs") {
            std::stringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) s.favs.insert(std::atoi(tok.c_str()));
        }
    }
    std::fclose(f);
    return s;
}

void save_settings(const Settings& s) {
    FILE* f = std::fopen(settings_path().c_str(), "w");
    if (!f) return;
    std::fprintf(f, "volume=%.2f\nscale=%d\nhud=%d\ninfo=%d\nscanlines=%d\nclock24=%d\n"
                    "fav_only=%d\nscan_max=%d\nlast_channel=%d\nlast_program=%d\nfavs=",
                 s.volume, s.scale, (int)s.hud, (int)s.info, s.scanlines, (int)s.clock24,
                 (int)s.fav_only, s.scan_max, s.last_channel, s.last_program);
    bool first = true;
    for (int c : s.favs) { std::fprintf(f, first ? "%d" : ",%d", c); first = false; }
    std::fprintf(f, "\n");
    std::fclose(f);
}

// SMPTE-style colour bars (demo mode stand-in for a picture), 4:3.
SDL_Texture* make_color_bars(SDL_Renderer* ren) {
    const int W = 640, H = 480;
    SDL_Surface* s = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(s->format);
    auto fill = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        SDL_Rect rc{x, y, w, h};
        SDL_FillSurfaceRect(s, &rc, SDL_MapRGB(d, nullptr, r, g, b));
    };
    const uint8_t top[7][3] = {{191,191,191},{191,191,0},{0,191,191},{0,191,0},
                               {191,0,191},{191,0,0},{0,0,191}};
    const uint8_t mid[7][3] = {{0,0,191},{19,19,19},{191,0,191},{19,19,19},
                               {0,191,191},{19,19,19},{191,191,191}};
    int bw = (W + 6) / 7;
    for (int i = 0; i < 7; ++i) {
        fill(i * bw, 0, bw, H * 67 / 100, top[i][0], top[i][1], top[i][2]);
        fill(i * bw, H * 67 / 100, bw, H * 8 / 100, mid[i][0], mid[i][1], mid[i][2]);
    }
    int y = H * 75 / 100, h = H - y, q = W * 5 / 28;
    fill(0, y, q, h, 0, 33, 76);
    fill(q, y, q, h, 255, 255, 255);
    fill(2 * q, y, q, h, 50, 0, 106);
    fill(3 * q, y, W - 3 * q, h, 19, 19, 19);
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
    SDL_DestroySurface(s);
    return t;
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [--channel <n>] [--freq <hz>] [--fw <path>] [--font <ttf>]\n"
        "          [--width <px>] [--height <px>] [--trace] [--no-hud] [--rescan]\n"
        "          [--dump-ts <file>] [--screenshot <bmp>] [--shot-after <ms>]\n"
        "          [--menu] [--keys <Key,Key,...>] [--demo] [--ts-file <capture.ts>]\n"
        "          [--stats] [--decoder auto|cpu] [--vsync on|off|adaptive]\n"
        "          [--deinterlace auto|bwdif|yadif|bob|weave|passthrough]\n"
        "          [--field-order auto|tff|bff] [--deinterlace-force]\n"
        "          [--setup | --shortcuts | --about] [--help]\n"
        "  Enter menu; Esc back / quit; Left/Right or PgUp/PgDn channel;\n"
        "  Up/Down volume; +/- volume in menus; 0-9 direct channel; C list; G guide;\n"
        "  A audio track; Z screen scale; I toggle signal info; H toggle HUD; F fullscreen;\n"
        "  ? / F1 shortcuts; B firmware; F2 About; M mute; S scan. --demo runs without a tuner (colour bars) to preview\n"
        "  the OSD; --ts-file replays a capture (e.g. from --dump-ts) as RF\n"
        "  --channel; --keys injects key presses (SDL key names, \"+ms\" waits).\n"
        "  Synthetic replay fixtures and their expected diagnostics live in\n"
        "  tests/fixtures (see the README there).\n",
        prog);
}

// ---------------------------------------------------------------------------
SDL_Keycode shortcut_key(const SDL_KeyboardEvent& event) {
    if (event.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL | SDL_KMOD_ALT)) return SDLK_UNKNOWN;
    auto key = event.key;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (event.repeat && key != SDLK_UP && key != SDLK_DOWN && key != SDLK_PLUS &&
        key != SDLK_EQUALS && key != SDLK_KP_PLUS && key != SDLK_MINUS &&
        key != SDLK_KP_MINUS && key != SDLK_VOLUMEUP && key != SDLK_VOLUMEDOWN) return SDLK_UNKNOWN;
    if (key == SDLK_QUESTION || (key == SDLK_SLASH && (event.mod & SDL_KMOD_SHIFT))) return SDLK_F1;
    return key;
}

SDL_Keycode scripted_key(const std::string& name) {
    // SDL_GetKeyFromName("?") returns the unshifted '/' key. Scripted input
    // has no modifier event, so preserve the shortcut's intended meaning.
    return name == "?" ? SDLK_F1 : SDL_GetKeyFromName(name.c_str());
}

std::string firmware_directory() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/hibiki/fw";
}

// Dialog callback data outlives the window if the application closes while a
// platform file picker is pending. The callback only posts copied path strings.
struct ImportDialog {
    std::mutex mutex;
    bool pending = false, ready = false;
    std::vector<std::filesystem::path> paths;
    std::string error;
};
void SDLCALL import_selected(void* userdata, const char* const* files, int) {
    std::unique_ptr<std::shared_ptr<ImportDialog>> owner(
        static_cast<std::shared_ptr<ImportDialog>*>(userdata));
    auto state = *owner;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->paths.clear(); state->error.clear();
    if (!files) state->error = std::string("File picker: ") + SDL_GetError();
    else for (int i = 0; files[i]; ++i) state->paths.emplace_back(files[i]);
    state->pending = false; state->ready = true;
}


struct Options {
    int channel = 11;
    bool channel_explicit = false;
    uint32_t freq_hz = 0;
    std::string fw_path, font_path, dump_path, screenshot_path, keys;
    std::string ts_file;       // replay a TS capture instead of the tuner
    bool fw_explicit = false, font_explicit = false;
    bool rescan = false, start_menu = false, trace = false, no_hud = false, demo = false;
    bool setup = false;
    std::string start_page;
    bool stats = false;        // print playback diagnostics every 2 s
    player::DecoderPreference decoder = player::DecoderPreference::Auto;
    std::string vsync = "on";  // on | off | adaptive
    // CPU processing; Auto applies BWDIF to interlaced frames only.
    video::ProcessingSettings processing{video::ProcessingMode::Auto,
                                         video::FieldOrder::Auto, false};
    int change_after = 0;      // test hook: auto channel-change after N seconds
    int shot_after_ms = -1;
    int win_w = 1280, win_h = 720;
};

class App {
public:
    explicit App(const Options& o) : opt_(o) {}
    int run();

private:
    friend struct AppRegression;
    // -- lifecycle / IO --
    bool tune_to(int ch, uint32_t f, int program = -1);
    void change_channel(int dir);
    void do_scan();
    void poll_status();
    bool pump_tuning();
    void handle_key(SDL_Keycode k);
    void handle_event(SDL_Event& event);
    void adjust_volume(int dir);
    void set_hud(bool visible);
    void toggle_info();
    void open_page(osd::Page page);
    void verify_firmware();
    void choose_firmware(bool folder);
    void import_firmware(const std::vector<std::filesystem::path>& paths);
    void poll_import();
    osd::Page firmware_page();
    osd::Page shortcuts_page();
    void commit_digits();
    void apply_gain() { pl_.set_gain(muted_ ? 0.0f : set_.volume); }
    bool persist() const { return !opt_.demo && opt_.ts_file.empty(); }
    // Stream open but no bytes from the tuner for 2 s: antenna / signal lost.
    bool signal_lost() const { return media_ok_ && pl_.ms_since_data() > 2000; }
    bool live() const { return opt_.ts_file.empty() && !opt_.demo; }
    void save() { if (persist()) save_settings(set_); }
    player::ReadFn make_source();
    void toast(const std::string& s, int ms = 2000) { toast_ = s; toast_until_ = SDL_GetTicks() + ms; }
    void show_banner(int ms = 4000) { banner_until_ = SDL_GetTicks() + ms; }
    int scanned_index(int ch) const;
    std::string res_label() const;
    std::string subch_label() const;
    std::string audio_value() const;
    void cycle_audio();
    // -- PSIP / guide --
    int current_program() const { return media_ok_ ? pl_.program() : demo_program_; }
    const psip::VirtualChannel* current_vc() const { return psip_.by_program(current_program()); }
    std::vector<const psip::VirtualChannel*> guide_channels() const;
    std::string vc_label(const psip::VirtualChannel& vc) const;
    std::string clock_hm(int64_t t) const;
    std::string rf_names(int rf) const;
    void watch_program(int program_number);
    void psip_housekeeping(bool force = false);
    // -- virtual channels (PSIP major-minor) across all memorised RF channels --
    struct VChan {
        int rf = 0; uint32_t freq = 0; float snr = 0;
        int major = 0, minor = 0;      // 0-0: no VCT known for this RF
        int program = -1;
        std::string name;
    };
    void rebuild_vchans();
    int current_vchan() const;
    std::string vchan_num(const VChan& v) const;
    void tune_vchan(const VChan& v);
    static int fav_key(const VChan& v) { return v.rf * 100000 + std::max(0, v.program); }
    bool is_fav(const VChan& v) const { return set_.favs.count(fav_key(v)) || set_.favs.count(v.rf); }
    void read_vct(int rf);
    // -- tuner bring-up (the app stays up without a tuner) --
    bool init_tuner();
    void retry_tuner();
    osd::Page tuner_error_page();
    // -- drawing --
    void present();
    void maybe_screenshot();
    void draw_picture();
    void draw_hud();
    void draw_scan(int ch, uint32_t f, int done, int total, bool recheck);
    SDL_FRect video_rect(float sw, float sh, float aspect, SDL_FRect* src) const;
    // -- menu pages --
    osd::Page main_page();
    osd::Page channels_page();
    osd::Page guide_page();
    osd::Page guide_channel_page(int source_id);
    osd::Page event_page(int source_id, int event_id);
    osd::Page programs_page();
    osd::Page audio_page();
    osd::Page feature_page();
    osd::Page setup_channel_page();
    osd::Page about_page();

    Options opt_;
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    hibiki::Tuner tuner_;
    player::Player pl_;
    bool media_ok_ = false;
    FILE* ts_file_ = nullptr;
    std::vector<uint8_t> ts_tap_;
    osd::Renderer osd_;
    osd::Menu menu_;
    Uint32 app_event_ = 0;
    std::array<hibiki::firmware::Status, 2> fw_status_;
    std::string firmware_message_ = "IMPORT FILES, A FOLDER, OR DROP FILES ON THIS WINDOW.";
    std::shared_ptr<ImportDialog> import_dialog_ = std::make_shared<ImportDialog>();
    std::vector<std::filesystem::path> dropped_paths_;
    Settings set_;
    std::vector<ChannelEntry> scanned_;
    int channel_ = 0;
    uint32_t freq_ = 0;
    std::string fw_path_;
    hibiki::AtscStatus st_;
    bool st_ok_ = false;
    uint64_t st_ms_ = 0;
    bool muted_ = false, fullscreen_ = false;
    bool busy_ = false, cancel_tune_ = false;
    std::string tuning_;            // non-empty while a blocking tune is shown
    uint64_t banner_until_ = 0, vol_until_ = 0, toast_until_ = 0;
    uint64_t legend_until_ = 0;
    std::string toast_;
    std::string digits_;
    uint64_t digits_ms_ = 0;
    SDL_Texture* bars_ = nullptr;
    uint64_t start_ms_ = 0, last_present_ = 0;
    bool shot_saved_ = false;
    uint64_t menu_refresh_ms_ = 0;
    // PSIP: decoded from the live TS (tapped in ts_read) plus the per-RF cache.
    psip::Decoder psip_;
    int demo_program_ = -1;
    bool tuner_ok_ = false;
    std::vector<std::string> tuner_err_;
    uint64_t psip_saved_gen_ = 0, psip_saved_ms_ = 0;
    std::map<int, std::vector<psip::VirtualChannel>> rf_vct_;  // names per RF, from caches
    std::vector<VChan> vchans_;
    uint64_t vchan_gen_ = 0;
    uint64_t stats_ms_ = 0;
    std::string rend_name_ = "unknown", vsync_req_ = "on", vsync_active_ = "unknown";
    uint64_t present_gap_max_ms_ = 0;
};

int App::scanned_index(int ch) const {
    for (size_t i = 0; i < scanned_.size(); ++i) if (scanned_[i].ch == ch) return (int)i;
    return -1;
}

// ---- tuning ----------------------------------------------------------------
player::ReadFn App::make_source() {
    if (!opt_.ts_file.empty()) {
        FILE* f = ts_file_;
        return [f](uint8_t* buf, int size) -> int {
            size_t n = std::fread(buf, 1, (size_t)size, f);
            return n > 0 ? (int)n : -1;   // end of capture
        };
    }
    return [this](uint8_t* buf, int size) -> int {
        int n = tuner_.ReadTS(buf, (uint32_t)size);   // waits up to 100 ms
        if (n > 0 && g_dump) std::fwrite(buf, 1, (size_t)n, g_dump);
        return n;
    };
}

// Tune RF channel `ch` and start `program` (-1: the last one watched there,
// else the first virtual channel in its VCT, else the player's main service).
bool App::tune_to(int ch, uint32_t f, int program) {
    if (busy_) return false;
    psip_housekeeping(true);   // keep what we learned about the old channel
    pl_.close();
    media_ok_ = false;
    psip_.reset();
    demo_program_ = -1;
    if (persist()) load_psip_cache(ch, psip_, (int64_t)std::time(nullptr));
    psip_saved_gen_ = psip_.generation();
    if (program < 0 && ch == set_.last_channel && set_.last_program >= 0) program = set_.last_program;
    if (program < 0) {
        auto it = rf_vct_.find(ch);
        if (it != rf_vct_.end())
            for (const auto& vc : it->second)
                if (!vc.hidden && vc.service_type != 4) { program = vc.program_number; break; }
    }
    channel_ = ch;
    freq_ = f;
    set_.last_channel = ch;
    save();
    show_banner();
    if (live() && !tuner_ok_) return false;
    busy_ = true;
    cancel_tune_ = false;
    tuning_ = fmt("TUNING CH %d", ch);
    present();   // the tune below blocks for up to several seconds
    bool ok = false;
    if (opt_.demo) {
        SDL_Delay(250);
        ok = scanned_index(ch) >= 0;
        if (ok) {   // synthetic PSIP through the real decoder
            std::vector<uint8_t> ts = psip::build_demo_ts(ch, (int64_t)std::time(nullptr));
            psip_.feed(ts.data(), ts.size());
            demo_program_ = program >= 0 ? program
                          : psip_.channels().empty() ? -1 : psip_.channels()[0].program_number;
        }
    } else if (!opt_.ts_file.empty()) {
        if (ts_file_) std::fclose(ts_file_);
        ts_file_ = std::fopen(opt_.ts_file.c_str(), "rb");
        if (!ts_file_) std::fprintf(stderr, "[-] cannot open %s\n", opt_.ts_file.c_str());
        tuning_ = fmt("CH %d  READING STREAM", ch);
        present();
        ok = ts_file_ && pl_.open(make_source(), ren_, program,
                                   [this] { return pump_tuning(); }, opt_.decoder,
                                   opt_.processing);
    } else {
        std::printf("[*] tuning channel %d (%.3f MHz)...\n", ch, f / 1e6);
        hibiki::AtscStatus st;
        bool locked = false;
        if (tuner_.SetPara(f) == 0) {
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            while (pump_tuning() && std::chrono::steady_clock::now() < until) {
                if (tuner_.ReadStatus(st) && st.locked()) { locked = true; break; }
                SDL_Delay(60);
            }
        }
        if (locked) {
            std::printf("[+] locked (SNR %.1f dB)\n", st.snr_db_tenths / 10.0);
            tuning_ = fmt("CH %d  READING STREAM", ch);
            present();
            ok = pl_.open(make_source(), ren_, program,
                          [this] { return pump_tuning(); }, opt_.decoder,
                          opt_.processing);
        } else {
            std::fprintf(stderr, "[-] channel %d did not lock\n", ch);
        }
    }
    tuning_.clear();
    busy_ = false;
    media_ok_ = ok && !opt_.demo;
    if (media_ok_) { set_.last_program = pl_.program(); save(); }
    apply_gain();
    st_ms_ = 0;   // refresh signal status right away
    rebuild_vchans();
    return ok;
}
// Channel up/down walks the virtual channels (13-1, 13-2, 13-4, 15-1 ...);
// sub-channels on the RF already tuned switch instantly without a retune.
void App::change_channel(int dir) {
    std::vector<VChan> list;
    bool any_fav = false;
    for (const auto& v : vchans_) any_fav |= is_fav(v);
    for (const auto& v : vchans_)
        if (!set_.fav_only || !any_fav || is_fav(v)) list.push_back(v);
    if (list.empty()) {
        int nch = channel_ + dir;
        uint32_t nf = hibiki::atsc_channel_freq_hz(nch);
        if (nf) tune_to(nch, nf);
        return;
    }
    int n = (int)list.size(), cur = -1;
    int ci = current_vchan();
    if (ci >= 0)
        for (int i = 0; i < n; ++i)
            if (list[i].rf == vchans_[ci].rf && list[i].program == vchans_[ci].program) cur = i;
    int pick;
    if (cur >= 0) {
        pick = ((cur + dir) % n + n) % n;
    } else {   // current channel is filtered out: next one by RF order
        pick = dir > 0 ? 0 : n - 1;
        for (int i = 0; i < n; ++i) {
            int j = dir > 0 ? i : n - 1 - i;
            if (dir > 0 ? list[j].rf > channel_ : list[j].rf < channel_) { pick = j; break; }
        }
    }
    tune_vchan(list[pick]);
}
// "13" -> first 13-x, "13-2" -> 13-2; no such virtual channel -> RF 13.
void App::commit_digits() {
    if (digits_.empty()) return;
    std::string d = digits_;
    digits_.clear();
    size_t dash = d.find('-');
    int major = std::atoi(d.substr(0, dash).c_str());
    int minor = dash != std::string::npos && dash + 1 < d.size() ? std::atoi(d.substr(dash + 1).c_str()) : -1;
    const VChan* best = nullptr;
    for (const auto& v : vchans_) {
        if (v.major != major || (minor >= 0 && v.minor != minor)) continue;
        if (!best || v.minor < best->minor) best = &v;
    }
    if (best) { VChan v = *best; tune_vchan(v); return; }
    if (dash != std::string::npos) { toast("CH " + d + " NOT FOUND"); return; }
    uint32_t f = hibiki::atsc_channel_freq_hz(major);
    if (!f) { toast(fmt("CH %d NOT IN PLAN", major)); return; }
    tune_to(major, f);
}

void App::rebuild_vchans() {
    vchans_.clear();
    for (const auto& e : scanned_) {
        const std::vector<psip::VirtualChannel>* vct = nullptr;
        if (e.ch == channel_ && !psip_.channels().empty()) vct = &psip_.channels();
        else if (auto it = rf_vct_.find(e.ch); it != rf_vct_.end() && !it->second.empty()) vct = &it->second;
        bool added = false;
        if (vct) {
            for (const auto& c : *vct) {
                if (c.hidden || c.service_type == 4) continue;   // hidden / data-only
                vchans_.push_back({e.ch, e.freq, e.snr, c.major, c.minor, c.program_number,
                                   vcr_text(c.name, 7)});
                added = true;
            }
        }
        if (!added) vchans_.push_back({e.ch, e.freq, e.snr, 0, 0, -1, ""});
    }
    std::stable_sort(vchans_.begin(), vchans_.end(), [](const VChan& a, const VChan& b) {
        int ka = a.major ? a.major : a.rf, kb = b.major ? b.major : b.rf;
        return ka != kb ? ka < kb : a.minor < b.minor;
    });
    vchan_gen_ = psip_.generation();
}

int App::current_vchan() const {
    int pn = current_program(), rf_only = -1;
    for (size_t i = 0; i < vchans_.size(); ++i) {
        if (vchans_[i].rf != channel_) continue;
        if (vchans_[i].program == pn && pn >= 0) return (int)i;
        if (rf_only < 0) rf_only = (int)i;
    }
    return rf_only;
}

std::string App::vchan_num(const VChan& v) const {
    return v.major ? fmt("%d-%d", v.major, v.minor) : fmt("%d", v.rf);
}

void App::tune_vchan(const VChan& v) {
    if (v.rf == channel_ && (media_ok_ || opt_.demo) && v.program >= 0) watch_program(v.program);
    else tune_to(v.rf, v.freq, v.program);
}

// Read the VCT straight off the tuner right after a scan lock (TVCT repeats
// at least every 400 ms) so the channel list has names before anything is
// watched. Merges into, and saves, the RF channel's PSIP cache.
void App::read_vct(int rf) {
    if (opt_.demo) {
        psip::Decoder d;
        std::vector<uint8_t> ts = psip::build_demo_ts(rf, (int64_t)std::time(nullptr));
        d.feed(ts.data(), ts.size());
        rf_vct_[rf] = d.channels();
        return;
    }
    psip::Decoder d;
    int64_t now = (int64_t)std::time(nullptr);
    load_psip_cache(rf, d, now);
    size_t had = d.channels().size();
    uint64_t seen = d.sections_ok();
    std::vector<uint8_t> buf(64 * 1024);
    uint64_t t0 = SDL_GetTicks(), got_vct = 0;
    while (SDL_GetTicks() - t0 < 2500 && hibiki::g_running) {
        int n = tuner_.ReadTS(buf.data(), (uint32_t)buf.size());
        if (n > 0) d.feed(buf.data(), (size_t)n);
        if (!got_vct && d.channels().size() > had) got_vct = SDL_GetTicks();
        if (got_vct && SDL_GetTicks() - got_vct > 500) break;   // let multi-section VCTs finish
    }
    if (d.sections_ok() > seen && !d.channels().empty()) {
        rf_vct_[rf] = d.channels();
        save_psip_cache(rf, d, d.now_utc());
    }
}
void App::poll_status() {
    if (opt_.no_hud || !opt_.ts_file.empty() || (live() && !tuner_ok_)) return;
    uint64_t now = SDL_GetTicks();
    if (st_ms_ && now - st_ms_ < 500) return;
    st_ms_ = now;
    if (opt_.demo) {
        int i = scanned_index(channel_);
        st_ok_ = true;
        bool lk = i >= 0;
        st_.frame_lock = st_.atsc_lock = st_.fec_lock = lk;
        st_.snr_db_tenths = lk ? (int16_t)(scanned_[i].snr * 10 + (int)(now / 500 % 5) - 2) : 0;
        return;
    }
    st_ok_ = tuner_.ReadStatus(st_);
}

void App::do_scan() {
    if (busy_) return;
    if (!opt_.ts_file.empty()) { toast("NO SCAN WHEN REPLAYING A FILE"); return; }
    if (!opt_.demo && !tuner_ok_) { toast("NO TUNER - CANNOT SCAN"); return; }
    pl_.close();
    media_ok_ = false;
    menu_.close();
    std::vector<ChannelEntry> prev = scanned_, found, maybe;
    bool cancel = false;
    auto pump = [&]() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { hibiki::g_running = false; cancel = true; }
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) cancel = true;
        }
    };
    // 2 = full lock, 1 = partial (carrier/frame but not FEC), 0 = nothing.
    auto probe = [&](int ch, uint32_t f, int dwell, hibiki::AtscStatus& st) -> int {
        if (opt_.demo) {
            SDL_Delay(60);
            static const int demo_chs[] = {7, 9, 11, 14, 20, 27, 33};
            for (int c : demo_chs) if (c == ch) { st.snr_db_tenths = (int16_t)(180 + ch * 3); return 2; }
            return 0;
        }
        if (tuner_.SetPara(f) != 0) return 0;
        if (tuner_.WaitLock(dwell, &st)) return 2;
        return (st.atsc_lock || st.frame_lock) ? 1 : 0;
    };
    const int lo = hibiki::ATSC_CH_MIN, hi = set_.scan_max;
    const int total = hi - lo + 1;
    for (int ch = lo; ch <= hi && !cancel; ++ch) {
        uint32_t f = hibiki::atsc_channel_freq_hz(ch);
        if (!f) continue;
        scanned_ = found;   // so the scan screen can show the running count
        draw_scan(ch, f, ch - lo, total, false);
        pump();
        if (cancel) break;
        hibiki::AtscStatus st;
        int r = probe(ch, f, 2000, st);
        std::fprintf(stderr, "ch %2d %7.3f MHz: snr %5.1f dB carrier %+7d Hz "
                     "flags f%d a%d e%d -> %s\n", ch, f / 1e6,
                     st.snr_db_tenths / 10.0, st.carrier_offset_hz,
                     st.frame_lock, st.atsc_lock, st.fec_lock,
                     r == 2 ? "LOCK" : r == 1 ? "partial" : "none");
        if (r == 2) {
            found.push_back({ch, f, st.snr_db_tenths / 10.0f});
            read_vct(ch);
        }
        else if (r == 1) maybe.push_back({ch, f, st.snr_db_tenths / 10.0f});
    }
    // Weak stations can take several seconds to reach FEC lock; re-probe the
    // partial-lock candidates with a longer dwell before giving up.
    for (size_t i = 0; i < maybe.size() && !cancel; ++i) {
        const auto& e = maybe[i];
        scanned_ = found;
        draw_scan(e.ch, e.freq, (int)i, (int)maybe.size(), true);
        pump();
        if (cancel) break;
        hibiki::AtscStatus st;
        if (probe(e.ch, e.freq, 5000, st) == 2) {
            found.push_back({e.ch, e.freq, st.snr_db_tenths / 10.0f});
            read_vct(e.ch);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const ChannelEntry& a, const ChannelEntry& b) { return a.ch < b.ch; });
    if (cancel && !prev.empty()) {
        scanned_ = prev;   // an aborted scan keeps the old channel memory
        toast("SCAN CANCELLED");
    } else {
        scanned_ = found;
        if (!opt_.demo) save_channels(scanned_);
        toast(fmt("%zu CHANNELS FOUND", scanned_.size()), 3000);
        std::printf("[+] scan complete: %zu channel(s) saved to %s\n",
                    scanned_.size(), channels_path().c_str());
    }
    if (!hibiki::g_running) return;
    if (!scanned_.empty()) {
        int i = std::max(0, scanned_index(channel_));
        tune_to(scanned_[i].ch, scanned_[i].freq);
    }
}

// ---- labels ----------------------------------------------------------------
std::string App::res_label() const {
    if (opt_.demo) return "640X480";
    if (!media_ok_ || !pl_.width()) return "---";
    return fmt("%dX%d", pl_.width(), pl_.height());
}

std::string App::subch_label() const {
    if (const psip::VirtualChannel* vc = current_vc()) return vc_label(*vc);
    if (!media_ok_) return "---";
    auto progs = video_programs(pl_.format());
    for (size_t i = 0; i < progs.size(); ++i)
        if (progs[i]->id == pl_.program()) return fmt("%zu/%zu", i + 1, progs.size());
    return "1/1";
}

std::string App::audio_value() const {
    if (!media_ok_ || pl_.audio_stream() < 0) return "NONE";
    const AVFormatContext* fc = pl_.format();
    auto aud = streams_of_type(fc, find_program(fc, pl_.program()), AVMEDIA_TYPE_AUDIO);
    size_t pos = std::find(aud.begin(), aud.end(), pl_.audio_stream()) - aud.begin();
    return fmt("%zu ", pos + 1) + audio_label(fc->streams[pl_.audio_stream()]);
}

void App::cycle_audio() {
    if (!media_ok_) return;
    const AVFormatContext* fc = pl_.format();
    auto aud = streams_of_type(fc, find_program(fc, pl_.program()), AVMEDIA_TYPE_AUDIO);
    if (aud.size() < 2) { toast("AUDIO: " + audio_value()); return; }
    size_t pos = std::find(aud.begin(), aud.end(), pl_.audio_stream()) - aud.begin();
    pl_.select_audio(aud[(pos + 1) % aud.size()]);
    apply_gain();
    toast("AUDIO: " + audio_value());
}

// ---- PSIP / guide helpers ----------------------------------------------------
std::vector<const psip::VirtualChannel*> App::guide_channels() const {
    std::vector<const psip::VirtualChannel*> v;
    for (const auto& c : psip_.channels())
        if (!c.hidden && !c.hide_guide) v.push_back(&c);
    return v;
}

std::string App::vc_label(const psip::VirtualChannel& vc) const {
    return fmt("%d-%d %s", vc.major, vc.minor, vcr_text(vc.name, 7).c_str());
}

std::string App::clock_hm(int64_t t) const {
    std::time_t tt = (std::time_t)t;
    std::tm lt{};
    localtime_r(&tt, &lt);
    char b[16];
    if (set_.clock24) { std::strftime(b, sizeof(b), "%H:%M", &lt); return b; }
    std::strftime(b, sizeof(b), "%I:%M", &lt);
    return std::string(b) + (lt.tm_hour < 12 ? "A" : "P");
}

std::string App::rf_names(int rf) const {
    const std::vector<psip::VirtualChannel>* v = nullptr;
    if (rf == channel_ && !psip_.channels().empty()) v = &psip_.channels();
    else if (auto it = rf_vct_.find(rf); it != rf_vct_.end()) v = &it->second;
    if (!v || v->empty()) return "";
    std::string s = vc_label(v->front());
    if (v->size() > 1) s += fmt(" +%zu", v->size() - 1);
    return s;
}

void App::watch_program(int pn) {
    if (busy_) return;
    if (pn == current_program()) { show_banner(); return; }
    if (media_ok_) {
        const AVFormatContext* fc = pl_.format();
        if (!find_program(fc, pn) || !pl_.select_program(pn)) {
            toast("SUB CHANNEL NOT IN STREAM");
            if (pl_.video_stream() < 0) pl_.select_program(-1);
        }
        apply_gain();
        set_.last_program = pl_.program();
        save();
    } else if (opt_.demo) {
        demo_program_ = pn;
    }
    show_banner();
}
// Persist the current channel's PSIP every 15 s when it changed (and on retune
// / exit), and keep the per-RF name table for the channel list current.
void App::psip_housekeeping(bool force) {
    if (channel_ <= 0 || psip_.channels().empty()) return;
    if (psip_.generation() == psip_saved_gen_) return;
    uint64_t ms = SDL_GetTicks();
    if (!force && ms - psip_saved_ms_ < 15000) return;
    rf_vct_[channel_] = psip_.channels();
    if (persist()) {
        int64_t now = psip_.now_utc();
        psip_.prune(now);
        save_psip_cache(channel_, psip_, now);
    }
    psip_saved_gen_ = psip_.generation();
    psip_saved_ms_ = ms;
}

// ---- tuner bring-up ------------------------------------------------------------
// Launched from Finder there is no terminal, so failures become an on-screen
// page (with retry) instead of a silent exit.
bool App::init_tuner() {
    tuner_err_.clear();
    fw_path_ = resolve_fw(opt_.fw_path, opt_.fw_explicit);
    std::printf("[*] firmware: %s\n", fw_path_.c_str());
    verify_firmware();
    if (!fw_status_[0].verified) {
        std::fprintf(stderr, "[-] firmware not found: %s (set --fw or $HIBIKI_FW)\n",
                     fw_path_.c_str());
        tuner_err_ = {"FIRMWARE MISSING OR INVALID", "USE MENU > FIRMWARE SETUP",
                      "PUT MBIN_MXL_EAGLE_FW.BIN IN:",
                      " ~/Library/Application Support/",
                      "   hibiki/fw/",
                      "OR SET $HIBIKI_FW / --fw PATH"};
        tuner_ok_ = false;
        return false;
    }
    hibiki::TunerConfig cfg;
    cfg.fw_path = fw_path_;
    cfg.trace = opt_.trace;
    tuner_ok_ = tuner_.Init(cfg) == 0;
    if (!tuner_ok_) {
        std::fprintf(stderr, "[-] tuner init failed\n");
        tuner_err_ = {"TUNER NOT FOUND", "",
                      "CHECK THE STICK IS PLUGGED IN",
                      "AND ITS USB FIRMWARE IS LOADED:",
                      " scripts/wake_tuner.py",
                      "(USB 1F4D:E691 EXPECTED)"};
    }
    return tuner_ok_;
}

void App::retry_tuner() {
    if (busy_) return;
    tuner_.Close();
    if (!init_tuner()) { menu_.refresh(); toast("STILL NO TUNER"); return; }
    menu_.close();
    if (scanned_.empty()) { do_scan(); return; }   // scan tunes when done
    int i = std::max(0, scanned_index(channel_ ? channel_ : set_.last_channel));
    tune_to(scanned_[i].ch, scanned_[i].freq);
}

osd::Page App::tuner_error_page() {
    osd::Page p;
    p.title = "TUNER ERROR";
    p.hints = {"SELECT:\xE2\x96\xB2\xE2\x96\xBC KEY", "SET   :\xE2\x96\xBA KEY",
               "BACK  :ESC KEY"};
    p.build = [this](osd::Page& pg) {
        pg.body = tuner_err_;
        pg.items.clear();
        osd::Item it;
        it.label = "RETRY";
        it.activate = [this] { retry_tuner(); };
        pg.items.push_back(it);
        it = {};
        it.label = "FIRMWARE SETUP";
        it.activate = [this] { menu_.push(firmware_page()); };
        pg.items.push_back(it);
        it = {};
        it.label = "QUIT";
        it.activate = [] { hibiki::g_running = false; };
        pg.items.push_back(it);
    };
    return p;
}

// ---- input -----------------------------------------------------------------
void App::handle_key(SDL_Keycode k) {
    if (k == SDLK_F1 || k == SDLK_QUESTION) {
        if (menu_.top() && menu_.top()->title == "KEYBOARD SHORTCUTS") menu_.pop();
        else menu_.push(shortcuts_page());
        return;
    }
    // Dedicated volume keys also work inside the guide and menus. '-' remains
    // the virtual-channel separator while a channel number is being entered.
    if (k == SDLK_PLUS || k == SDLK_EQUALS || k == SDLK_KP_PLUS || k == SDLK_VOLUMEUP) {
        adjust_volume(+1); return;
    }
    if ((k == SDLK_MINUS || k == SDLK_KP_MINUS || k == SDLK_VOLUMEDOWN) && digits_.empty()) {
        adjust_volume(-1); return;
    }
    if (menu_.open() && menu_.key(k)) return;
    if (k == SDLK_BACKSPACE && !digits_.empty()) {
        digits_.pop_back(); digits_ms_ = SDL_GetTicks(); return;
    }

    int digit = -1;
    if (k >= SDLK_0 && k <= SDLK_9) digit = (int)(k - SDLK_0);
    else if (k >= SDLK_KP_1 && k <= SDLK_KP_9) digit = (int)(k - SDLK_KP_1) + 1;
    else if (k == SDLK_KP_0) digit = 0;
    if (digit >= 0 && !menu_.open()) {
        if (digits_.empty() && digit == 0) return;
        if (digits_.size() >= 6) return;
        digits_ += (char)('0' + digit);
        digits_ms_ = SDL_GetTicks();
        return;
    }
    bool dash = k == SDLK_MINUS || k == SDLK_PERIOD || k == SDLK_KP_MINUS || k == SDLK_KP_PERIOD;
    if (dash && !menu_.open() && !digits_.empty() && digits_.find('-') == std::string::npos) {
        digits_ += '-';
        digits_ms_ = SDL_GetTicks();
        return;
    }

    switch (k) {
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        if (!digits_.empty()) digits_.clear();
        else hibiki::g_running = false;
        break;
    case SDLK_Q: hibiki::g_running = false; break;
    case SDLK_R: if (live() && !tuner_ok_) retry_tuner(); break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_TAB:
        if (!digits_.empty()) commit_digits();
        else if (!menu_.open()) menu_.push(main_page());
        break;
    case SDLK_C:
    case SDLK_L: menu_.close(); menu_.push(channels_page()); break;
    case SDLK_G: menu_.close(); menu_.push(guide_page()); break;
    case SDLK_A: cycle_audio(); break;
    case SDLK_Z:
        set_.scale = (set_.scale + 1) % SCALE_COUNT;
        save();
        toast(std::string("SCALE: ") + kScaleNames[set_.scale]);
        break;
    case SDLK_I: menu_.close(); toggle_info(); break;
    case SDLK_B: open_page(firmware_page()); break;
    case SDLK_F2: open_page(about_page()); break;
    case SDLK_H:
        menu_.close();
        set_hud(!(set_.hud || SDL_GetTicks() < banner_until_));
        break;
    case SDLK_F:
        fullscreen_ = !fullscreen_;
        SDL_SetWindowFullscreen(win_, fullscreen_);
        break;
    case SDLK_M: muted_ = !muted_; apply_gain(); vol_until_ = SDL_GetTicks() + 2500; break;
    case SDLK_UP:
    case SDLK_DOWN:
        adjust_volume(k == SDLK_UP ? +1 : -1);
        break;
    case SDLK_S: do_scan(); break;
    case SDLK_RIGHT: case SDLK_PAGEDOWN: change_channel(+1); break;
    case SDLK_LEFT:  case SDLK_PAGEUP:   change_channel(-1); break;
    default: legend_until_ = SDL_GetTicks() + 3000; break;
    }
}

void App::adjust_volume(int dir) {
    set_.volume = std::clamp(std::round((set_.volume + dir * 0.1f) * 10) / 10, 0.0f, 1.5f);
    muted_ = false;
    apply_gain();
    save();
    vol_until_ = SDL_GetTicks() + 2500;
}

void App::set_hud(bool visible) {
    set_.hud = visible;
    // A recent channel change must not keep the banner visible after H hides it.
    banner_until_ = 0;
    save();
    toast(set_.hud ? "HUD: ON" : "HUD: OFF");
}

void App::toggle_info() {
    set_.info = !set_.info;
    if (set_.info) { opt_.no_hud = false; st_ms_ = 0; }
    save();
    toast(set_.info ? "SIGNAL INFO: ON" : "SIGNAL INFO: OFF");
}

void App::open_page(osd::Page page) {
    menu_.close();
    menu_.push(std::move(page));
}

bool App::pump_tuning() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) cancel_tune_ = true;
        else handle_event(event);
    }
    // PSIP collected during probing already contains channel names and often
    // show listings; display it without waiting for the video decoder to open.
    pl_.take_ts(ts_tap_);
    if (!ts_tap_.empty()) psip_.feed(ts_tap_.data(), ts_tap_.size());
    if (SDL_GetTicks() - last_present_ >= 33) present();
    return hibiki::g_running && !cancel_tune_;
}

void App::handle_event(SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT) hibiki::g_running = false;
    else if (event.type == SDL_EVENT_MOUSE_MOTION &&
             event.motion.windowID == SDL_GetWindowID(win_) &&
             (event.motion.xrel != 0 || event.motion.yrel != 0)) {
        legend_until_ = SDL_GetTicks() + 3000;
    }
    else if (event.type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode key = shortcut_key(event.key);
        if (key != SDLK_UNKNOWN) handle_key(key);
    } else if (event.type == app_event_) {
        switch (static_cast<hibiki::AppAction>(event.user.code)) {
        case hibiki::AppAction::about: open_page(about_page()); break;
        case hibiki::AppAction::shortcuts: menu_.push(shortcuts_page()); break;
        case hibiki::AppAction::firmware: open_page(firmware_page()); break;
        case hibiki::AppAction::rescan: do_scan(); break;
        }
    } else if (event.type == SDL_EVENT_DROP_BEGIN) dropped_paths_.clear();
    else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) dropped_paths_.emplace_back(event.drop.data);
    else if (event.type == SDL_EVENT_DROP_COMPLETE && !dropped_paths_.empty()) {
        import_firmware(dropped_paths_);
        dropped_paths_.clear();
    }
}

void App::verify_firmware() {
    fw_path_ = resolve_fw(opt_.fw_path, opt_.fw_explicit);
    fw_status_[0] = hibiki::firmware::verify(fw_path_, hibiki::firmware::specs[0]);
    auto bridge = std::filesystem::path(fw_path_).parent_path() / hibiki::firmware::specs[1].path;
    auto installed = std::filesystem::path(firmware_directory()) / hibiki::firmware::specs[1].path;
    std::error_code ec;
    if (std::filesystem::is_regular_file(installed, ec)) bridge = installed;
    fw_status_[1] = hibiki::firmware::verify(bridge, hibiki::firmware::specs[1]);
}

void App::choose_firmware(bool folder) {
    {
        std::lock_guard<std::mutex> lock(import_dialog_->mutex);
        if (import_dialog_->pending) return;
        import_dialog_->pending = true;
    }
    auto* owner = new std::shared_ptr<ImportDialog>(import_dialog_);
    if (folder) SDL_ShowOpenFolderDialog(import_selected, owner, win_, nullptr, false);
    else SDL_ShowOpenFileDialog(import_selected, owner, win_, nullptr, 0, nullptr, true);
}

void App::import_firmware(const std::vector<std::filesystem::path>& paths) {
    if (hibiki::firmware::install(paths, firmware_directory(), firmware_message_)) {
        verify_firmware();
        firmware_message_ = "VERIFIED AND INSTALLED. CONNECT WHEN READY.";
        if (opt_.fw_explicit || std::getenv("HIBIKI_FW"))
            firmware_message_ = "INSTALLED. --FW / HIBIKI_FW OVERRIDE STILL TAKES PRIORITY.";
    }
    open_page(firmware_page());
}

void App::poll_import() {
    std::vector<std::filesystem::path> paths;
    {
        std::lock_guard<std::mutex> lock(import_dialog_->mutex);
        if (!import_dialog_->ready) return;
        import_dialog_->ready = false;
        paths.swap(import_dialog_->paths);
        if (!import_dialog_->error.empty()) firmware_message_ = import_dialog_->error;
    }
    if (!paths.empty()) import_firmware(paths);
    else menu_.refresh();
}

osd::Page App::firmware_page() {
    verify_firmware();
    osd::Page p;
    p.title = "FIRMWARE SETUP";
    p.hints = {"UP/DOWN:SELECT  ENTER:SET", "B / ESC:BACK  R:CONNECT"};
    p.on_key = [this](SDL_Keycode key) {
        if (key == SDLK_B) { menu_.pop(); return true; }
        if (key == SDLK_O) { choose_firmware(true); return true; }
        if (key == SDLK_V) {
            verify_firmware(); firmware_message_ = "RECHECKED PINNED SIZES AND SHA-256."; return true;
        }
        if (key == SDLK_R) { if (live()) retry_tuner(); else toast("CONNECT FROM LIVE MODE"); return true; }
        return false;
    };
    p.build = [this](osd::Page& pg) {
        pg.body = {
            std::string("EAGLE: ") + (fw_status_[0].verified ? "VERIFIED" : "MISSING / INVALID") + "  43997 B",
            std::string("FX2  : ") + (fw_status_[1].verified ? "VERIFIED" : "MISSING / INVALID") + "   4212 B",
            "SIZES + SHA-256 CHECKED",
            "IMPORT JSON, BIN, B64 OR A FOLDER",
            "FX2 WAKE: SCRIPTS/WAKE_TUNER.PY"
        };
        auto feedback = wrap(vcr_text(firmware_message_), 33);
        for (size_t i = 0; i < std::min<size_t>(2, feedback.size()); ++i) pg.body.push_back(feedback[i]);
        pg.items.clear();
        osd::Item item;
        item.label = "IMPORT FILES";
        item.activate = [this] { choose_firmware(false); };
        pg.items.push_back(item);
        item = {}; item.label = "IMPORT FOLDER (O)";
        item.activate = [this] { choose_firmware(true); };
        pg.items.push_back(item);
        item = {}; item.label = "VERIFY INSTALLED FILES (V)";
        item.activate = [this] { verify_firmware(); firmware_message_ = "RECHECKED PINNED SIZES AND SHA-256."; };
        pg.items.push_back(item);
        item = {}; item.label = "CONNECT / RETRY (R)";
        item.activate = [this] { if (live()) retry_tuner(); else toast("CONNECT FROM LIVE MODE"); };
        pg.items.push_back(item);
    };
    return p;
}

osd::Page App::shortcuts_page() {
    osd::Page p;
    p.title = "KEYBOARD SHORTCUTS";
    p.body = {"WATCHING: UP/DOWN = VOLUME", "MENUS: UP/DOWN = SELECT"};
    p.hints = {"UP/DOWN:SCROLL  ESC:BACK", "?:CLOSE  +/-:VOLUME ANYWHERE"};
    const char* rows[] = {
        "? / F1      SHORTCUT HELP",
        "ENTER / TAB MAIN MENU",
        "G           PROGRAM GUIDE",
        "C / L       CHANNEL LIST",
        "I           TOGGLE SIGNAL INFO",
        "UP / DOWN   VOLUME WHEN WATCHING",
        "+ / -       VOLUME IN ANY SCREEN",
        "LEFT / PGUP PREVIOUS CHANNEL",
        "RIGHT/ PGDN NEXT CHANNEL",
        "0-9, -      ENTER VIRTUAL CHANNEL",
        "A           NEXT AUDIO TRACK",
        "Z           SCREEN SCALE",
        "H           TOGGLE BASIC HUD",
        "F / M       FULLSCREEN / MUTE",
        "S           AUTO CHANNEL SCAN",
        "B           FIRMWARE SETUP",
        "F2          ABOUT HIBIKI",
        "ESC / BKSP  BACK (WATCHING: QUIT)"
    };
    for (const char* row : rows) { osd::Item item; item.label = row; p.items.push_back(item); }
    return p;
}

// ---- menu pages --------------------------------------------------------------
osd::Page App::main_page() {
    osd::Page p;
    p.title = "MENU";
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        osd::Item it;
        it = {}; it.label = "CHANNEL LIST";
        it.activate = [this] { menu_.push(channels_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "PROGRAM GUIDE";
        it.activate = [this] { menu_.push(guide_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "SUB CHANNEL"; it.value = [this] { return subch_label(); };
        it.enabled = media_ok_ || !psip_.channels().empty();
        it.activate = [this] { menu_.push(programs_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "AUDIO TRACK"; it.value = [this] { return audio_value(); };
        it.enabled = media_ok_ && pl_.audio_stream() >= 0;
        it.activate = [this] { menu_.push(audio_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "SCREEN SCALE";
        it.value = [this] { return std::string(kScaleNames[set_.scale]); };
        it.adjust = [this](int d) { set_.scale = (set_.scale + d + SCALE_COUNT) % SCALE_COUNT; save(); };
        pg.items.push_back(it);
        it = {}; it.label = "CLOSED CAPTION"; it.value = [] { return std::string("OFF"); };
        it.enabled = false; it.note = "SOON";
        pg.items.push_back(it);
        it = {}; it.label = "RECORD/TIMER"; it.enabled = false; it.note = "SOON";
        pg.items.push_back(it);
        it = {}; it.label = "SET UP FEATURE";
        it.activate = [this] { menu_.push(feature_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "SET UP CHANNEL";
        it.activate = [this] { menu_.push(setup_channel_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "FIRMWARE SETUP";
        it.activate = [this] { menu_.push(firmware_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "KEYBOARD SHORTCUTS";
        it.activate = [this] { menu_.push(shortcuts_page()); };
        pg.items.push_back(it);
        it = {}; it.label = "ABOUT";
        it.activate = [this] { menu_.push(about_page()); };
        pg.items.push_back(it);
    };
    return p;
}

osd::Page App::channels_page() {
    osd::Page p;
    p.title = "CHANNELS";
    p.hints = {"SELECT:\xE2\x96\xB2\xE2\x96\xBC KEY", "TUNE  :\xE2\x96\xBA KEY",
               "FAV   :SPACE KEY", "END   :ESC KEY"};
    p.sel = std::max(0, current_vchan());
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        pg.body.clear();
        if (vchans_.empty()) {
            pg.body = {"NO CHANNELS MEMORIZED"};
            osd::Item it;
            it.label = "AUTO CH MEMORY";
            it.activate = [this] { do_scan(); };
            pg.items.push_back(it);
            return;
        }
        int cur = current_vchan();
        for (size_t i = 0; i < vchans_.size(); ++i) {
            const VChan& v = vchans_[i];
            std::string num = vchan_num(v), name = v.major ? v.name : fmt("%.0fMHZ", v.freq / 1e6);
            num.resize(std::max<size_t>(num.size() + 1, 6), ' ');
            name.resize(std::max<size_t>(name.size() + 1, 8), ' ');
            osd::Item it;
            it.label = num + name + fmt("RF%-3d%4.1fDB", v.rf, v.snr);
            it.marked = (int)i == cur;
            if (is_fav(v)) it.note = "FAV";
            VChan copy = v;
            it.activate = [this, copy] { menu_.close(); tune_vchan(copy); };
            pg.items.push_back(it);
        }
    };
    p.on_key = [this](SDL_Keycode k) {
        osd::Page* pg = menu_.top();
        if (k != SDLK_SPACE || !pg || vchans_.empty()) return false;
        const VChan& v = vchans_[(size_t)std::clamp(pg->sel, 0, (int)vchans_.size() - 1)];
        if (is_fav(v)) { set_.favs.erase(fav_key(v)); set_.favs.erase(v.rf); }
        else set_.favs.insert(fav_key(v));
        save();
        return true;
    };
    return p;
}
osd::Page App::guide_page() {
    osd::Page p;
    p.title = "PROGRAM GUIDE";
    p.hints = {"SELECT:\xE2\x96\xB2\xE2\x96\xBC KEY", "OPEN  :\xE2\x96\xBA KEY", "END   :ESC KEY"};
    auto chans = guide_channels();
    for (size_t i = 0; i < chans.size(); ++i)
        if (chans[i]->program_number == current_program()) p.sel = (int)i;
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        pg.body.clear();
        int64_t now = psip_.now_utc();
        std::time_t tt = (std::time_t)now;
        std::tm lt{};
        localtime_r(&tt, &lt);
        char date[16];
        std::strftime(date, sizeof(date), "%a %b %d", &lt);
        pg.body.push_back(upper(date) + "  " + clock_hm(now) + fmt("   RF %d", channel_));
        auto chans = guide_channels();
        if (chans.empty()) {
            pg.body.push_back("");
            if (media_ok_) {
                pg.body.push_back("COLLECTING GUIDE DATA" + std::string((size_t)(SDL_GetTicks() / 500 % 4), '.'));
                pg.body.push_back(fmt("PSIP SECTIONS: %llu", (unsigned long long)psip_.sections_ok()));
            } else {
                pg.body.push_back("NO SIGNAL - NO GUIDE DATA");
            }
            return;
        }
        for (const psip::VirtualChannel* vc : chans) {
            const psip::Event* e = psip_.at(vc->source_id, now);
            std::string head = fmt("%d-%d", vc->major, vc->minor);
            head.resize(std::max<size_t>(head.size() + 1, 6), ' ');
            std::string name = vcr_text(vc->name, 7);
            name.resize(8, ' ');
            osd::Item it;
            it.label = head + name + (e ? vcr_text(e->title, 18) : std::string("(NO LISTINGS)"));
            it.marked = vc->program_number == current_program();
            int src = vc->source_id;
            it.activate = [this, src] { menu_.push(guide_channel_page(src)); };
            pg.items.push_back(it);
        }
    };
    return p;
}

osd::Page App::guide_channel_page(int source_id) {
    osd::Page p;
    const psip::VirtualChannel* vc0 = psip_.by_source(source_id);
    p.title = vc0 ? vc_label(*vc0) : "GUIDE";
    p.hints = {"SELECT:\xE2\x96\xB2\xE2\x96\xBC KEY", "INFO  :ENTER KEY",
               "SUB CH:\xE2\x97\x84\xE2\x96\xBA KEY", "END   :ESC KEY"};
    p.build = [this, source_id](osd::Page& pg) {
        pg.items.clear();
        pg.body.clear();
        const psip::VirtualChannel* vc = psip_.by_source(source_id);
        if (vc && !vc->description.empty()) pg.body.push_back(vcr_text(vc->description, 33));
        if (vc && vc->program_number != current_program()) {
            osd::Item w;
            w.label = "WATCH " + vc_label(*vc);
            int pn = vc->program_number;
            w.activate = [this, pn] { menu_.close(); watch_program(pn); };
            pg.items.push_back(w);
        }
        int64_t now = psip_.now_utc();
        for (const psip::Event& e : psip_.events(source_id, now)) {
            osd::Item it;
            bool on = e.start <= now && now < e.end();
            it.label = clock_hm(e.start) + " " + vcr_text(e.title, 24);
            it.marked = on;
            int mins = e.duration / 60;
            it.note = on ? "ON" : (mins % 60 ? (mins >= 60 ? fmt("%dH%02d", mins / 60, mins % 60) : fmt("%dM", mins))
                                             : fmt("%dH", mins / 60));
            int eid = e.event_id;
            it.activate = [this, source_id, eid] { menu_.push(event_page(source_id, eid)); };
            pg.items.push_back(it);
        }
        if (psip_.events(source_id, now).empty()) pg.body.push_back("NO LISTINGS YET");
    };
    p.on_key = [this, source_id](SDL_Keycode k) {
        if (k != SDLK_LEFT && k != SDLK_RIGHT) return false;
        auto chans = guide_channels();
        if (chans.size() < 2) return true;
        size_t i = 0;
        while (i < chans.size() && chans[i]->source_id != source_id) ++i;
        size_t n = chans.size();
        size_t j = k == SDLK_RIGHT ? (i + 1) % n : (i + n - 1) % n;
        int next = chans[j]->source_id;
        menu_.pop();
        menu_.push(guide_channel_page(next));
        return true;
    };
    return p;
}

osd::Page App::event_page(int source_id, int event_id) {
    osd::Page p;
    p.title = "PROGRAM INFO";
    p.hints = {"END   :ESC KEY"};
    p.build = [this, source_id, event_id](osd::Page& pg) {
        pg.items.clear();
        pg.body.clear();
        const psip::VirtualChannel* vc = psip_.by_source(source_id);
        const psip::Event* ev = nullptr;
        std::vector<psip::Event> evs = psip_.events(source_id);
        for (const auto& e : evs) if (e.event_id == event_id) ev = &e;
        if (!ev) { pg.body = {"EVENT NO LONGER LISTED"}; return; }
        for (const auto& l : wrap(vcr_text(ev->title), 33)) pg.body.push_back(l);
        std::time_t tt = (std::time_t)ev->start;
        std::tm lt{};
        localtime_r(&tt, &lt);
        char day[8];
        std::strftime(day, sizeof(day), "%a", &lt);
        pg.body.push_back(upper(day) + " " + clock_hm(ev->start) + "-" + clock_hm(ev->end()) +
                          fmt("  %d MIN", ev->duration / 60));
        std::string meta = vc ? vc_label(*vc) : "";
        if (!ev->rating.empty()) meta += (meta.empty() ? "" : "  ") + vcr_text(ev->rating, 12);
        if (!meta.empty()) pg.body.push_back(meta);
        if (!ev->description.empty()) {
            pg.body.push_back("");
            auto lines = wrap(vcr_text(ev->description), 33);
            const size_t max_lines = 5;
            if (lines.size() > max_lines) {
                lines.resize(max_lines);
                std::string& last = lines.back();
                if (last.size() > 30) last.resize(30);
                last += "...";
            }
            for (const auto& l : lines) pg.body.push_back(l);
        }
        if (vc && vc->program_number != current_program()) {
            osd::Item w;
            w.label = "WATCH " + vc_label(*vc);
            int pn = vc->program_number;
            w.activate = [this, pn] { menu_.close(); watch_program(pn); };
            pg.items.push_back(w);
        }
    };
    return p;
}

osd::Page App::programs_page() {
    osd::Page p;
    p.title = "SUB CHANNEL";
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        pg.body.clear();
        auto add = [&](int pn, const std::string& detail, const std::string& note) {
            osd::Item it;
            const psip::VirtualChannel* vc = psip_.by_program(pn);
            std::string head = vc ? vc_label(*vc) : fmt("%d PROG %d", channel_, pn);
            head.resize(std::max<size_t>(head.size() + 1, 14), ' ');
            it.label = head + detail;
            it.note = note;
            it.marked = pn == current_program();
            it.activate = [this, pn] { watch_program(pn); };
            pg.items.push_back(it);
            if (it.marked) pg.sel = (int)pg.items.size() - 1;
        };
        if (media_ok_) {
            const AVFormatContext* fc = pl_.format();
            for (const AVProgram* pr : video_programs(fc)) {
                auto v = streams_of_type(fc, pr, AVMEDIA_TYPE_VIDEO);
                auto a = streams_of_type(fc, pr, AVMEDIA_TYPE_AUDIO);
                const AVCodecParameters* cp = fc->streams[v[0]]->codecpar;
                add(pr->id, fmt("%dX%d", cp->width, cp->height), fmt("%zu AUD", a.size()));
            }
        } else {
            for (const auto& c : psip_.channels()) if (!c.hidden) add(c.program_number, "", "");
        }
        if (pg.items.empty()) pg.body = {"NO PROGRAM TABLE"};
    };
    return p;
}

osd::Page App::audio_page() {
    osd::Page p;
    p.title = "AUDIO TRACK";
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        if (!media_ok_) return;
        const AVFormatContext* fc = pl_.format();
        auto aud = streams_of_type(fc, find_program(fc, pl_.program()), AVMEDIA_TYPE_AUDIO);
        for (size_t i = 0; i < aud.size(); ++i) {
            osd::Item it;
            it.label = fmt("%zu  ", i + 1) + audio_label(fc->streams[aud[i]]);
            it.marked = aud[i] == pl_.audio_stream();
            int idx = aud[i];
            it.activate = [this, idx] {
                if (idx == pl_.audio_stream()) return;
                pl_.select_audio(idx);
                apply_gain();
            };
            pg.items.push_back(it);
            if (it.marked) pg.sel = (int)i;
        }
    };
    return p;
}

osd::Page App::feature_page() {
    osd::Page p;
    p.title = "SET UP FEATURE";
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        osd::Item it;
        it = {}; it.label = "SCREEN SCALE";
        it.value = [this] { return std::string(kScaleNames[set_.scale]); };
        it.adjust = [this](int d) { set_.scale = (set_.scale + d + SCALE_COUNT) % SCALE_COUNT; save(); };
        pg.items.push_back(it);
        it = {}; it.label = "HUD";
        it.value = [this] { return std::string(set_.hud ? "ON" : "OFF"); };
        it.adjust = [this](int) { set_hud(!set_.hud); };
        pg.items.push_back(it);
        it = {}; it.label = "SIGNAL INFO";
        it.value = [this] { return std::string(set_.info ? "ON" : "OFF"); };
        it.adjust = [this](int) { toggle_info(); };
        pg.items.push_back(it);
        it = {}; it.label = "SCANLINES";
        it.value = [this] { return std::string(kScanNames[set_.scanlines]); };
        it.adjust = [this](int d) { set_.scanlines = (set_.scanlines + d + SCAN_COUNT) % SCAN_COUNT; save(); };
        pg.items.push_back(it);
        it = {}; it.label = "CLOCK";
        it.value = [this] { return std::string(set_.clock24 ? "24H" : "12H"); };
        it.adjust = [this](int) { set_.clock24 = !set_.clock24; save(); };
        pg.items.push_back(it);
        it = {}; it.label = "VOLUME";
        it.value = [this] {
            int n = (int)std::lround(set_.volume * 10);
            return std::string((size_t)std::min(n, 10), '|') +
                   std::string((size_t)std::max(0, 10 - n), '-') + (n > 10 ? "+" : "");
        };
        it.adjust = [this](int d) {
            set_.volume = std::clamp(std::round((set_.volume + d * 0.1f) * 10) / 10, 0.0f, 1.5f);
            muted_ = false; apply_gain(); save();
        };
        pg.items.push_back(it);
        it = {}; it.label = "CH SURF";
        it.value = [this] { return std::string(set_.fav_only ? "FAVORITES" : "ALL"); };
        it.adjust = [this](int) { set_.fav_only = !set_.fav_only; save(); };
        pg.items.push_back(it);
        it = {}; it.label = "PARENTAL LOCK"; it.value = [] { return std::string("OFF"); };
        it.enabled = false; it.note = "SOON";
        pg.items.push_back(it);
    };
    return p;
}

osd::Page App::setup_channel_page() {
    osd::Page p;
    p.title = "SET UP CHANNEL";
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        osd::Item it;
        it = {}; it.label = "COUNTRY"; it.value = [] { return std::string("USA"); };
        pg.items.push_back(it);
        it = {}; it.label = "SERVICE"; it.value = [] { return std::string("ANTENNA"); };
        pg.items.push_back(it);
        it = {}; it.label = "SCAN RANGE";
        it.value = [this] { return fmt("CH %d-%d", hibiki::ATSC_CH_MIN, set_.scan_max); };
        it.adjust = [this](int) {
            set_.scan_max = set_.scan_max == hibiki::ATSC_CH_MAX ? hibiki::ATSC_CH_MAX_POST_REPACK
                                                                 : hibiki::ATSC_CH_MAX;
            save();
        };
        pg.items.push_back(it);
        it = {}; it.label = "MEMORIZED"; it.value = [this] { return fmt("%zu CH", scanned_.size()); };
        pg.items.push_back(it);
        it = {}; it.label = "AUTO CH MEMORY";
        it.activate = [this] { do_scan(); };
        pg.items.push_back(it);
        it = {}; it.label = "CLEAR FAVORITES";
        it.enabled = !set_.favs.empty();
        it.activate = [this] { set_.favs.clear(); save(); toast("FAVORITES CLEARED"); };
        pg.items.push_back(it);
    };
    return p;
}

osd::Page App::about_page() {
    verify_firmware();
    osd::Page p;
    p.title = "ABOUT HIBIKI";
    p.hints = {"UP/DOWN:SELECT  ENTER:OPEN", "END:ESC KEY"};
    p.build = [this](osd::Page& pg) {
        int sdl = SDL_GetVersion();
        pg.body = {fmt("HIBIKI TV  V%s", VERSION),
                   "INDEPENDENT ATSC TELEVISION",
                   "MYGICA / GENIATECH EYETV STICK",
                   opt_.demo ? "TUNER: DEMO MODE" : !opt_.ts_file.empty() ? "TUNER: CAPTURE REPLAY" : "TUNER: USB 1F4D:E691",
                   "VIDEO: " + res_label(),
                   fmt("PSIP: %zu CHANNELS / %zu EVENTS", psip_.channels().size(), psip_.event_count()),
                   fmt("SDL %d.%d.%d / FFMPEG %s", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl), av_version_info()),
                   std::string("FW: EAGLE ") + (fw_status_[0].verified ? "OK" : "--") + " / FX2 " + (fw_status_[1].verified ? "OK" : "--")};
        pg.items.clear();
        osd::Item item;
        item.label = "PROJECT / SOURCE";
        item.activate = [] { SDL_OpenURL("https://github.com/crux161/hibiki"); };
        pg.items.push_back(item);
        item = {}; item.label = "FIRMWARE SETUP";
        item.activate = [this] { menu_.push(firmware_page()); };
        pg.items.push_back(item);
        item = {}; item.label = "KEYBOARD SHORTCUTS";
        item.activate = [this] { menu_.push(shortcuts_page()); };
        pg.items.push_back(item);
    };
    return p;
}

// ---- drawing ---------------------------------------------------------------
SDL_FRect App::video_rect(float sw, float sh, float aspect, SDL_FRect* src) const {
    float W = (float)osd_.width(), H = (float)osd_.height();
    *src = SDL_FRect{0, 0, sw, sh};
    if (set_.scale == SCALE_STRETCH || aspect <= 0) return SDL_FRect{0, 0, W, H};
    float out_aspect = W / H;
    if (set_.scale == SCALE_FIT) {
        if (aspect > out_aspect) { float h = W / aspect; return SDL_FRect{0, (H - h) / 2, W, h}; }
        float w = H * aspect;
        return SDL_FRect{(W - w) / 2, 0, w, H};
    }
    // FULL: fill the window, crop the overflow from the source.
    if (aspect > out_aspect) {
        float keep = sw * out_aspect / aspect;
        src->x = (sw - keep) / 2; src->w = keep;
    } else {
        float keep = sh * aspect / out_aspect;
        src->y = (sh - keep) / 2; src->h = keep;
    }
    return SDL_FRect{0, 0, W, H};
}

void App::draw_picture() {
    SDL_FRect src, dst;
    if (media_ok_ && pl_.texture()) {
        dst = video_rect((float)pl_.width(), (float)pl_.height(), (float)pl_.display_aspect(), &src);
        SDL_RenderTexture(ren_, pl_.texture(), &src, &dst);
        if (signal_lost() && SDL_GetTicks() / 700 % 2 == 0)   // frozen picture: say why
            osd_.text(osd::Font::Menu, "NO SIGNAL", osd_.width() / 2.0f, osd_.height() * 0.47f,
                      osd::kWhite, osd::Align::Center, osd::Style::Vhs);
        return;
    }
    if (opt_.demo && bars_ && tuning_.empty() && scanned_index(channel_) >= 0) {
        dst = video_rect(640, 480, 4.0f / 3.0f, &src);
        SDL_RenderTexture(ren_, bars_, &src, &dst);
        return;
    }
    // VCR "blue back": no picture -> solid blue screen.
    osd_.rect(0, 0, (float)osd_.width(), (float)osd_.height(), osd::kVcrBlue);
    float H = (float)osd_.height(), W = (float)osd_.width();
    if (!tuning_.empty()) {
        int dots = (int)(SDL_GetTicks() / 400 % 4);
        osd_.text(osd::Font::Menu, tuning_ + std::string((size_t)dots, '.'),
                  W / 2 - osd_.text_w(osd::Font::Menu, tuning_) / 2, H * 0.47f,
                  osd::kWhite, osd::Align::Left, osd::Style::Vhs);
    } else if (SDL_GetTicks() / 700 % 2 == 0 || menu_.open()) {
        osd_.text(osd::Font::Menu, "NO SIGNAL", W / 2, H * 0.47f, osd::kWhite,
                  osd::Align::Center, osd::Style::Vhs);
    }
}

void App::draw_hud() {
    using osd::Font; using osd::Align; using osd::Style;
    const float W = (float)osd_.width(), H = (float)osd_.height();
    const float mx = std::round(W * 0.06f), my = std::round(H * 0.07f);
    const float sh = osd_.line_h(Font::Small);
    const uint64_t now = SDL_GetTicks();
    const bool banner = now < banner_until_ || set_.hud;

    if (banner) {
        // Top-left: transport status / channel / frequency (VHS "PLAY" corner).
        bool playing = (media_ok_ && !signal_lost()) || (opt_.demo && scanned_index(channel_) >= 0);
        std::string status = !tuning_.empty() ? "TUNE"
                           : !playing ? "NO SIGNAL"
                           : media_ok_ && !pl_.has_frame() ? "WAIT" : "PLAY \xE2\x96\xBA";
        osd_.text(Font::Small, status, mx, my, osd::kWhite, Align::Left, Style::Vhs);
        float y = my + sh;
        float big = osd_.line_h(Font::Big);
        // With PSIP: virtual channel number + station name and what is on now.
        const psip::VirtualChannel* vc = current_vc();
        int vi = current_vchan();
        std::string num = vc ? fmt("CH %d-%d", vc->major, vc->minor)
                        : vi >= 0 && vchans_[vi].major ? "CH " + vchan_num(vchans_[vi])
                                                       : fmt("CH %d", channel_);
        osd_.text(Font::Big, num, mx, y, osd::kWhite, Align::Left, Style::Vhs);
        y += big * 0.92f;
        std::string name = vc ? vcr_text(vc->name, 7)
                         : vi >= 0 && vchans_[vi].major ? vchans_[vi].name : std::string();
        std::string line = name.empty() ? fmt("RF %d  %.2f MHZ", channel_, freq_ / 1e6)
                                        : name + fmt("  RF %d", channel_);
        if (vi >= 0 && is_fav(vchans_[vi])) line += "  FAV";
        osd_.text(Font::Small, line, mx, y, osd::kWhite, Align::Left, Style::Vhs);
        y += sh;
        if (vc) {
            int64_t t_now = psip_.now_utc();
            if (const psip::Event* e = psip_.at(vc->source_id, t_now)) {
                osd_.text(Font::Small, clock_hm(e->start) + "-" + clock_hm(e->end()) +
                              (e->rating.empty() ? "" : "  " + vcr_text(e->rating, 8)),
                          mx, y, osd::kVcrYellow, Align::Left, Style::Vhs);
                osd_.text(Font::Small, vcr_text(e->title, 30), mx, y + sh, osd::kVcrYellow,
                          Align::Left, Style::Vhs);
            }
        }

        // Top-right: date and clock.
        std::time_t t = std::time(nullptr);
        std::tm lt{};
        localtime_r(&t, &lt);
        char date[32], clk[32];
        std::strftime(date, sizeof(date), "%b %d.%y", &lt);
        std::strftime(clk, sizeof(clk), set_.clock24 ? "%H:%M:%S" : "%I:%M:%S %p", &lt);
        osd_.text(Font::Small, upper(date), W - mx, my, osd::kWhite, Align::Right, Style::Vhs);
        osd_.text(Font::Small, clk, W - mx, my + sh, osd::kWhite, Align::Right, Style::Vhs);
    }

    // Bottom-left: volume, shown briefly after a change ("VOLUME |||||||--").
    if (now < vol_until_ || muted_) {
        float y = H - my - sh * 2;
        osd_.text(Font::Small, muted_ ? "MUTE" : "VOLUME", mx, y, osd::kWhite, Align::Left, Style::Vhs);
        if (!muted_)
            osd_.meter(mx, y + sh * 1.05f, osd_.cell_w(Font::Small) * 15, sh * 0.62f, 15,
                       set_.volume / 1.5f, osd::kWhite, osd::kWhite);
    }

    // Bottom-right: independently toggled signal and playback information.
    if (set_.info) {
        float y = H - my - sh * 2;
        if (st_ok_) {
            float snr = st_.snr_db_tenths / 10.0f;
            std::string s = fmt("SIG %4.1fDB %s", snr, st_.locked() ? "LOCK" : "----");
            osd_.text(Font::Small, s, W - mx, y, st_.locked() ? osd::kWhite : osd::kRed,
                      Align::Right, Style::Vhs);
            float mw = osd_.cell_w(Font::Small) * 12;
            osd_.meter(W - mx - mw, y + sh * 1.05f, mw, sh * 0.62f, 12, snr / 35.0f,
                       snr >= 15 ? osd::kGreen : osd::kRed, osd::kWhite);
            osd_.text(Font::Small, fmt("FRM:%d ATSC:%d FEC:%d", st_.frame_lock,
                                       st_.atsc_lock, st_.fec_lock),
                      W - mx, y - sh, osd::kWhite, Align::Right, Style::Vhs, 200);
        } else {
            osd_.text(Font::Small, opt_.ts_file.empty() ? "SIG ---" : "SIG --- REPLAY", W - mx, y,
                      osd::kWhite, Align::Right, Style::Vhs);
        }
        if (media_ok_) {   // playback diagnostics
            const player::Stats& ps = pl_.stats();
            const player::VideoInfo& vi = pl_.info();
            osd_.text(Font::Small, fmt("AV %+.3fS BUF %.2fS DROP %llu", ps.av_offset,
                                       ps.audio_latency, (unsigned long long)ps.frames_dropped),
                      W - mx, y - sh * 2, osd::kWhite, Align::Right, Style::Vhs, 200);
            std::string field = vi.field_order_observed != "unknown"
                                    ? vi.field_order_observed : vi.field_order;
            osd_.text(Font::Small, fmt("%s %dX%d %s", upper(vi.codec).c_str(),
                                       vi.active_width, vi.active_height,
                                       field_tag(field).c_str()),
                      W - mx, y - sh * 3, osd::kWhite, Align::Right, Style::Vhs, 200);
            osd_.text(Font::Small, fmt("DEC %.2f OUT %.2f FPS", ps.decoded_fps,
                                       ps.output_fps),
                      W - mx, y - sh * 4, osd::kWhite, Align::Right, Style::Vhs, 200);
            osd_.text(Font::Small, fmt("PROC %s %.1f MS", upper(vi.processing).c_str(),
                                       ps.decode_ms + ps.process_ms + ps.upload_ms),
                      W - mx, y - sh * 5, osd::kWhite, Align::Right, Style::Vhs, 200);
            osd_.text(Font::Small, fmt("Q V %.2f P %.2f A %.2f S", ps.queue_video_s,
                                       ps.queue_packets_s, ps.audio_latency),
                      W - mx, y - sh * 6, osd::kWhite, Align::Right, Style::Vhs, 200);
            osd_.text(Font::Small, fmt("DEC %s>%s", upper(vi.decoder_requested).c_str(),
                                       upper(vi.decoder_active).c_str()),
                      W - mx, y - sh * 7, osd::kWhite, Align::Right, Style::Vhs, 200);
            osd_.text(Font::Small, fmt("REN %s VSYNC %s", upper(rend_name_).c_str(),
                                       upper(vsync_active_).c_str()),
                      W - mx, y - sh * 8, osd::kWhite, Align::Right, Style::Vhs, 200);
        }
    }

    // Direct channel entry ("CH 1-").
    if (!digits_.empty()) {
        std::string d = "CH " + digits_ + (digits_.find('-') == std::string::npos ? "-" : "_");
        osd_.text(Font::Big, d, W - mx, my + sh * 2.2f, osd::kVcrYellow, Align::Right, Style::Vhs);
    }
}

void App::maybe_screenshot() {
    uint64_t now = SDL_GetTicks();
    int shot_after = opt_.shot_after_ms >= 0 ? opt_.shot_after_ms : (opt_.demo ? 1200 : 6000);
    if (!opt_.screenshot_path.empty() && !shot_saved_ && now - start_ms_ > (uint64_t)shot_after &&
        (pl_.has_frame() || opt_.demo || (!media_ok_ && tuning_.empty()))) {
        if (SDL_Surface* s = SDL_RenderReadPixels(ren_, nullptr)) {
            SDL_SaveBMP(s, opt_.screenshot_path.c_str());
            std::printf("[+] screenshot saved: %s\n", opt_.screenshot_path.c_str());
            SDL_DestroySurface(s);
        }
        shot_saved_ = true;
        if (opt_.demo || opt_.setup) hibiki::g_running = false;
    }
}

void App::present() {
    uint64_t present_now = SDL_GetTicks();
    if (last_present_)
        present_gap_max_ms_ = std::max<uint64_t>(present_gap_max_ms_,
                                                 present_now - last_present_);
    osd_.begin_frame();
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    draw_picture();
    if (menu_.open()) {
        // Rebuild twice a second so live text (signal, counts) stays current.
        if (SDL_GetTicks() - menu_refresh_ms_ >= 500) {
            menu_refresh_ms_ = SDL_GetTicks();
            menu_.refresh();
        }
        menu_.draw(osd_);
    } else {
        draw_hud();
    }
    if (!menu_.open() && SDL_GetTicks() < toast_until_ && !toast_.empty()) {
        float W = (float)osd_.width(), H = (float)osd_.height();
        osd_.text(osd::Font::Small, toast_, W / 2, H * 0.62f, osd::kVcrYellow,
                  osd::Align::Center, osd::Style::Vhs);
    }
    if (menu_.open() && SDL_GetTicks() < vol_until_) {
        osd_.text(osd::Font::Small, muted_ ? "MUTE" : fmt("VOLUME %d%%", (int)std::lround(set_.volume * 100)),
                  osd_.width() * 0.94f, osd_.height() * 0.015f, osd::kWhite,
                  osd::Align::Right, osd::Style::Vhs);
    }
    if (!menu_.open() && SDL_GetTicks() < legend_until_)
        osd_.text(osd::Font::Small, "? HELP  G GUIDE  H HUD  I SIGNAL INFO", osd_.width() * 0.5f,
                  osd_.height() * 0.96f, osd::kWhite, osd::Align::Center, osd::Style::Vhs);
    if (set_.scanlines == SCAN_ALL) osd_.scanlines(55);
    osd_.end_frame();

    maybe_screenshot();
    SDL_RenderPresent(ren_);
    last_present_ = SDL_GetTicks();
}

void App::draw_scan(int ch, uint32_t f, int done, int total, bool recheck) {
    using osd::Font; using osd::Align;
    osd_.begin_frame();
    const float W = (float)osd_.width(), H = (float)osd_.height();
    const float lh = osd_.line_h(Font::Menu), cw = osd_.cell_w(Font::Menu);
    osd_.rect(0, 0, W, H, osd::kVcrBlue);
    int cols = std::min(36, (int)(W * 0.9f / cw));
    float x0 = std::round((W - cols * cw) / 2), y = std::round(H * 0.08f);
    std::string t = recheck ? " RECHECK " : " AUTO CH MEMORY ";
    int dash = std::max(2, (cols - (int)t.size()) / 2);
    osd_.text(Font::Menu, std::string((size_t)dash, '-') + t + std::string((size_t)dash, '-'),
              W / 2, y, osd::kVcrYellow, Align::Center);
    y += lh * 2;
    osd_.text(Font::Menu, fmt("CH %2d   %7.2f MHZ", ch, f / 1e6), x0 + cw * 2, y, osd::kVcrYellow);
    y += lh * 1.5f;
    osd_.frame(x0 + cw * 2, y, (cols - 4) * cw, lh * 0.9f, std::max(2.0f, lh / 14), osd::kVcrYellow);
    osd_.meter(x0 + cw * 2.4f, y + lh * 0.18f, (cols - 4.8f) * cw, lh * 0.54f, 24,
               total ? (float)done / total : 0, osd::kVcrYellow, osd::kVcrBlue);
    y += lh * 1.6f;
    osd_.text(Font::Menu, fmt("FOUND : %zu", scanned_.size()), x0 + cw * 2, y, osd::kVcrYellow);
    y += lh;
    std::string list;
    for (const auto& e : scanned_) list += fmt("%d ", e.ch);
    if (list.size() > (size_t)cols - 4) list = "..." + list.substr(list.size() - (cols - 7));
    osd_.text(Font::Menu, list, x0 + cw * 2, y, osd::kVcrYellow);
    osd_.text(Font::Menu, "END   :ESC KEY", x0, H - H * 0.06f - lh, osd::kVcrYellow);
    if (set_.scanlines != SCAN_OFF) osd_.scanlines(60);
    osd_.end_frame();
    maybe_screenshot();
    SDL_RenderPresent(ren_);
}

// ---- main loop ---------------------------------------------------------------
int App::run() {
    SDL_SetAppMetadata("Hibiki", HIBIKI_VERSION, "org.hibiki.tv");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "mediaplayer");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) std::fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
    win_ = SDL_CreateWindow("Hibiki", opt_.win_w, opt_.win_h, SDL_WINDOW_RESIZABLE);
    if (!win_) {
        std::fprintf(stderr, "create window: %s\n", SDL_GetError());
        return 1;
    }
    ren_ = SDL_CreateRenderer(win_, nullptr);
    if (!ren_) {
        std::fprintf(stderr, "create renderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win_, 640, 480);
    {
        // Record the active presentation path separately from the requested
        // one, and request VSync explicitly so the result can be verified.
        const char* name = SDL_GetRendererName(ren_);
        rend_name_ = name ? name : "unknown";
        vsync_req_ = opt_.vsync;
        int wanted = vsync_req_ == "off" ? 0
                   : vsync_req_ == "adaptive" ? SDL_RENDERER_VSYNC_ADAPTIVE
                   : 1;
        bool requested = SDL_SetRenderVSync(ren_, wanted);
        int active = -1;
        bool known = SDL_GetRenderVSync(ren_, &active);
        vsync_active_ = !known ? "unknown"
                        : active == 0 ? "off"
                        : active == 1 ? "on"
                        : active == SDL_RENDERER_VSYNC_ADAPTIVE ? "adaptive"
                        : fmt("%d", active);
        int sdl_version = SDL_GetVersion();
        std::printf("[*] hibiki %s | SDL %d.%d.%d | FFmpeg %s | decoder=%s\n", VERSION,
                    SDL_VERSIONNUM_MAJOR(sdl_version),
                    SDL_VERSIONNUM_MINOR(sdl_version),
                    SDL_VERSIONNUM_MICRO(sdl_version), av_version_info(),
                    opt_.decoder == player::DecoderPreference::Cpu ? "cpu" : "auto");
        std::printf("[+] presentation: renderer=auto>%s vsync=%s>%s\n",
                    rend_name_.c_str(), vsync_req_.c_str(), vsync_active_.c_str());
        std::fflush(stdout);
        if (!requested)
            std::fprintf(stderr, "[!] vsync request '%s' failed: %s\n",
                         vsync_req_.c_str(), SDL_GetError());
    }
    app_event_ = SDL_RegisterEvents(1);
    hibiki::install_app_menu(app_event_);
    start_ms_ = SDL_GetTicks();

    std::string font_path = resolve_font(opt_.font_path, opt_.font_explicit);
    std::printf("[*] OSD font: %s\n", font_path.c_str());
    if (!osd_.init(ren_, font_path))
        std::fprintf(stderr, "[!] font load failed (%s); OSD text disabled\n", font_path.c_str());

    set_ = persist() ? load_settings() : Settings{};
    if (opt_.no_hud) set_.hud = set_.info = false;
    fw_path_ = resolve_fw(opt_.fw_path, opt_.fw_explicit);
    if (opt_.demo) {
        bars_ = make_color_bars(ren_);
        for (int c : {7, 9, 11, 14, 20, 27, 33}) {
            scanned_.push_back({c, hibiki::atsc_channel_freq_hz(c), 18.0f + c * 0.3f});
            read_vct(c);
        }
        set_.favs = {11 * 100000 + 1, 27 * 100000 + 2};
    } else if (!opt_.ts_file.empty()) {
        // Replay: the capture stands in for one memorised RF channel.
        int rf = opt_.channel;
        scanned_.push_back({rf, hibiki::atsc_channel_freq_hz(rf), 0.0f});
        std::printf("[*] replaying %s as RF %d\n", opt_.ts_file.c_str(), rf);
    } else {
        scanned_ = load_channels();
        for (const auto& e : scanned_) {
            psip::Decoder tmp;
            if (load_psip_cache(e.ch, tmp, 0, true)) rf_vct_[e.ch] = tmp.channels();
        }
        if (!opt_.setup && init_tuner() &&
            (opt_.rescan || (scanned_.empty() && !opt_.channel_explicit && !opt_.freq_hz))) do_scan();
    }
    if (!hibiki::g_running) { tuner_.Close(); return 0; }
    rebuild_vchans();

    // Start channel: --channel/--freq, else the last watched, else the first scanned.
    int ch = opt_.channel;
    uint32_t f = opt_.freq_hz;
    if (!opt_.channel_explicit && !f) {
        int last = scanned_index(set_.last_channel);
        if (last >= 0) { ch = scanned_[last].ch; f = scanned_[last].freq; }
        else if (!scanned_.empty()) { ch = scanned_[0].ch; f = scanned_[0].freq; }
    }
    if (!f) f = hibiki::atsc_channel_freq_hz(ch);
    if (!f) { std::fprintf(stderr, "invalid channel\n"); return 2; }
    if (!opt_.setup && (!media_ok_ || channel_ != ch)) tune_to(ch, f);

    if (opt_.start_menu) menu_.push(main_page());
    if (live() && !tuner_ok_ && !opt_.setup) menu_.push(tuner_error_page());
    if (opt_.start_page == "firmware") open_page(firmware_page());
    else if (opt_.start_page == "shortcuts") open_page(shortcuts_page());
    else if (opt_.start_page == "about") open_page(about_page());
    // Test hook: scripted key presses (SDL key names, comma separated; a
    // "+ms" token waits that long before the next key).
    std::deque<std::pair<uint64_t, SDL_Keycode>> script;
    if (!opt_.keys.empty()) {
        std::stringstream ss(opt_.keys);
        std::string name;
        uint64_t at = SDL_GetTicks();
        while (std::getline(ss, name, ',')) {
            if (name.size() > 1 && name[0] == '+' &&
                std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
                at += (uint64_t)std::atoi(name.c_str() + 1); continue;
            }
            SDL_Keycode k = scripted_key(name);
            if (k == SDLK_UNKNOWN) { std::fprintf(stderr, "[!] unknown key '%s'\n", name.c_str()); continue; }
            script.emplace_back(at, k);
        }
    }

    bool did_auto_change = false;
    auto print_stats = [&] {
        const player::Stats& ps = pl_.stats();
        const player::VideoInfo& vi = pl_.info();
        std::string field = vi.field_order_observed != "unknown"
                                ? vi.field_order_observed : vi.field_order;
        std::printf("[stats] av=%+.3f audio=%.3f shown=%llu dropped=%llu resyncs=%llu "
                    "decoded=%llu processed=%llu dec_fps=%.2f out_fps=%.2f "
                    "dec_ms=%.2f proc_ms=%.2f up_ms=%.2f "
                    "q_video=%.3f q_packets=%.3f size=%dx%d size_changes=%llu "
                    "field=%s proc=%s decoder=%s>%s renderer=auto>%s vsync=%s>%s "
                    "present_gap_ms=%llu psip_ch=%zu psip_ev=%zu\n",
                    ps.av_offset, ps.audio_latency,
                    (unsigned long long)ps.frames_shown,
                    (unsigned long long)ps.frames_dropped,
                    (unsigned long long)ps.resyncs,
                    (unsigned long long)ps.frames_decoded,
                    (unsigned long long)ps.frames_processed,
                    ps.decoded_fps, ps.output_fps,
                    ps.decode_ms, ps.process_ms, ps.upload_ms,
                    ps.queue_video_s, ps.queue_packets_s,
                    vi.active_width, vi.active_height,
                    (unsigned long long)vi.size_changes, field.c_str(),
                    vi.processing.c_str(), vi.decoder_requested.c_str(),
                    vi.decoder_active.c_str(), rend_name_.c_str(), vsync_req_.c_str(),
                    vsync_active_.c_str(),
                    (unsigned long long)present_gap_max_ms_,
                    psip_.channels().size(), psip_.event_count());
        std::fflush(stdout);
        present_gap_max_ms_ = 0;
    };
    while (hibiki::g_running) {
        uint64_t now = SDL_GetTicks();
        if (opt_.change_after > 0 && !did_auto_change &&
            now - start_ms_ > (uint64_t)opt_.change_after * 1000) {
            did_auto_change = true;
            std::printf("[*] test: auto channel change\n");
            change_channel(1);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            handle_event(ev);
        }
        while (!script.empty() && SDL_GetTicks() >= script.front().first) {
            SDL_Keycode k = script.front().second;
            script.pop_front();
            handle_key(k);
        }
        if (!hibiki::g_running) break;
        if (!digits_.empty() && SDL_GetTicks() - digits_ms_ > 1500) commit_digits();
        poll_import();
        poll_status();
        psip_housekeeping();

        bool dirty = false;
        if (media_ok_) {
            pl_.take_ts(ts_tap_);
            if (!ts_tap_.empty()) psip_.feed(ts_tap_.data(), ts_tap_.size());
            dirty = pl_.service();
            if (pl_.ended()) {
                if (!opt_.ts_file.empty()) {    // replay: loop the capture
                    tune_to(channel_, freq_, pl_.program());
                } else {
                    std::fprintf(stderr, "[-] stream ended\n");
                    pl_.close();
                    media_ok_ = false;
                }
            }
        }
        if (psip_.generation() != vchan_gen_) rebuild_vchans();
        if (opt_.stats && media_ok_ && SDL_GetTicks() - stats_ms_ >= 2000) {
            stats_ms_ = SDL_GetTicks();
            print_stats();
        }
        if (dirty || SDL_GetTicks() - last_present_ >= 33) present();
        else SDL_Delay((Uint32)std::max(1, media_ok_ ? pl_.idle_ms() : 5));
    }

    psip_housekeeping(true);
    if (opt_.stats && media_ok_) print_stats();   // final sample at shutdown
    pl_.close();
    if (ts_file_) std::fclose(ts_file_);
    if (bars_) SDL_DestroyTexture(bars_);
    tuner_.Close();
    osd_.release();
    SDL_DestroyRenderer(ren_);
    SDL_DestroyWindow(win_);
    TTF_Quit();
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { hibiki::g_running = false; });
    std::signal(SIGTERM, [](int) { hibiki::g_running = false; });

    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--channel" && (v = next())) { o.channel = std::atoi(v); o.channel_explicit = true; }
        else if (a == "--rescan") o.rescan = true;
        else if (a == "--change-after" && (v = next())) o.change_after = std::atoi(v);
        else if (a == "--menu") o.start_menu = true;
        else if (a == "--demo") o.demo = true;
        else if (a == "--setup") { o.setup = true; o.start_page = "firmware"; }
        else if (a == "--shortcuts") o.start_page = "shortcuts";
        else if (a == "--about") o.start_page = "about";
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a == "--ts-file" && (v = next())) o.ts_file = v;
        else if (a == "--stats") o.stats = true;
        else if (a == "--decoder" && (v = next())) {
            std::string s = upper(v);
            if (s == "AUTO") o.decoder = player::DecoderPreference::Auto;
            else if (s == "CPU") o.decoder = player::DecoderPreference::Cpu;
            else {
                std::fprintf(stderr, "unsupported decoder '%s' (auto|cpu)\n", v);
                return 2;
            }
        }
        else if (a == "--deinterlace" && (v = next())) {
            std::string s = upper(v);
            if (s == "PASSTHROUGH" || s == "OFF")
                o.processing.mode = video::ProcessingMode::PassThrough;
            else if (s == "AUTO") o.processing.mode = video::ProcessingMode::Auto;
            else if (s == "BWDIF") o.processing.mode = video::ProcessingMode::Bwdif;
            else if (s == "YADIF") o.processing.mode = video::ProcessingMode::Yadif;
            else if (s == "BOB") o.processing.mode = video::ProcessingMode::Bob;
            else if (s == "WEAVE") o.processing.mode = video::ProcessingMode::Weave;
            else {
                std::fprintf(stderr, "unsupported deinterlace mode '%s' "
                                     "(auto|bwdif|yadif|bob|weave|passthrough)\n", v);
                return 2;
            }
        }
        else if (a == "--field-order" && (v = next())) {
            std::string s = upper(v);
            if (s == "AUTO") o.processing.field_order = video::FieldOrder::Auto;
            else if (s == "TFF") o.processing.field_order = video::FieldOrder::TopFirst;
            else if (s == "BFF") o.processing.field_order = video::FieldOrder::BottomFirst;
            else {
                std::fprintf(stderr, "unsupported field order '%s' (auto|tff|bff)\n", v);
                return 2;
            }
        }
        else if (a == "--deinterlace-force") o.processing.force = true;
        else if (a == "--vsync" && (v = next())) {
            std::string s = upper(v);
            if (s == "ON" || s == "OFF" || s == "ADAPTIVE")
                o.vsync = s == "ON" ? "on" : s == "OFF" ? "off" : "adaptive";
            else {
                std::fprintf(stderr, "unsupported vsync mode '%s' (on|off|adaptive)\n", v);
                return 2;
            }
        }
        else if (a == "--keys" && (v = next())) o.keys = v;
        else if (a == "--country" && (v = next())) {
            if (upper(v) != "US") {
                std::fprintf(stderr, "only country 'us' is supported for now\n");
                return 2;
            }
        }
        else if (a == "--freq" && (v = next())) o.freq_hz = (uint32_t)std::strtoul(v, nullptr, 0);
        else if (a == "--fw" && (v = next())) { o.fw_path = v; o.fw_explicit = true; }
        else if (a == "--font" && (v = next())) { o.font_path = v; o.font_explicit = true; }
        else if (a == "--dump-ts" && (v = next())) o.dump_path = v;
        else if (a == "--screenshot" && (v = next())) o.screenshot_path = v;
        else if (a == "--shot-after" && (v = next())) o.shot_after_ms = std::atoi(v);
        else if (a == "--no-hud") o.no_hud = true;
        else if (a == "--width" && (v = next())) o.win_w = std::atoi(v);
        else if (a == "--height" && (v = next())) o.win_h = std::atoi(v);
        else if (a == "--trace") o.trace = true;
        else { usage(argv[0]); return 2; }
    }
    if (!o.dump_path.empty()) g_dump = std::fopen(o.dump_path.c_str(), "wb");
    App app(o);
    int rc = app.run();
    if (g_dump) std::fclose(g_dump);
    return rc;
}
