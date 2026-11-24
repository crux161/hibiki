// hibiki SDL front-end - live ATSC TV with video + audio + HUD.
//
// Pipeline: hibiki::Tuner (async TS reader) -> custom FFmpeg AVIO -> libavformat
// demux -> libavcodec decode -> SDL3 YUV texture (video) / libswresample ->
// SDL3 audio stream (audio). A TTF HUD overlays channel, SNR and the three
// lock flags, refreshed from the demod via Tuner::ReadStatus.
//
// Controls: Esc / window-close quits; Left/Right (or PageUp/PageDown) changes
// channel; H toggles the HUD.

#include "hibiki/tuner.h"

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
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int AUDIO_RATE = 48000;
constexpr int AUDIO_CH = 2;
const char* DEFAULT_FONT = "/System/Library/Fonts/Supplemental/Arial.ttf";

// Optional raw-TS capture of exactly what the demuxer receives (diagnostics).
FILE* g_dump = nullptr;

// ---------------------------------------------------------------------------
// FFmpeg AVIO backed by the tuner's TS ring.
int ts_read(void* opaque, uint8_t* buf, int size) {
    auto* t = static_cast<hibiki::Tuner*>(opaque);
    while (hibiki::g_running) {
        int got = t->ReadTS(buf, (uint32_t)size);
        if (got > 0) {
            if (g_dump) std::fwrite(buf, 1, (size_t)got, g_dump);
            return got;
        }
    }
    return AVERROR_EOF;
}

// ---------------------------------------------------------------------------
// Media (demux + decode + render) state, rebuilt on every channel change.
struct Media {
    AVFormatContext* fmt = nullptr;
    AVIOContext* avio = nullptr;
    uint8_t* avio_buf = nullptr;
    int vstream = -1, astream = -1;
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
};

void close_media(Media& m, SDL_Renderer* ren) {
    if (m.vtex) { SDL_DestroyTexture(m.vtex); m.vtex = nullptr; }
    if (m.audio) { SDL_DestroyAudioStream(m.audio); m.audio = nullptr; }
    if (m.swr) swr_free(&m.swr);
    if (m.audio_out) { av_free(m.audio_out); m.audio_out = nullptr; }
    m.audio_out_cap = 0;  // else push_audio_frame reuses a freed buffer next channel
    if (m.yuvframe) av_frame_free(&m.yuvframe);
    if (m.sws) sws_freeContext(m.sws);
    m.sws = nullptr;
    if (m.vframe) av_frame_free(&m.vframe);
    if (m.aframe) av_frame_free(&m.aframe);
    if (m.vdec) avcodec_free_context(&m.vdec);
    if (m.adec) avcodec_free_context(&m.adec);
    if (m.fmt) {
        // avio buffer is owned by the AVIOContext; free via avio_context_free.
        if (m.fmt->pb) { avio_context_free(&m.fmt->pb); }
        avformat_close_input(&m.fmt);
    }
    m.avio_buf = nullptr;
    m.vstream = m.astream = -1;
    m.width = m.height = 0;
    (void)ren;
}

