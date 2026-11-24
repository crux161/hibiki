// hibiki SDL front-end - live ATSC TV with video + audio + a VCR-style OSD.
//
// Pipeline: hibiki::Tuner (async TS reader) -> custom FFmpeg AVIO -> libavformat
// demux -> libavcodec decode -> SDL3 YUV texture (video) / libswresample ->
// SDL3 audio stream (audio). The on-screen display (osd.h) draws a VHS-style
// playback HUD and a blue VCR set-up menu over the picture.
//
// Decoding and drawing are decoupled: decoded frames only update the video
// texture, and the screen is redrawn on every new frame or at least every
// ~33 ms, so menus stay responsive with no picture (no signal, audio-only
// stretches, demo mode).
//
// Remote-style controls (see usage()):
//   Enter menu, Esc back/quit, Left/Right or PgUp/PgDn channel, Up/Down volume,
//   0-9 direct channel entry, C channel list, G guide, A audio track,
//   Z screen scale, I info, H status HUD, F fullscreen, M mute, S scan.

#include "hibiki/tuner.h"
#include "osd.h"
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
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int AUDIO_RATE = 48000;
constexpr int AUDIO_CH = 2;
const char* DEFAULT_FONT = "/System/Library/Fonts/Supplemental/Arial.ttf";
const char* VERSION = "0.1.0";

// Optional raw-TS capture of exactly what the demuxer receives (diagnostics).
FILE* g_dump = nullptr;

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
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

// ---------------------------------------------------------------------------
// FFmpeg AVIO backed by the tuner's TS ring. Every byte the demuxer reads is
// also fed to the PSIP decoder, since libavformat ignores ATSC PSIP.
struct TsSource {
    hibiki::Tuner* tuner = nullptr;
    psip::Decoder* psip = nullptr;
};

int ts_read(void* opaque, uint8_t* buf, int size) {
    auto* src = static_cast<TsSource*>(opaque);
    while (hibiki::g_running) {
        int got = src->tuner->ReadTS(buf, (uint32_t)size);
        if (got > 0) {
            if (g_dump) std::fwrite(buf, 1, (size_t)got, g_dump);
            if (src->psip) src->psip->feed(buf, (size_t)got);
            return got;
        }
    }
    return AVERROR_EOF;
}

// ---------------------------------------------------------------------------
// Media (demux + decode) state, rebuilt on every channel change. A broadcast
// RF channel carries several sub-channels (MPEG programs); `program` is the
// one being decoded and can be switched without re-probing.
struct Media {
    AVFormatContext* fmt = nullptr;
    int vstream = -1, astream = -1;
    int program = -1;                 // AVProgram id, -1 = no program table
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* aframe = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* yuvframe = nullptr;      // sws target when input isn't yuv420p
    SwrContext* swr = nullptr;
    uint8_t* audio_out = nullptr;
    int audio_out_cap = 0;            // bytes
    SDL_Texture* vtex = nullptr;
    SDL_AudioStream* audio = nullptr;
    int width = 0, height = 0;
    AVRational sar{0, 1};
};

void close_video(Media& m) {
    if (m.vtex) { SDL_DestroyTexture(m.vtex); m.vtex = nullptr; }
    if (m.yuvframe) av_frame_free(&m.yuvframe);
    if (m.sws) sws_freeContext(m.sws);
    m.sws = nullptr;
    if (m.vframe) av_frame_free(&m.vframe);
    if (m.vdec) avcodec_free_context(&m.vdec);
    m.vstream = -1;
    m.width = m.height = 0;
    m.sar = AVRational{0, 1};
}

void close_audio(Media& m) {
    if (m.audio) { SDL_DestroyAudioStream(m.audio); m.audio = nullptr; }
    if (m.swr) swr_free(&m.swr);
    if (m.audio_out) { av_free(m.audio_out); m.audio_out = nullptr; }
    m.audio_out_cap = 0;  // else push_audio_frame reuses a freed buffer
    if (m.aframe) av_frame_free(&m.aframe);
    if (m.adec) avcodec_free_context(&m.adec);
    m.astream = -1;
}

void close_media(Media& m) {
    close_video(m);
    close_audio(m);
    if (m.fmt) {
        // The avio buffer is owned by the AVIOContext; free via avio_context_free.
        if (m.fmt->pb) avio_context_free(&m.fmt->pb);
        avformat_close_input(&m.fmt);
    }
    m.program = -1;
}