AVCodecContext* open_decoder(const AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

// Set up demux + decoders + SDL video texture + audio stream for the current
// (already locked) channel. Returns false on failure.
bool open_media(hibiki::Tuner& t, Media& m, SDL_Renderer* ren) {
    m.fmt = avformat_alloc_context();
    if (!m.fmt) return false;
    m.avio_buf = static_cast<uint8_t*>(av_malloc(65536));
    m.avio = avio_alloc_context(m.avio_buf, 65536, 0, &t, ts_read, nullptr, nullptr);
    if (!m.avio) return false;
    m.fmt->pb = m.avio;
    m.fmt->probesize = 8 * 1024 * 1024;
    m.fmt->max_analyze_duration = 5 * AV_TIME_BASE;

    if (avformat_open_input(&m.fmt, nullptr, nullptr, nullptr) < 0) {
        std::fprintf(stderr, "[-] avformat_open_input failed\n");
        return false;
    }
    if (avformat_find_stream_info(m.fmt, nullptr) < 0) {
        std::fprintf(stderr, "[-] find_stream_info failed\n");
        return false;
    }

    // A broadcast RF channel carries several sub-channels (programs). Default to
    // the main service = the highest-resolution video, and take the audio from
    // that same program.
    int best_area = 0;
    for (unsigned i = 0; i < m.fmt->nb_streams; ++i) {
        AVStream* st = m.fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            int area = st->codecpar->width * st->codecpar->height;
            if (area > best_area) { best_area = area; m.vstream = (int)i; }
        }
    }
    if (m.vstream < 0) {
        std::fprintf(stderr, "[-] no video stream found\n");
        return false;
    }
    if (const AVProgram* prog =
            av_find_program_from_stream(m.fmt, nullptr, m.vstream)) {
        for (unsigned j = 0; j < prog->nb_stream_indexes; ++j) {
            int idx = (int)prog->stream_index[j];
            if (m.fmt->streams[idx]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                m.astream = idx;
                break;
            }
        }
    }
    if (m.astream < 0) {
        for (unsigned i = 0; i < m.fmt->nb_streams; ++i) {
            if (m.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                m.astream = (int)i;
                break;
            }
        }
    }

    m.vdec = open_decoder(m.fmt->streams[m.vstream]);
    if (!m.vdec) { std::fprintf(stderr, "[-] video decoder open failed\n"); return false; }
    m.vframe = av_frame_alloc();
    m.width = m.vdec->width;
    m.height = m.vdec->height;

    m.vtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
                               SDL_TEXTUREACCESS_STREAMING, m.width, m.height);
    if (!m.vtex) { std::fprintf(stderr, "[-] SDL texture: %s\n", SDL_GetError()); return false; }

    if (m.astream >= 0) {
        m.adec = open_decoder(m.fmt->streams[m.astream]);
        if (m.adec) {
            m.aframe = av_frame_alloc();
            // Resample to interleaved float, stereo, 48 kHz for SDL.
            AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
            int ret = swr_alloc_set_opts2(&m.swr,
                &out_layout, AV_SAMPLE_FMT_FLT, AUDIO_RATE,
                &m.adec->ch_layout, m.adec->sample_fmt, m.adec->sample_rate,
                0, nullptr);
            if (ret < 0 || swr_init(m.swr) < 0) {
                std::fprintf(stderr, "[!] audio resampler init failed; audio disabled\n");
                swr_free(&m.swr);
                avcodec_free_context(&m.adec);
            } else {
                SDL_AudioSpec spec{SDL_AUDIO_F32LE, AUDIO_CH, AUDIO_RATE};
                m.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                    &spec, nullptr, nullptr);
                if (!m.audio) {
                    std::fprintf(stderr, "[!] SDL audio open failed: %s\n", SDL_GetError());
                } else {
                    SDL_ResumeAudioStreamDevice(m.audio);
                }
            }
        }
    }
    std::printf("[+] media: %dx%d video%s\n", m.width, m.height,
                m.adec ? " + audio" : "");
    return true;
}