AVCodecContext* open_decoder(const AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

bool open_video(Media& m, SDL_Renderer* ren, int idx) {
    const AVStream* st = m.fmt->streams[idx];
    m.vdec = open_decoder(st);
    if (!m.vdec) { std::fprintf(stderr, "[-] video decoder open failed\n"); return false; }
    m.vstream = idx;
    m.vframe = av_frame_alloc();
    m.width = m.vdec->width;
    m.height = m.vdec->height;
    m.sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio
                                        : st->codecpar->sample_aspect_ratio;
    m.vtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
                               SDL_TEXTUREACCESS_STREAMING, m.width, m.height);
    if (!m.vtex) { std::fprintf(stderr, "[-] SDL texture: %s\n", SDL_GetError()); return false; }
    return true;
}

bool open_audio(Media& m, int idx) {
    m.adec = open_decoder(m.fmt->streams[idx]);
    if (!m.adec) return false;
    m.astream = idx;
    m.aframe = av_frame_alloc();
    // Resample to interleaved float, stereo, 48 kHz for SDL.
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int ret = swr_alloc_set_opts2(&m.swr,
        &out_layout, AV_SAMPLE_FMT_FLT, AUDIO_RATE,
        &m.adec->ch_layout, m.adec->sample_fmt, m.adec->sample_rate, 0, nullptr);
    if (ret < 0 || swr_init(m.swr) < 0) {
        std::fprintf(stderr, "[!] audio resampler init failed; audio disabled\n");
        close_audio(m);
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32LE, AUDIO_CH, AUDIO_RATE};
    m.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, nullptr, nullptr);
    if (!m.audio) std::fprintf(stderr, "[!] SDL audio open failed: %s\n", SDL_GetError());
    else SDL_ResumeAudioStreamDevice(m.audio);
    return true;
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