// ---------------------------------------------------------------------------
void render_video_frame(Media& m, SDL_Renderer* ren, AVFrame* f) {
    if (f->format == AV_PIX_FMT_YUV420P) {
        SDL_UpdateYUVTexture(m.vtex, nullptr,
                             f->data[0], f->linesize[0],
                             f->data[1], f->linesize[1],
                             f->data[2], f->linesize[2]);
    } else {
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
    SDL_RenderTexture(ren, m.vtex, nullptr, nullptr);
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
    int got = swr_convert(m.swr, out, out_samples,
                          (const uint8_t**)f->data, f->nb_samples);
    if (got > 0) {
        // Bound latency: if SDL already holds > ~0.5 s, skip this chunk.
        int queued = SDL_GetAudioStreamQueued(m.audio);
        if (queued < AUDIO_RATE * AUDIO_CH * (int)sizeof(float) / 2)
            SDL_PutAudioStreamData(m.audio, m.audio_out,
                                   got * AUDIO_CH * (int)sizeof(float));
    }
}

// ---------------------------------------------------------------------------
struct Hud {
    TTF_Font* font = nullptr;
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool visible = true;
    bool poll = true;      // false => skip the demod I2C read (diagnostics)
    uint64_t last_ms = 0;

    void update(SDL_Renderer* ren, hibiki::Tuner& t, int channel, uint32_t freq) {
        if (!poll) return;
        uint64_t now = SDL_GetTicks();
        if (now - last_ms < 500 && tex) return;
        last_ms = now;
        hibiki::AtscStatus st;
        bool ok = t.ReadStatus(st);
        char line[256];
        if (ok) {
            std::snprintf(line, sizeof(line),
                "CH %d  %.3f MHz   SNR %.1f dB   FRAME:%d ATSC:%d FEC:%d  %s",
                channel, freq / 1e6, st.snr_db_tenths / 10.0,
                st.frame_lock, st.atsc_lock, st.fec_lock,
                st.locked() ? "LOCKED" : "no lock");
        } else {
            std::snprintf(line, sizeof(line), "CH %d  %.3f MHz   (status read failed)",
                          channel, freq / 1e6);
        }
        if (!font) return;
        SDL_Color fg{255, 255, 255, 255};
        SDL_Surface* surf = TTF_RenderText_Blended(font, line, 0, fg);
        if (!surf) return;
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTextureFromSurface(ren, surf);
        w = surf->w; h = surf->h;
        SDL_DestroySurface(surf);
    }

    void draw(SDL_Renderer* ren) {
        if (!visible || !tex) return;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 140);
        SDL_FRect bg{8.0f, 8.0f, (float)w + 16.0f, (float)h + 12.0f};
        SDL_RenderFillRect(ren, &bg);
        SDL_FRect dst{16.0f, 14.0f, (float)w, (float)h};
        SDL_RenderTexture(ren, tex, nullptr, &dst);
    }
};

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

// ---------------------------------------------------------------------------
// Scanned-channel memory. The app scans the (US) channel plan once, remembers
// the channels that lock, and thereafter tunes / navigates only those.
struct ChannelEntry { int ch; uint32_t freq; float snr; };

std::string channels_path() {
    char* p = SDL_GetPrefPath("hibiki", "Hibiki");
    std::string dir = p ? p : "";
    if (p) SDL_free(p);
    if (dir.empty())
        dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
              "/.config/hibiki/";
    return dir + "channels.txt";
}

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