// Only decode what is shown; drop packets of the other sub-channels.
void apply_discard(Media& m) {
    for (unsigned i = 0; i < m.fmt->nb_streams; ++i)
        m.fmt->streams[i]->discard =
            ((int)i == m.vstream || (int)i == m.astream) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

// Decode program `prog_id` (-1 = the main service: highest-resolution video).
bool select_program(Media& m, SDL_Renderer* ren, int prog_id) {
    close_video(m);
    close_audio(m);
    const AVProgram* prog = prog_id >= 0 ? find_program(m.fmt, prog_id) : nullptr;
    int best = -1, best_area = 0;
    for (int i : streams_of_type(m.fmt, prog, AVMEDIA_TYPE_VIDEO)) {
        int area = m.fmt->streams[i]->codecpar->width * m.fmt->streams[i]->codecpar->height;
        if (best < 0 || area > best_area) { best_area = area; best = i; }
    }
    if (best < 0) { std::fprintf(stderr, "[-] no video stream found\n"); return false; }
    prog = av_find_program_from_stream(m.fmt, nullptr, best);
    m.program = prog ? prog->id : -1;
    if (!open_video(m, ren, best)) return false;
    auto aud = streams_of_type(m.fmt, prog, AVMEDIA_TYPE_AUDIO);
    if (aud.empty()) aud = streams_of_type(m.fmt, nullptr, AVMEDIA_TYPE_AUDIO);
    if (!aud.empty()) open_audio(m, aud[0]);
    apply_discard(m);
    std::printf("[+] media: program %d, %dx%d video%s\n", m.program, m.width, m.height,
                m.adec ? " + audio" : "");
    return true;
}

bool switch_audio(Media& m, int idx) {
    close_audio(m);
    bool ok = open_audio(m, idx);
    apply_discard(m);
    return ok;
}

// Probe the (already locked) channel and start decoding the main service.
bool open_media(TsSource& src, Media& m, SDL_Renderer* ren) {
    m.fmt = avformat_alloc_context();
    if (!m.fmt) return false;
    auto* buf = static_cast<uint8_t*>(av_malloc(65536));
    AVIOContext* avio = avio_alloc_context(buf, 65536, 0, &src, ts_read, nullptr, nullptr);
    if (!avio) { av_free(buf); return false; }
    m.fmt->pb = avio;
    m.fmt->probesize = 8 * 1024 * 1024;
    m.fmt->max_analyze_duration = 5 * AV_TIME_BASE;
    if (avformat_open_input(&m.fmt, nullptr, nullptr, nullptr) < 0) {
        // On failure avformat_open_input frees the context but not a custom pb.
        avio_context_free(&avio);
        std::fprintf(stderr, "[-] avformat_open_input failed\n");
        return false;
    }
    if (avformat_find_stream_info(m.fmt, nullptr) < 0) {
        std::fprintf(stderr, "[-] find_stream_info failed\n");
        return false;
    }
    return select_program(m, ren, -1);
}

void upload_video_frame(Media& m, AVFrame* f) {
    if (f->width != m.width || f->height != m.height) return;  // mid-stream resize
    if (f->format == AV_PIX_FMT_YUV420P) {
        SDL_UpdateYUVTexture(m.vtex, nullptr, f->data[0], f->linesize[0],
                             f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
        return;
    }
    // Convert unusual pixel formats to yuv420p for the SDL texture.
    if (!m.sws) {
        m.sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                               m.width, m.height, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        m.yuvframe = av_frame_alloc();
        m.yuvframe->format = AV_PIX_FMT_YUV420P;
        m.yuvframe->width = m.width;
        m.yuvframe->height = m.height;
        av_frame_get_buffer(m.yuvframe, 32);
    }
    if (m.sws && m.yuvframe) {
        sws_scale(m.sws, f->data, f->linesize, 0, f->height,
                  m.yuvframe->data, m.yuvframe->linesize);
        SDL_UpdateYUVTexture(m.vtex, nullptr,
                             m.yuvframe->data[0], m.yuvframe->linesize[0],
                             m.yuvframe->data[1], m.yuvframe->linesize[1],
                             m.yuvframe->data[2], m.yuvframe->linesize[2]);
    }
}

void push_audio_frame(Media& m, AVFrame* f) {
    if (!m.swr || !m.audio) return;
    int delay = (int)swr_get_delay(m.swr, f->sample_rate);
    int out_samples = f->nb_samples + delay;
    int need = out_samples * AUDIO_CH * (int)sizeof(float);
    if (need > m.audio_out_cap) {
        av_free(m.audio_out);
        m.audio_out_cap = need + 4096;
        m.audio_out = static_cast<uint8_t*>(av_malloc(m.audio_out_cap));
    }
    uint8_t* out[1] = {m.audio_out};
    int got = swr_convert(m.swr, out, out_samples, (const uint8_t**)f->data, f->nb_samples);
    if (got > 0) {
        // Bound latency: if SDL already holds > ~0.5 s, skip this chunk.
        int queued = SDL_GetAudioStreamQueued(m.audio);
        if (queued < AUDIO_RATE * AUDIO_CH * (int)sizeof(float) / 2)
            SDL_PutAudioStreamData(m.audio, m.audio_out, got * AUDIO_CH * (int)sizeof(float));
    }
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
    const char* base = SDL_GetBasePath();
    std::string b = base ? base : "";
    cands.push_back(b + "../Resources/fw/" + name);
    std::string up = b;
    for (int i = 0; i < 6; ++i) { cands.push_back(up + "fw/" + name); up += "../"; }
    cands.push_back(std::string("fw/") + name);
    if (const char* home = std::getenv("HOME")) {
        cands.push_back(std::string(home) + "/Library/Application Support/hibiki/fw/" + name);
        cands.push_back(std::string(home) + "/.config/hibiki/fw/" + name);
    }
    for (const auto& c : cands) if (file_exists(c)) return c;
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
    bool hud = false;          // persistent status HUD
    int scanlines = SCAN_MENU;
    bool clock24 = false;
    bool fav_only = false;     // channel surfing skips non-favourites
    int scan_max = hibiki::ATSC_CH_MAX_POST_REPACK;
    int last_channel = 0;
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
        else if (k == "scanlines") s.scanlines = std::clamp(iv, 0, SCAN_COUNT - 1);
        else if (k == "clock24") s.clock24 = iv != 0;
        else if (k == "fav_only") s.fav_only = iv != 0;
        else if (k == "scan_max")
            s.scan_max = iv >= hibiki::ATSC_CH_MAX ? hibiki::ATSC_CH_MAX
                                                   : hibiki::ATSC_CH_MAX_POST_REPACK;
        else if (k == "last_channel") s.last_channel = iv;
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
    std::fprintf(f, "volume=%.2f\nscale=%d\nhud=%d\nscanlines=%d\nclock24=%d\n"
                    "fav_only=%d\nscan_max=%d\nlast_channel=%d\nfavs=",
                 s.volume, s.scale, (int)s.hud, s.scanlines, (int)s.clock24,
                 (int)s.fav_only, s.scan_max, s.last_channel);
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
        "          [--menu] [--keys <Key,Key,...>] [--demo]\n"
        "  Enter menu; Esc back / quit; Left/Right or PgUp/PgDn channel;\n"
        "  Up/Down volume; 0-9 direct channel; C channel list; G guide;\n"
        "  A audio track; Z screen scale; I info; H status HUD; F fullscreen;\n"
        "  M mute; S scan. --demo runs without a tuner (colour bars) to preview\n"
        "  the OSD; --keys injects key presses at start-up (SDL key names).\n",
        prog);
}

// ---------------------------------------------------------------------------
struct Options {
    int channel = 11;
    bool channel_explicit = false;
    uint32_t freq_hz = 0;
    std::string fw_path, font_path, dump_path, screenshot_path, keys;
    bool fw_explicit = false, font_explicit = false;
    bool rescan = false, start_menu = false, trace = false, no_hud = false, demo = false;
    int change_after = 0;      // test hook: auto channel-change after N seconds
    int shot_after_ms = -1;
    int win_w = 1280, win_h = 720;
};

class App {
public:
    explicit App(const Options& o) : opt_(o) {}
    int run();

private:
    // -- lifecycle / IO --
    bool tune_to(int ch, uint32_t f);
    void change_channel(int dir);
    void do_scan();
    void poll_status();
    void handle_key(SDL_Keycode k);
    void commit_digits();
    void apply_gain() { if (m_.audio) SDL_SetAudioStreamGain(m_.audio, muted_ ? 0.0f : set_.volume); }
    void save() { if (!opt_.demo) save_settings(set_); }
    void toast(const std::string& s, int ms = 2000) { toast_ = s; toast_until_ = SDL_GetTicks() + ms; }
    void show_banner(int ms = 4000) { banner_until_ = SDL_GetTicks() + ms; }
    int scanned_index(int ch) const;
    std::string res_label() const;
    std::string subch_label() const;
    std::string audio_value() const;
    void cycle_audio();
    // -- PSIP / guide --
    int current_program() const { return media_ok_ ? m_.program : demo_program_; }
    const psip::VirtualChannel* current_vc() const { return psip_.by_program(current_program()); }
    std::vector<const psip::VirtualChannel*> guide_channels() const;
    std::string vc_label(const psip::VirtualChannel& vc) const;
    std::string clock_hm(int64_t t) const;
    std::string rf_names(int rf) const;
    void watch_program(int program_number);
    void psip_housekeeping(bool force = false);
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
    Media m_;
    bool media_ok_ = false;
    bool have_frame_ = false;
    osd::Renderer osd_;
    osd::Menu menu_;
    Settings set_;
    std::vector<ChannelEntry> scanned_;
    int channel_ = 0;
    uint32_t freq_ = 0;
    std::string fw_path_;
    hibiki::AtscStatus st_;
    bool st_ok_ = false;
    uint64_t st_ms_ = 0;
    bool muted_ = false, fullscreen_ = false;
    std::string tuning_;            // non-empty while a blocking tune is shown
    uint64_t banner_until_ = 0, vol_until_ = 0, toast_until_ = 0;
    std::string toast_;
    std::string digits_;
    uint64_t digits_ms_ = 0;
    SDL_Texture* bars_ = nullptr;
    uint64_t start_ms_ = 0, last_present_ = 0;
    bool shot_saved_ = false;
    uint64_t menu_refresh_ms_ = 0;
    // PSIP: decoded from the live TS (tapped in ts_read) plus the per-RF cache.
    psip::Decoder psip_;
    TsSource src_{&tuner_, &psip_};
    int demo_program_ = -1;
    uint64_t psip_saved_gen_ = 0, psip_saved_ms_ = 0;
    std::map<int, std::vector<psip::VirtualChannel>> rf_vct_;  // names per RF, from caches
};

int App::scanned_index(int ch) const {
    for (size_t i = 0; i < scanned_.size(); ++i) if (scanned_[i].ch == ch) return (int)i;
    return -1;
}

// ---- tuning ----------------------------------------------------------------
bool App::tune_to(int ch, uint32_t f) {
    psip_housekeeping(true);   // keep what we learned about the old channel
    close_media(m_);
    media_ok_ = false;
    have_frame_ = false;
    psip_.reset();
    demo_program_ = -1;
    if (!opt_.demo) load_psip_cache(ch, psip_, (int64_t)std::time(nullptr));
    psip_saved_gen_ = psip_.generation();
    channel_ = ch;
    freq_ = f;
    set_.last_channel = ch;
    save();
    show_banner();
    tuning_ = fmt("TUNING CH %d", ch);
    present();   // the tune below blocks for up to several seconds
    bool ok = false;
    if (opt_.demo) {
        SDL_Delay(250);
        ok = scanned_index(ch) >= 0;
        if (ok) {   // synthetic PSIP through the real decoder
            std::vector<uint8_t> ts = psip::build_demo_ts(ch, (int64_t)std::time(nullptr));
            psip_.feed(ts.data(), ts.size());
            if (!psip_.channels().empty()) demo_program_ = psip_.channels()[0].program_number;
        }
    } else {
        std::printf("[*] tuning channel %d (%.3f MHz)...\n", ch, f / 1e6);
        hibiki::AtscStatus st;
        if (tuner_.SetPara(f) == 0 && tuner_.WaitLock(6000, &st)) {
            std::printf("[+] locked (SNR %.1f dB)\n", st.snr_db_tenths / 10.0);
            tuning_ = fmt("CH %d  READING STREAM", ch);
            present();
            ok = open_media(src_, m_, ren_);
            if (!ok) close_media(m_);
        } else {
            std::fprintf(stderr, "[-] channel %d did not lock\n", ch);
        }
    }
    tuning_.clear();
    media_ok_ = ok && !opt_.demo;
    apply_gain();
    st_ms_ = 0;   // refresh signal status right away
    return ok;
}

void App::change_channel(int dir) {
    std::vector<ChannelEntry> list;
    for (const auto& e : scanned_)
        if (!set_.fav_only || set_.favs.empty() || set_.favs.count(e.ch)) list.push_back(e);
    if (list.empty()) {
        int nch = channel_ + dir;
        uint32_t nf = hibiki::atsc_channel_freq_hz(nch);
        if (nf) tune_to(nch, nf);
        return;
    }
    // Next channel in `list` after/before the current one (which may not be in it).
    int n = (int)list.size(), pick = dir > 0 ? 0 : n - 1;
    for (int i = 0; i < n; ++i) {
        int j = dir > 0 ? i : n - 1 - i;
        if (dir > 0 ? list[j].ch > channel_ : list[j].ch < channel_) { pick = j; break; }
    }
    tune_to(list[pick].ch, list[pick].freq);
}

void App::commit_digits() {
    if (digits_.empty()) return;
    int ch = std::atoi(digits_.c_str());
    digits_.clear();
    uint32_t f = hibiki::atsc_channel_freq_hz(ch);
    if (!f) { toast(fmt("CH %d NOT IN PLAN", ch)); return; }
    tune_to(ch, f);
}

void App::poll_status() {
    if (opt_.no_hud) return;
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
    close_media(m_);
    media_ok_ = false;
    have_frame_ = false;
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
        if (r == 2) found.push_back({ch, f, st.snr_db_tenths / 10.0f});
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
        if (probe(e.ch, e.freq, 5000, st) == 2)
            found.push_back({e.ch, e.freq, st.snr_db_tenths / 10.0f});
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
    if (!media_ok_) return "---";
    return fmt("%dX%d", m_.width, m_.height);
}

std::string App::subch_label() const {
    if (const psip::VirtualChannel* vc = current_vc()) return vc_label(*vc);
    if (!media_ok_) return "---";
    auto progs = video_programs(m_.fmt);
    for (size_t i = 0; i < progs.size(); ++i)
        if (progs[i]->id == m_.program) return fmt("%zu/%zu", i + 1, progs.size());
    return "1/1";
}

std::string App::audio_value() const {
    if (!media_ok_ || m_.astream < 0) return "NONE";
    auto aud = streams_of_type(m_.fmt, find_program(m_.fmt, m_.program), AVMEDIA_TYPE_AUDIO);
    size_t pos = std::find(aud.begin(), aud.end(), m_.astream) - aud.begin();
    return fmt("%zu ", pos + 1) + audio_label(m_.fmt->streams[m_.astream]);
}

void App::cycle_audio() {
    if (!media_ok_) return;
    auto aud = streams_of_type(m_.fmt, find_program(m_.fmt, m_.program), AVMEDIA_TYPE_AUDIO);
    if (aud.size() < 2) { toast("AUDIO: " + audio_value()); return; }
    size_t pos = std::find(aud.begin(), aud.end(), m_.astream) - aud.begin();
    switch_audio(m_, aud[(pos + 1) % aud.size()]);
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
    if (pn == current_program()) return;
    if (media_ok_) {
        if (!find_program(m_.fmt, pn) || !select_program(m_, ren_, pn)) {
            toast("SUB CHANNEL NOT IN STREAM");
            if (!m_.vdec) select_program(m_, ren_, -1);
        }
        have_frame_ = false;
        apply_gain();
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
    if (!opt_.demo) {
        int64_t now = psip_.now_utc();
        psip_.prune(now);
        save_psip_cache(channel_, psip_, now);
    }
    psip_saved_gen_ = psip_.generation();
    psip_saved_ms_ = ms;
}

// ---- input -----------------------------------------------------------------
void App::handle_key(SDL_Keycode k) {
    if (menu_.open() && menu_.key(k)) return;

    int digit = -1;
    if (k >= SDLK_0 && k <= SDLK_9) digit = (int)(k - SDLK_0);
    else if (k >= SDLK_KP_1 && k <= SDLK_KP_9) digit = (int)(k - SDLK_KP_1) + 1;
    else if (k == SDLK_KP_0) digit = 0;
    if (digit >= 0 && !menu_.open()) {
        if (digits_.empty() && digit == 0) return;
        digits_ += (char)('0' + digit);
        digits_ms_ = SDL_GetTicks();
        if (digits_.size() >= 2) commit_digits();
        return;
    }

    switch (k) {
    case SDLK_ESCAPE:
        if (!digits_.empty()) digits_.clear();
        else hibiki::g_running = false;
        break;
    case SDLK_Q: hibiki::g_running = false; break;
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
    case SDLK_I: show_banner(5000); break;
    case SDLK_H:
        set_.hud = !set_.hud;
        save();
        toast(set_.hud ? "STATUS HUD: ON" : "STATUS HUD: OFF");
        break;
    case SDLK_F:
        fullscreen_ = !fullscreen_;
        SDL_SetWindowFullscreen(win_, fullscreen_);
        break;
    case SDLK_M: muted_ = !muted_; apply_gain(); vol_until_ = SDL_GetTicks() + 2500; break;
    case SDLK_UP:
    case SDLK_DOWN:
        set_.volume = std::clamp(set_.volume + (k == SDLK_UP ? 0.1f : -0.1f), 0.0f, 1.5f);
        set_.volume = std::round(set_.volume * 10) / 10;
        muted_ = false;
        apply_gain();
        save();
        vol_until_ = SDL_GetTicks() + 2500;
        break;
    case SDLK_S: do_scan(); break;
    case SDLK_RIGHT: case SDLK_PAGEUP:   change_channel(+1); break;
    case SDLK_LEFT:  case SDLK_PAGEDOWN: change_channel(-1); break;
    default: break;
    }
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
        it.enabled = media_ok_ && m_.astream >= 0;
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
    p.sel = std::max(0, scanned_index(channel_));
    p.build = [this](osd::Page& pg) {
        pg.items.clear();
        pg.body.clear();
        if (scanned_.empty()) {
            pg.body = {"NO CHANNELS MEMORIZED"};
            osd::Item it;
            it.label = "AUTO CH MEMORY";
            it.activate = [this] { do_scan(); };
            pg.items.push_back(it);
            return;
        }
        for (const auto& e : scanned_) {
            osd::Item it;
            std::string names = rf_names(e.ch);
            it.label = names.empty()
                ? fmt("CH %2d  %7.2f MHZ  %4.1f DB", e.ch, e.freq / 1e6, e.snr)
                : fmt("CH %2d  %-19s %4.1fDB", e.ch, names.substr(0, 19).c_str(), e.snr);
            it.marked = e.ch == channel_;
            if (set_.favs.count(e.ch)) it.note = "FAV";
            int ch = e.ch; uint32_t f = e.freq;
            it.activate = [this, ch, f] { menu_.close(); tune_to(ch, f); };
            pg.items.push_back(it);
        }
    };
    p.on_key = [this](SDL_Keycode k) {
        osd::Page* pg = menu_.top();
        if (k != SDLK_SPACE || !pg || scanned_.empty()) return false;
        int ch = scanned_[(size_t)std::clamp(pg->sel, 0, (int)scanned_.size() - 1)].ch;
        if (!set_.favs.erase(ch)) set_.favs.insert(ch);
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
            for (const AVProgram* pr : video_programs(m_.fmt)) {
                auto v = streams_of_type(m_.fmt, pr, AVMEDIA_TYPE_VIDEO);
                auto a = streams_of_type(m_.fmt, pr, AVMEDIA_TYPE_AUDIO);
                const AVCodecParameters* cp = m_.fmt->streams[v[0]]->codecpar;
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
        auto aud = streams_of_type(m_.fmt, find_program(m_.fmt, m_.program), AVMEDIA_TYPE_AUDIO);
        for (size_t i = 0; i < aud.size(); ++i) {
            osd::Item it;
            it.label = fmt("%zu  ", i + 1) + audio_label(m_.fmt->streams[aud[i]]);
            it.marked = aud[i] == m_.astream;
            int idx = aud[i];
            it.activate = [this, idx] {
                if (idx == m_.astream) return;
                switch_audio(m_, idx);
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
        it = {}; it.label = "STATUS HUD";
        it.value = [this] { return std::string(set_.hud ? "ON" : "OFF"); };
        it.adjust = [this](int) { set_.hud = !set_.hud; save(); };
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
    osd::Page p;
    p.title = "ABOUT";
    p.hints = {"END   :ESC KEY"};
    p.build = [this](osd::Page& pg) {
        std::string fw = fw_path_.substr(fw_path_.find_last_of('/') + 1);
        pg.body = {fmt("HIBIKI TV  V%s", VERSION),
                   "MYGICA PT682C / EYETV STICK",
                   "MXL691 EAGLE ATSC DEMOD",
                   "",
                   opt_.demo ? "TUNER : DEMO MODE" : "TUNER : USB 1F4D:E691",
                   "FW    : " + upper(fw),
                   st_ok_ ? fmt("SIGNAL: %.1f DB %s", st_.snr_db_tenths / 10.0,
                                st_.locked() ? "LOCKED" : "NO LOCK")
                          : std::string("SIGNAL: ---"),
                   "VIDEO : " + res_label(),
                   fmt("PSIP  : %zu CH  %zu EVENTS", psip_.channels().size(), psip_.event_count())};
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
    if (media_ok_ && have_frame_ && m_.vtex) {
        float sar = m_.sar.num && m_.sar.den ? (float)av_q2d(m_.sar) : 1.0f;
        float aspect = m_.height ? m_.width * sar / m_.height : 0;
        dst = video_rect((float)m_.width, (float)m_.height, aspect, &src);
        SDL_RenderTexture(ren_, m_.vtex, &src, &dst);
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
        bool locked = media_ok_ || (opt_.demo && scanned_index(channel_) >= 0);
        std::string status = !tuning_.empty() ? "TUNE" : locked ? "PLAY \xE2\x96\xBA" : "NO SIGNAL";
        osd_.text(Font::Small, status, mx, my, osd::kWhite, Align::Left, Style::Vhs);
        float y = my + sh;
        float big = osd_.line_h(Font::Big);
        // With PSIP: virtual channel number + station name and what is on now.
        const psip::VirtualChannel* vc = current_vc();
        std::string num = vc ? fmt("CH %d-%d", vc->major, vc->minor) : fmt("CH %d", channel_);
        osd_.text(Font::Big, num, mx, y, osd::kWhite, Align::Left, Style::Vhs);
        y += big * 0.92f;
        std::string line = vc ? vcr_text(vc->name, 7) + fmt("  RF %d", channel_)
                              : fmt("%.2f MHZ", freq_ / 1e6) +
                                    (media_ok_ ? "  SUB " + subch_label() : std::string());
        if (set_.favs.count(channel_)) line += "  FAV";
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

    // Bottom-right: signal meter + lock flags (persistent status HUD).
    if (set_.hud || (banner && st_ok_)) {
        float y = H - my - sh * 2;
        if (st_ok_) {
            float snr = st_.snr_db_tenths / 10.0f;
            std::string s = fmt("SIG %4.1fDB %s", snr, st_.locked() ? "LOCK" : "----");
            osd_.text(Font::Small, s, W - mx, y, st_.locked() ? osd::kWhite : osd::kRed,
                      Align::Right, Style::Vhs);
            float mw = osd_.cell_w(Font::Small) * 12;
            osd_.meter(W - mx - mw, y + sh * 1.05f, mw, sh * 0.62f, 12, snr / 35.0f,
                       snr >= 15 ? osd::kGreen : osd::kRed, osd::kWhite);
            if (set_.hud)
                osd_.text(Font::Small, fmt("FRM:%d ATSC:%d FEC:%d", st_.frame_lock,
                                           st_.atsc_lock, st_.fec_lock),
                          W - mx, y - sh, osd::kWhite, Align::Right, Style::Vhs, 200);
        } else if (set_.hud) {
            osd_.text(Font::Small, "SIG ---", W - mx, y, osd::kWhite, Align::Right, Style::Vhs);
        }
    }

    // Direct channel entry ("CH 1-").
    if (!digits_.empty()) {
        std::string d = "CH " + digits_ + "-";
        osd_.text(Font::Big, d, W - mx, my + sh * 2.2f, osd::kVcrYellow, Align::Right, Style::Vhs);
    }
}

void App::maybe_screenshot() {
    uint64_t now = SDL_GetTicks();
    int shot_after = opt_.shot_after_ms >= 0 ? opt_.shot_after_ms : (opt_.demo ? 1200 : 6000);
    if (!opt_.screenshot_path.empty() && !shot_saved_ && now - start_ms_ > (uint64_t)shot_after &&
        (have_frame_ || opt_.demo || (!media_ok_ && tuning_.empty()))) {
        if (SDL_Surface* s = SDL_RenderReadPixels(ren_, nullptr)) {
            SDL_SaveBMP(s, opt_.screenshot_path.c_str());
            std::printf("[+] screenshot saved: %s\n", opt_.screenshot_path.c_str());
            SDL_DestroySurface(s);
        }
        shot_saved_ = true;
        if (opt_.demo) hibiki::g_running = false;
    }
}

void App::present() {
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) std::fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer("Hibiki", opt_.win_w, opt_.win_h, SDL_WINDOW_RESIZABLE,
                                     &win_, &ren_)) {
        std::fprintf(stderr, "create window/renderer: %s\n", SDL_GetError());
        return 1;
    }
    start_ms_ = SDL_GetTicks();

    std::string font_path = resolve_font(opt_.font_path, opt_.font_explicit);
    std::printf("[*] OSD font: %s\n", font_path.c_str());
    if (!osd_.init(ren_, font_path))
        std::fprintf(stderr, "[!] font load failed (%s); OSD text disabled\n", font_path.c_str());

    set_ = opt_.demo ? Settings{} : load_settings();
    fw_path_ = resolve_fw(opt_.fw_path, opt_.fw_explicit);
    if (opt_.demo) {
        bars_ = make_color_bars(ren_);
        for (int c : {7, 9, 11, 14, 20, 27, 33})
            scanned_.push_back({c, hibiki::atsc_channel_freq_hz(c), 18.0f + c * 0.3f});
        set_.favs = {11, 27};
    } else {
        std::printf("[*] firmware: %s\n", fw_path_.c_str());
        hibiki::TunerConfig cfg;
        cfg.fw_path = fw_path_;
        cfg.trace = opt_.trace;
        if (tuner_.Init(cfg) != 0) {
            std::fprintf(stderr, "tuner init failed (firmware '%s' not found? set --fw "
                                 "or $HIBIKI_FW)\n", cfg.fw_path.c_str());
            return 1;
        }
        scanned_ = load_channels();
        for (const auto& e : scanned_) {
            psip::Decoder tmp;
            if (load_psip_cache(e.ch, tmp, 0, true)) rf_vct_[e.ch] = tmp.channels();
        }
        if (opt_.rescan || scanned_.empty()) do_scan();
    }
    if (!hibiki::g_running) { if (!opt_.demo) tuner_.Close(); return 0; }

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
    if (!media_ok_ || channel_ != ch) tune_to(ch, f);

    if (opt_.start_menu) menu_.push(main_page());
    // Test hook: inject key presses (SDL key names, comma separated).
    if (!opt_.keys.empty()) {
        std::stringstream ss(opt_.keys);
        std::string name;
        while (std::getline(ss, name, ',')) {
            SDL_Keycode k = SDL_GetKeyFromName(name.c_str());
            if (k == SDLK_UNKNOWN) { std::fprintf(stderr, "[!] unknown key '%s'\n", name.c_str()); continue; }
            SDL_Event e{};
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = k;
            e.key.down = true;
            SDL_PushEvent(&e);
        }
    }

    AVPacket* pkt = av_packet_alloc();
    bool did_auto_change = false;
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
            if (ev.type == SDL_EVENT_QUIT) hibiki::g_running = false;
            else if (ev.type == SDL_EVENT_KEY_DOWN) handle_key(ev.key.key);
        }
        if (!hibiki::g_running) break;
        if (!digits_.empty() && SDL_GetTicks() - digits_ms_ > 2000) commit_digits();
        poll_status();
        psip_housekeeping();

        bool dirty = false;
        if (media_ok_) {
            int ret = av_read_frame(m_.fmt, pkt);
            if (ret == AVERROR_EOF) break;
            if (ret >= 0) {
                if (pkt->stream_index == m_.vstream && m_.vdec) {
                    avcodec_send_packet(m_.vdec, pkt);
                    while (avcodec_receive_frame(m_.vdec, m_.vframe) == 0) {
                        upload_video_frame(m_, m_.vframe);
                        have_frame_ = true;
                        dirty = true;
                        av_frame_unref(m_.vframe);
                    }
                } else if (pkt->stream_index == m_.astream && m_.adec) {
                    avcodec_send_packet(m_.adec, pkt);
                    while (avcodec_receive_frame(m_.adec, m_.aframe) == 0) {
                        push_audio_frame(m_, m_.aframe);
                        av_frame_unref(m_.aframe);
                    }
                }
                av_packet_unref(pkt);
            }
        }
        // With video, present per decoded frame; without, at ~30 Hz so the
        // OSD (clock, blinking, menus) stays live.
        if (dirty || SDL_GetTicks() - last_present_ >= 33) present();
        else if (!media_ok_) SDL_Delay(5);
    }

    av_packet_free(&pkt);
    psip_housekeeping(true);
    close_media(m_);
    if (bars_) SDL_DestroyTexture(bars_);
    if (!opt_.demo) tuner_.Close();
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