void draw_text(SDL_Renderer* ren, TTF_Font* font, const std::string& s,
               float x, float y) {
    if (!font) return;
    SDL_Color fg{255, 255, 255, 255};
    SDL_Surface* surf = TTF_RenderText_Blended(font, s.c_str(), 0, fg);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FRect dst{x, y, (float)surf->w, (float)surf->h};
    SDL_RenderTexture(ren, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_DestroySurface(surf);
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [--channel <n>] [--freq <hz>] [--fw <path>] [--font <ttf>]\n"
        "          [--width <px>] [--height <px>] [--trace] [--no-hud]\n"
        "          [--dump-ts <file>] [--screenshot <bmp>] [--rescan]\n"
        "  Esc quits; Left/Right or PageUp/PageDown changes channel; H toggles HUD;\n"
        "  F fullscreen; M mute; Up/Down volume; S rescans channels.\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { hibiki::g_running = false; });
    std::signal(SIGTERM, [](int) { hibiki::g_running = false; });

    int channel = 11;
    bool channel_explicit = false;
    uint32_t freq_hz = 0;
    std::string fw_path;
    bool fw_explicit = false;
    bool rescan = false;
    int change_after = 0;   // test hook: auto channel-change after N seconds
    bool start_menu = false; // test hook: open the channel menu at startup
    std::string font_path;
    bool font_explicit = false;
    std::string dump_path;
    std::string screenshot_path;
    int win_w = 1280, win_h = 720;
    bool trace = false;
    bool no_hud = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--channel" && i + 1 < argc) { channel = std::atoi(argv[++i]); channel_explicit = true; }
        else if (a == "--rescan") rescan = true;
        else if (a == "--change-after" && i + 1 < argc) change_after = std::atoi(argv[++i]);
        else if (a == "--menu") start_menu = true;
        else if (a == "--country" && i + 1 < argc) {
            std::string c = argv[++i];
            if (c != "us" && c != "US") {
                std::fprintf(stderr, "only country 'us' is supported for now\n");
                return 2;
            }
        }
        else if (a == "--freq" && i + 1 < argc)
            freq_hz = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (a == "--fw" && i + 1 < argc) { fw_path = argv[++i]; fw_explicit = true; }
        else if (a == "--font" && i + 1 < argc) { font_path = argv[++i]; font_explicit = true; }
        else if (a == "--dump-ts" && i + 1 < argc) dump_path = argv[++i];
        else if (a == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
        else if (a == "--no-hud") no_hud = true;
        else if (a == "--width" && i + 1 < argc) win_w = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) win_h = std::atoi(argv[++i]);
        else if (a == "--trace") trace = true;
        else { usage(argv[0]); return 2; }
    }
    if (freq_hz == 0) freq_hz = hibiki::atsc_channel_freq_hz(channel);
    if (freq_hz == 0) { std::fprintf(stderr, "invalid channel\n"); return 2; }
    if (!dump_path.empty()) g_dump = std::fopen(dump_path.c_str(), "wb");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) {
        std::fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        // continue without HUD text
    }

    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("hibiki TV", win_w, win_h,
                                     SDL_WINDOW_RESIZABLE, &win, &ren)) {
        std::fprintf(stderr, "create window/renderer: %s\n", SDL_GetError());
        return 1;
    }

    hibiki::TunerConfig cfg;
    cfg.fw_path = resolve_fw(fw_path, fw_explicit);
    std::printf("[*] firmware: %s\n", cfg.fw_path.c_str());
    cfg.trace = trace;
    hibiki::Tuner tuner;
    if (tuner.Init(cfg) != 0) {
        std::fprintf(stderr, "tuner init failed (firmware '%s' not found? set --fw "
                             "or $HIBIKI_FW)\n", cfg.fw_path.c_str());
        return 1;
    }

    font_path = resolve_font(font_path, font_explicit);
    std::printf("[*] OSD font: %s\n", font_path.c_str());
    TTF_Font* font = TTF_OpenFont(font_path.c_str(), 20.0f);
    if (!font)
        std::fprintf(stderr, "[!] font load failed (%s); HUD text disabled\n",
                     font_path.c_str());

    auto tune = [&](int ch, uint32_t f) -> bool {
        std::printf("[*] tuning channel %d (%.3f MHz)...\n", ch, f / 1e6);
        if (tuner.SetPara(f) != 0) return false;
        hibiki::AtscStatus st;
        if (!tuner.WaitLock(6000, &st)) {
            std::fprintf(stderr, "[-] channel %d did not lock\n", ch);
            return false;
        }
        std::printf("[+] locked (SNR %.1f dB)\n", st.snr_db_tenths / 10.0);
        return true;
    };

    // Remembered channels: load, or scan the US plan on first run / --rescan.
    std::vector<ChannelEntry> scanned = load_channels();
    auto do_scan = [&](int lo, int hi) {
        scanned.clear();
        std::vector<ChannelEntry> maybe;  // partial-lock candidates for pass 2
        auto overlay = [&](const std::string& s) {
            SDL_RenderClear(ren);
            draw_text(ren, font, s, 40, 40);
            draw_text(ren, font, "Esc to cancel", 40, 80);
            SDL_RenderPresent(ren);
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_EVENT_QUIT) hibiki::g_running = false;
        };
        // 2 = full lock, 1 = partial (carrier/frame but not FEC), 0 = nothing.
        auto probe = [&](int ch, uint32_t f, int dwell, hibiki::AtscStatus& st) -> int {
            if (tuner.SetPara(f) != 0) return 0;
            if (tuner.WaitLock(dwell, &st)) return 2;
            return (st.atsc_lock || st.frame_lock) ? 1 : 0;
        };
        for (int ch = lo; ch <= hi && hibiki::g_running; ++ch) {
            uint32_t f = hibiki::atsc_channel_freq_hz(ch);
            if (!f) continue;
            char line[128];
            std::snprintf(line, sizeof(line),
                          "Scanning channel %d (%.3f MHz)  -  found %zu",
                          ch, f / 1e6, scanned.size());
            overlay(line);
            hibiki::AtscStatus st;
            int r = probe(ch, f, 2000, st);
            std::fprintf(stderr, "ch %2d %7.3f MHz: snr %5.1f dB carrier %+7d Hz "
                         "flags f%d a%d e%d -> %s\n", ch, f / 1e6,
                         st.snr_db_tenths / 10.0, st.carrier_offset_hz,
                         st.frame_lock, st.atsc_lock, st.fec_lock,
                         r == 2 ? "LOCK" : r == 1 ? "partial" : "none");
            if (r == 2) {
                scanned.push_back({ch, f, st.snr_db_tenths / 10.0f});
                std::printf("[+] scan: ch %d locked (%.1f dB)\n",
                            ch, st.snr_db_tenths / 10.0);
            } else if (r == 1) {
                maybe.push_back({ch, f, st.snr_db_tenths / 10.0f});
            }
        }
        // Weak stations can take several seconds to reach FEC lock; re-probe the
        // partial-lock candidates with a longer dwell before giving up.
        for (const auto& e : maybe) {
            if (!hibiki::g_running) break;
            char line[128];
            std::snprintf(line, sizeof(line), "Rechecking channel %d ...", e.ch);
            overlay(line);
            hibiki::AtscStatus st;
            if (probe(e.ch, e.freq, 5000, st) == 2) {
                scanned.push_back({e.ch, e.freq, st.snr_db_tenths / 10.0f});
                std::printf("[+] scan: ch %d locked on retry (%.1f dB)\n",
                            e.ch, st.snr_db_tenths / 10.0);
            }
        }
        std::sort(scanned.begin(), scanned.end(),
                  [](const ChannelEntry& a, const ChannelEntry& b) { return a.ch < b.ch; });
        save_channels(scanned);
        std::printf("[+] scan complete: %zu channel(s) saved to %s\n",
                    scanned.size(), channels_path().c_str());
    };

    if (rescan || scanned.empty())
        do_scan(hibiki::ATSC_CH_MIN, hibiki::ATSC_CH_MAX_POST_REPACK);
    if (!hibiki::g_running) { tuner.Close(); return 0; }
    if (!scanned.empty() && !channel_explicit) {
        channel = scanned[0].ch;
        freq_hz = scanned[0].freq;
    }
    if (freq_hz == 0) freq_hz = hibiki::atsc_channel_freq_hz(channel);

    if (!tune(channel, freq_hz)) { tuner.Close(); return 1; }

    Media m;
    if (!open_media(tuner, m, ren)) {
        close_media(m, ren);
        tuner.Close();
        return 1;
    }

    Hud hud;
    hud.poll = !no_hud;
    hud.font = font;

    AVPacket* pkt = av_packet_alloc();
    bool have_video_frame = false;
    uint64_t start_ms = SDL_GetTicks();
    bool shot_saved = false;

    float volume = 1.0f;
    bool muted = false;
    bool fullscreen = false;
    auto apply_gain = [&]() {
        if (m.audio) SDL_SetAudioStreamGain(m.audio, muted ? 0.0f : volume);
    };
    apply_gain();

    // VCR-OSD on-screen display: big channel banner + a channel-list menu.
    TTF_Font* font_big = TTF_OpenFont(font_path.c_str(), 48.0f);
    bool menu_open = start_menu;
    int menu_sel = 0;
    uint64_t banner_until = SDL_GetTicks() + 3000;

    auto save_screenshot = [&]() {
        if (screenshot_path.empty() || shot_saved) return;
        SDL_Surface* surf = SDL_RenderReadPixels(ren, nullptr);
        if (surf) {
            SDL_SaveBMP(surf, screenshot_path.c_str());
            std::printf("[+] screenshot saved: %s\n", screenshot_path.c_str());
            SDL_DestroySurface(surf);
        }
        shot_saved = true;
    };

    auto change_channel = [&](int dir) {
        int nch; uint32_t nf;
        if (!scanned.empty()) {
            int idx = 0;
            for (size_t i = 0; i < scanned.size(); ++i)
                if (scanned[i].ch == channel) { idx = (int)i; break; }
            int n = (int)scanned.size();
            int nidx = ((idx + dir) % n + n) % n;
            nch = scanned[nidx].ch; nf = scanned[nidx].freq;
        } else {
            nch = channel + dir;
            nf = hibiki::atsc_channel_freq_hz(nch);
        }
        if (nf == 0) return;
        close_media(m, ren);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        if (tune(nch, nf) && open_media(tuner, m, ren)) {
            channel = nch; freq_hz = nf;
            have_video_frame = false;
            apply_gain();
            banner_until = SDL_GetTicks() + 3000;
        }
    };

    auto draw_banner = [&]() {
        if (!font_big || SDL_GetTicks() > banner_until) return;
        char num[16];
        std::snprintf(num, sizeof(num), "%d", channel);
        SDL_Color fg{255, 255, 255, 255};
        SDL_Surface* s = TTF_RenderText_Blended(font_big, num, 0, fg);
        if (!s) return;
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
        SDL_FRect d{(float)(ow - s->w - 24), 24, (float)s->w, (float)s->h};
        SDL_RenderTexture(ren, t, nullptr, &d);
        SDL_DestroyTexture(t);
        SDL_DestroySurface(s);
    };

    auto draw_menu = [&]() {
        if (!menu_open || !font) return;
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(ren, &ow, &oh);
        int n = (int)scanned.size();
        const float rowh = 30.0f, w = 460.0f;
        float h = 60.0f + rowh * std::max(1, n);
        float x = (ow - w) / 2, y = (oh - h) / 2;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 190);
        SDL_FRect bg{x, y, w, h};
        SDL_RenderFillRect(ren, &bg);
        draw_text(ren, font, "CHANNELS", x + 16, y + 12);
        if (n == 0) draw_text(ren, font, "(none - press S to scan)", x + 16, y + 50);
        for (int i = 0; i < n; ++i) {
            if (i == menu_sel) {
                SDL_SetRenderDrawColor(ren, 0, 110, 0, 170);
                SDL_FRect hr{x + 8, y + 44 + i * rowh, w - 16, rowh};
                SDL_RenderFillRect(ren, &hr);
            }
            char line[128];
            std::snprintf(line, sizeof(line), "%s CH %2d  %7.3f MHz  %4.1f dB",
                          i == menu_sel ? ">" : " ", scanned[i].ch,
                          scanned[i].freq / 1e6, scanned[i].snr);
            draw_text(ren, font, line, x + 16, y + 48 + i * rowh);
        }
    };
    bool did_auto_change = false;

    while (hibiki::g_running) {
        if (change_after > 0 && !did_auto_change &&
            SDL_GetTicks() - start_ms > (uint64_t)change_after * 1000) {
            did_auto_change = true;
            std::printf("[*] test: auto channel change\n");
            change_channel(1);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) hibiki::g_running = false;
            else if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                if (k == SDLK_ESCAPE) {
                    if (menu_open) menu_open = false;
                    else hibiki::g_running = false;
                }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (menu_open) {
                        menu_open = false;
                        if (!scanned.empty()) {
                            int idx = std::clamp(menu_sel, 0, (int)scanned.size() - 1);
                            close_media(m, ren);
                            if (tune(scanned[idx].ch, scanned[idx].freq) &&
                                open_media(tuner, m, ren)) {
                                channel = scanned[idx].ch; freq_hz = scanned[idx].freq;
                                have_video_frame = false; apply_gain();
                                banner_until = SDL_GetTicks() + 3000;
                            }
                        }
                    } else {
                        menu_open = true;
                        menu_sel = 0;
                        for (size_t i = 0; i < scanned.size(); ++i)
                            if (scanned[i].ch == channel) { menu_sel = (int)i; break; }
                    }
                }
                else if (k == SDLK_H) hud.visible = !hud.visible;
                else if (k == SDLK_F) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen);
                }
                else if (k == SDLK_M) { muted = !muted; apply_gain(); }
                else if (k == SDLK_UP) {
                    if (menu_open) {
                        int n = (int)scanned.size();
                        if (n) menu_sel = ((menu_sel - 1) % n + n) % n;
                    } else { volume = std::min(1.5f, volume + 0.1f); muted = false; apply_gain(); }
                }
                else if (k == SDLK_DOWN) {
                    if (menu_open) {
                        int n = (int)scanned.size();
                        if (n) menu_sel = (menu_sel + 1) % n;
                    } else { volume = std::max(0.0f, volume - 0.1f); apply_gain(); }
                }
                else if (k == SDLK_S) {
                    close_media(m, ren);
                    do_scan(hibiki::ATSC_CH_MIN, hibiki::ATSC_CH_MAX_POST_REPACK);
                    if (!scanned.empty() &&
                        tune(scanned[0].ch, scanned[0].freq) &&
                        open_media(tuner, m, ren)) {
                        channel = scanned[0].ch; freq_hz = scanned[0].freq;
                        have_video_frame = false;
                        apply_gain();
                    }
                }
                else if (k == SDLK_RIGHT || k == SDLK_PAGEDOWN ||
                         k == SDLK_LEFT || k == SDLK_PAGEUP) {
                    change_channel((k == SDLK_RIGHT || k == SDLK_PAGEDOWN) ? 1 : -1);
                }
            }
        }
        if (!hibiki::g_running) break;

        int ret = av_read_frame(m.fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            continue;
        }
        if (pkt->stream_index == m.vstream && m.vdec) {
            avcodec_send_packet(m.vdec, pkt);
            while (avcodec_receive_frame(m.vdec, m.vframe) == 0) {
                SDL_RenderClear(ren);
                render_video_frame(m, ren, m.vframe);
                hud.update(ren, tuner, channel, freq_hz);
                hud.draw(ren);
                draw_banner();
                draw_menu();
                SDL_RenderPresent(ren);
                have_video_frame = true;
                if (!shot_saved && SDL_GetTicks() - start_ms > 6000)
                    save_screenshot();
                av_frame_unref(m.vframe);
            }
        } else if (pkt->stream_index == m.astream && m.adec) {
            avcodec_send_packet(m.adec, pkt);
            while (avcodec_receive_frame(m.adec, m.aframe) == 0) {
                push_audio_frame(m, m.aframe);
                av_frame_unref(m.aframe);
            }
        }
        av_packet_unref(pkt);

        // Keep the HUD fresh even when video is momentarily stalled.
        if (!have_video_frame) {
            hud.update(ren, tuner, channel, freq_hz);
        }
    }

    av_packet_free(&pkt);
    if (hud.tex) SDL_DestroyTexture(hud.tex);
    if (font_big) TTF_CloseFont(font_big);
    if (hud.font) TTF_CloseFont(hud.font);
    close_media(m, ren);
    tuner.Close();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
