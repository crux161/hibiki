// hibiki media player (see player.h).

#include "player.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>

namespace player {

namespace {
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kBytesPerSec = kRate * kChannels * sizeof(float);
constexpr double kAudioTarget = 0.25;      // seconds of audio kept queued in SDL
constexpr double kOutputLatency = 0.015;   // rough device latency beyond SDL's queue
constexpr double kMaxAudioAhead = 1.0;     // demux pauses when aq holds this much
constexpr size_t kMaxVideoBytes = 24u << 20;
constexpr size_t kMaxVideoPackets = 600;
constexpr double kMaxVideoAhead = 2.0;     // media seconds of video packets queued
constexpr size_t kMaxProcessedFrames = 8;  // decoded/filtered pictures awaiting display
constexpr size_t kMaxProcessedBytes = 32u << 20;
constexpr double kMaxProcessedAhead = 0.6; // media seconds of processed output queued
constexpr size_t kMaxTapBytes = 4u << 20;
constexpr double kDiscontinuity = 5.0;     // seconds

uint64_t now_ns() { return SDL_GetTicksNS(); }

std::string ratio_str(AVRational r) {
    if (!r.num || !r.den) return "0:1";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d:%d", r.num, r.den);
    return buf;
}

std::string rate_str(AVRational r) {
    if (!r.num || !r.den) return "0/1";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d/%d", r.num, r.den);
    return buf;
}

const char* field_order_name(AVFieldOrder order) {
    switch (order) {
        case AV_FIELD_PROGRESSIVE: return "progressive";
        case AV_FIELD_TT: return "tt";
        case AV_FIELD_BB: return "bb";
        case AV_FIELD_TB: return "tb";
        case AV_FIELD_BT: return "bt";
        default: return "unknown";
    }
}

const char* color_range_name(AVColorRange range) {
    const char* name = av_color_range_name(range);
    return name ? name : "unknown";
}

const char* color_space_name(AVColorSpace space) {
    const char* name = av_color_space_name(space);
    return name ? name : "unknown";
}

size_t frame_storage_bytes(const AVFrame* frame) {
    int64_t size = av_image_get_buffer_size((AVPixelFormat)frame->format,
                                            frame->width, frame->height, 1);
    if (size > 0) return (size_t)size;
    return (size_t)frame->width * (size_t)frame->height * 3 / 2;
}
} // namespace

int Player::avio_read(void* opaque, uint8_t* buf, int size) {
    auto* self = static_cast<Player*>(opaque);
    while (!self->stop_) {
        int n = self->read_(buf, size);
        if (n > 0) {
            self->last_data_ms_ = SDL_GetTicks();
            std::lock_guard<std::mutex> lk(self->mu_);
            if (self->tap_.size() < kMaxTapBytes) self->tap_.insert(self->tap_.end(), buf, buf + n);
            return n;
        }
        if (n < 0) return AVERROR_EOF;
    }
    return AVERROR_EOF;
}

bool Player::open(ReadFn read, SDL_Renderer* ren, int program_id, PumpFn pump,
                  DecoderPreference decoder) {
    close();
    read_ = std::move(read);
    ren_ = ren;
    decoder_pref_ = decoder;
    stop_ = false;
    eof_ = false;
    last_data_ms_ = SDL_GetTicks();
    fmt_ = avformat_alloc_context();
    if (!fmt_) return false;
    auto* buf = static_cast<uint8_t*>(av_malloc(65536));
    if (!buf) { close(); return false; }
    avio_ = avio_alloc_context(buf, 65536, 0, this, avio_read, nullptr, nullptr);
    if (!avio_) { av_free(buf); close(); return false; }
    fmt_->pb = avio_;
    fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt_->interrupt_callback = {[](void* self) -> int {
        return static_cast<Player*>(self)->stop_;
    }, this};
    fmt_->probesize = 8 * 1024 * 1024;
    fmt_->max_analyze_duration = 5 * AV_TIME_BASE;
    // FFmpeg probing may wait indefinitely for TS bytes. Keep renderer and
    // event handling on the caller thread while only probing runs off-thread.
    auto probing = std::async(std::launch::async, [this] {
        return avformat_open_input(&fmt_, nullptr, nullptr, nullptr) >= 0 &&
               avformat_find_stream_info(fmt_, nullptr) >= 0;
    });
    while (probing.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
        if (pump && !pump()) stop_ = true;
    if (!probing.get() || stop_) { close(); return false; }
    if (!select_program(program_id)) { close(); return false; }
    demux_ = std::thread([this] { demux_loop(); });
    return true;
}

void Player::close() {
    stop_ = true;
    worker_stop_ = true;
    cv_.notify_all();
    out_cv_.notify_all();
    if (demux_.joinable()) demux_.join();
    stop_worker();
    flush_queues();
    close_video();
    close_audio();
    if (vtex_) { SDL_DestroyTexture(vtex_); vtex_ = nullptr; }
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (yuv_) av_frame_free(&yuv_);
    if (tmp_) av_frame_free(&tmp_);
    if (fmt_) avformat_close_input(&fmt_);
    if (avio_) {
        av_freep(&avio_->buffer);
        avio_context_free(&avio_);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        tap_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        outq_.clear();
        outq_bytes_ = 0;
        generation_ = 0;
        reset_requested_ = false;
        worker_drained_ = false;
        processing_graph_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(info_mu_);
        info_ = VideoInfo{};
    }
    tff_frames_ = bff_frames_ = progressive_frames_ = 0;
    observed_logged_.clear();
    rate_start_ns_ = 0;
    rate_decoded_last_ = 0;
    rate_output_ = 0;
    decoded_total_ = 0;
    processed_total_ = 0;
    decode_ms_ = 0;
    process_ms_ = 0;
    program_ = vstream_ = astream_ = -1;
    sel_v_ = sel_a_ = -1;
    width_ = height_ = 0;
    have_frame_ = false;
    eof_ = false;
    stats_ = Stats{};
    reset_clock();
}

// ---------------------------------------------------------------------------
void Player::demux_loop() {
    AVPacket* pkt = av_packet_alloc();
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            auto room = [&] {
                return !paused_ && pq_duration(aq_) < kMaxAudioAhead
                    && vq_.bytes < kMaxVideoBytes
                    && vq_.q.size() < kMaxVideoPackets
                    && pq_duration(vq_) < kMaxVideoAhead;
            };
            cv_.wait_for(lk, std::chrono::milliseconds(20), [&] { return stop_ || room(); });
            if (stop_) break;
            if (!room()) continue;
        }
        int r = av_read_frame(fmt_, pkt);
        if (r < 0) {
            if (r == AVERROR_EOF || (fmt_->pb && fmt_->pb->eof_reached)) { eof_ = true; break; }
            continue;   // corrupt data: keep reading
        }
        int si = pkt->stream_index;
        PacketQueue* q = si == sel_v_ ? &vq_ : si == sel_a_ ? &aq_ : nullptr;
        if (!q) { av_packet_unref(pkt); continue; }
        AVPacket* p = av_packet_alloc();
        av_packet_move_ref(p, pkt);
        {
            std::lock_guard<std::mutex> lk(mu_);
            q->bytes += (size_t)p->size;
            q->q.push_back(p);
            cv_.notify_all();
        }
    }
    av_packet_free(&pkt);
}

double Player::pq_duration(const PacketQueue& q) const {
    if (q.q.size() < 2 || !fmt_) return 0;
    const AVPacket* a = q.q.front();
    const AVPacket* b = q.q.back();
    int64_t ta = a->dts != AV_NOPTS_VALUE ? a->dts : a->pts;
    int64_t tb = b->dts != AV_NOPTS_VALUE ? b->dts : b->pts;
    if (ta == AV_NOPTS_VALUE || tb == AV_NOPTS_VALUE) return 0;
    double d = (tb - ta) * av_q2d(fmt_->streams[a->stream_index]->time_base);
    return d < 0 || d > 60 ? 0 : d;   // ignore wraps / discontinuities
}

void Player::flush_queues() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto* q : {&vq_, &aq_}) {
            for (AVPacket* p : q->q) av_packet_free(&p);
            q->q.clear();
            q->bytes = 0;
        }
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        outq_.clear();
        outq_bytes_ = 0;
        out_cv_.notify_all();
    }
}

AVPacket* Player::pop_packet(PacketQueue& q) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q.q.empty()) return nullptr;
    AVPacket* p = q.q.front();
    q.q.pop_front();
    q.bytes -= (size_t)p->size;
    cv_.notify_all();
    return p;
}

// ---------------------------------------------------------------------------
bool Player::open_video(int idx) {
    const AVStream* st = fmt_->streams[idx];
    const AVCodecParameters* cp = st->codecpar;
    const AVCodec* codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) return false;
    vdec_ = avcodec_alloc_context3(codec);
    if (!vdec_ || avcodec_parameters_to_context(vdec_, cp) < 0 ||
        avcodec_open2(vdec_, codec, nullptr) < 0) {
        avcodec_free_context(&vdec_);
        std::fprintf(stderr, "[-] video decoder open failed\n");
        return false;
    }
    vstream_ = idx;
    sar_ = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : cp->sample_aspect_ratio;
    if (!width_) { width_ = cp->width; height_ = cp->height; }

    // Static stream properties for the HUD and --stats.
    VideoInfo info;
    info.codec = avcodec_get_name(cp->codec_id);
    const char* profile = avcodec_profile_name(cp->codec_id, cp->profile);
    info.profile = profile ? profile : "unknown";
    const char* pix_fmt = av_get_pix_fmt_name((AVPixelFormat)cp->format);
    info.pixel_format = pix_fmt ? pix_fmt : "unknown";
    info.width = cp->width;
    info.height = cp->height;
    info.active_width = cp->width;
    info.active_height = cp->height;
    info.sar = sar_.num && sar_.den ? ratio_str(sar_) : "1:1";
    AVRational fps = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    info.frame_rate = rate_str(fps);
    info.field_order = field_order_name(cp->field_order);
    info.color_range = color_range_name(cp->color_range);
    info.color_space = color_space_name(cp->color_space);
    info.decoder_requested = decoder_pref_ == DecoderPreference::Cpu ? "cpu" : "auto";
    info.decoder_active = "ffmpeg-cpu";
    info.processing = "passthrough";
    {
        std::lock_guard<std::mutex> lk(info_mu_);
        info_ = info;
    }
    std::printf("[+] video: codec=%s profile=%s size=%dx%d pix_fmt=%s field=%s "
                "range=%s space=%s sar=%s fps=%s decoder=%s>%s processing=%s\n",
                info.codec.c_str(), info.profile.c_str(), info.width, info.height,
                info.pixel_format.c_str(), info.field_order.c_str(),
                info.color_range.c_str(), info.color_space.c_str(), info.sar.c_str(),
                info.frame_rate.c_str(), info.decoder_requested.c_str(),
                info.decoder_active.c_str(), info.processing.c_str());
    return true;
}

bool Player::open_audio(int idx) {
    const AVStream* st = fmt_->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;
    adec_ = avcodec_alloc_context3(codec);
    if (!adec_ || avcodec_parameters_to_context(adec_, st->codecpar) < 0 ||
        avcodec_open2(adec_, codec, nullptr) < 0) {
        avcodec_free_context(&adec_);
        return false;
    }
    astream_ = idx;
    AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_FLT, kRate, &adec_->ch_layout,
                            adec_->sample_fmt, adec_->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
        std::fprintf(stderr, "[!] audio resampler init failed; audio disabled\n");
        close_audio();
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32LE, kChannels, kRate};
    audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!audio_) {
        std::fprintf(stderr, "[!] SDL audio open failed: %s\n", SDL_GetError());
    } else {
        SDL_SetAudioStreamGain(audio_, gain_);
        SDL_ResumeAudioStreamDevice(audio_);
    }
    return true;
}

void Player::close_video() {
    if (vdec_) avcodec_free_context(&vdec_);
    vstream_ = -1;
}

void Player::close_audio() {
    if (audio_) { SDL_DestroyAudioStream(audio_); audio_ = nullptr; }
    if (swr_) swr_free(&swr_);
    if (adec_) avcodec_free_context(&adec_);
    astream_ = -1;
}

static std::vector<int> streams_in(const AVFormatContext* fmt, const AVProgram* prog, AVMediaType t) {
    std::vector<int> v;
    if (prog) {
        for (unsigned j = 0; j < prog->nb_stream_indexes; ++j) {
            int i = (int)prog->stream_index[j];
            if (fmt->streams[i]->codecpar->codec_type == t) v.push_back(i);
        }
    } else {
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->codecpar->codec_type == t) v.push_back((int)i);
    }
    return v;
}

namespace {
// Holds the demux thread off while the selected streams change, so it does
// not race through the source discarding everything in the gap.
struct Pause {
    std::atomic<bool>& f;
    std::condition_variable& cv;
    Pause(std::atomic<bool>& flag, std::condition_variable& c) : f(flag), cv(c) { f = true; }
    ~Pause() { f = false; cv.notify_all(); }
};
} // namespace

void Player::start_worker() {
    if (worker_.joinable()) return;
    worker_stop_ = false;
    worker_ = std::thread([this] { video_worker(); });
}

void Player::stop_worker() {
    if (!worker_.joinable()) return;
    worker_stop_ = true;
    cv_.notify_all();
    out_cv_.notify_all();
    worker_.join();
    worker_stop_ = false;
}

void Player::bump_generation_locked() {
    ++generation_;
    outq_.clear();
    outq_bytes_ = 0;
    worker_drained_ = false;
}

void Player::request_video_reset(const char* reason) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (AVPacket* p : vq_.q) av_packet_free(&p);
        vq_.q.clear();
        vq_.bytes = 0;
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        reset_requested_ = true;
        bump_generation_locked();
    }
    out_cv_.notify_all();
    if (reason) std::printf("[*] video reset: %s\n", reason);
}

void Player::set_processing_graph(std::string graph) {
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        processing_graph_ = std::move(graph);
        reset_requested_ = true;
        bump_generation_locked();
    }
    cv_.notify_all();
    out_cv_.notify_all();
}

bool Player::select_program(int program_id) {
    if (!fmt_) return false;
    Pause hold(paused_, cv_);
    stop_worker();
    const AVProgram* prog = nullptr;
    if (program_id >= 0)
        for (unsigned i = 0; i < fmt_->nb_programs; ++i)
            if (fmt_->programs[i]->id == program_id) prog = fmt_->programs[i];
    // Main service when unspecified: largest picture, ties to the lowest
    // program number, so the choice is the same every time.
    int best = -1, best_area = -1, best_prog = 1 << 30;
    for (int i : streams_in(fmt_, prog, AVMEDIA_TYPE_VIDEO)) {
        const AVCodecParameters* cp = fmt_->streams[i]->codecpar;
        const AVProgram* p = av_find_program_from_stream(fmt_, nullptr, i);
        int pid = p ? p->id : 0, area = cp->width * cp->height;
        if (area > best_area || (area == best_area && pid < best_prog)) {
            best = i; best_area = area; best_prog = pid;
        }
    }
    if (best < 0) { std::fprintf(stderr, "[-] no video stream found\n"); return false; }

    sel_v_ = sel_a_ = -1;
    flush_queues();
    close_video();
    close_audio();
    prog = av_find_program_from_stream(fmt_, nullptr, best);
    program_ = prog ? prog->id : -1;
    if (!open_video(best)) return false;
    auto aud = streams_in(fmt_, prog, AVMEDIA_TYPE_AUDIO);
    if (aud.empty()) aud = streams_in(fmt_, nullptr, AVMEDIA_TYPE_AUDIO);
    if (!aud.empty()) open_audio(aud[0]);
    reset_clock();
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        ++generation_;
        outq_.clear();
        outq_bytes_ = 0;
        reset_requested_ = false;
        worker_drained_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(info_mu_);
        tff_frames_ = bff_frames_ = progressive_frames_ = 0;
        observed_logged_.clear();
        info_.field_order_observed = "unknown";
    }
    sel_v_ = vstream_;
    sel_a_ = astream_;
    std::printf("[+] media: program %d, %dx%d video%s\n", program_,
                fmt_->streams[best]->codecpar->width, fmt_->streams[best]->codecpar->height,
                adec_ ? " + audio" : "");
    start_worker();
    return true;
}

bool Player::select_audio(int idx) {
    if (!fmt_ || idx < 0 || idx >= (int)fmt_->nb_streams) return false;
    Pause hold(paused_, cv_);
    sel_a_ = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (AVPacket* p : aq_.q) av_packet_free(&p);
        aq_.q.clear();
        aq_.bytes = 0;
    }
    close_audio();
    bool ok = open_audio(idx);
    reset_clock();
    sel_a_ = astream_;
    return ok;
}

void Player::set_gain(float g) {
    gain_ = g;
    if (audio_) SDL_SetAudioStreamGain(audio_, g);
}

// ---------------------------------------------------------------------------
// Video worker: decode, run the processing boundary, queue bounded output.

void Player::video_worker() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    AVFrame* decoded = av_frame_alloc();
    if (!decoded) return;
    AVPacket* pkt = nullptr;
    bool configured = false;
    bool configure_failed = false;   // retry only after a reset
    int configured_format = -1, configured_width = 0, configured_height = 0;
    bool decoder_eof = false, flushed = false;
    uint64_t gen = 0;

    auto output_room = [&](const video::OutputFrame& out) {
        if (outq_.size() >= kMaxProcessedFrames) return false;
        size_t bytes = frame_storage_bytes(out.frame);
        if (outq_bytes_ + bytes > kMaxProcessedBytes) return false;
        if (!outq_.empty()) {
            double first = outq_.front().pts_seconds();
            double last = out.pts_seconds();
            if (first >= 0 && last >= 0 && last - first > kMaxProcessedAhead)
                return false;
        }
        return true;
    };
    // Move every ready output into the bounded display queue, waiting for the
    // main thread when full. Returns false if a reset/stop interrupted it.
    auto move_outputs = [&]() -> bool {
        video::OutputFrame out;
        while (processor.take_output(out)) {
            std::unique_lock<std::mutex> lk(out_mu_);
            out_cv_.wait(lk, [&] {
                return stop_ || worker_stop_ || reset_requested_ || generation_ != gen ||
                       output_room(out);
            });
            if (stop_ || worker_stop_) return false;
            if (reset_requested_ || generation_ != gen) return false;
            outq_bytes_ += frame_storage_bytes(out.frame);
            outq_.push_back(std::move(out));
            ++processed_total_;
            out_cv_.notify_all();
        }
        return true;
    };
    // Decode one frame into the processor. Returns false when a reset/stop
    // interrupted output queueing; the main loop handles it next iteration.
    auto handle_decoded = [&]() -> bool {
        ++decoded_total_;
        decoded->pts = decoded->best_effort_timestamp;   // may be AV_NOPTS_VALUE
        note_frame_info(decoded);
        if (configured && (decoded->width != configured_width ||
                           decoded->height != configured_height ||
                           decoded->format != configured_format)) {
            {
                std::lock_guard<std::mutex> lk(out_mu_);
                bump_generation_locked();
                gen = generation_;
            }
            std::printf("[*] video format changed: %dx%d fmt %d -> %dx%d fmt %d; "
                        "resetting processing\n",
                        configured_width, configured_height, configured_format,
                        decoded->width, decoded->height, decoded->format);
            processor.reset();
            configured = false;
            configure_failed = false;
            flushed = false;
        }
        if (!configured && !configure_failed) {
            AVRational vtb = fmt_->streams[vstream_]->time_base;
            AVRational fps = fmt_->streams[vstream_]->avg_frame_rate;
            if (processor.configure(decoded, vtb, fps, config)) {
                configured = true;
                configured_width = processor.info().width;
                configured_height = processor.info().height;
                configured_format = processor.info().pixel_format;
                {
                    std::lock_guard<std::mutex> lk(info_mu_);
                    info_.processing = processor.info().graph;
                }
                std::printf("[*] processing: %s, sink time base %d/%d\n",
                            processor.info().graph.c_str(),
                            processor.info().output_time_base.num,
                            processor.info().output_time_base.den);
            } else {
                configure_failed = true;
                std::fprintf(stderr, "[-] processor configure failed: %s\n",
                             processor.error().c_str());
            }
        }
        if (configured) {
            uint64_t start = SDL_GetTicksNS();
            if (processor.submit(decoded, gen)) {
                double ms = (SDL_GetTicksNS() - start) / 1e6;
                double previous = process_ms_.load();
                process_ms_.store(previous > 0 ? previous * 0.8 + ms * 0.2 : ms);
            } else {
                std::fprintf(stderr, "[-] processor submit failed: %s\n",
                             processor.error().c_str());
            }
            bool moved = move_outputs();
            av_frame_unref(decoded);
            return moved;
        }
        av_frame_unref(decoded);
        return true;
    };

    while (!stop_ && !worker_stop_) {
        bool do_reset = false;
        {
            std::lock_guard<std::mutex> lk(out_mu_);
            gen = generation_;
            config.graph = processing_graph_;
            if (reset_requested_) {
                reset_requested_ = false;
                do_reset = true;
            }
        }
        if (do_reset) {
            processor.reset();
            avcodec_flush_buffers(vdec_);
            configured = false;
            configure_failed = false;
            decoder_eof = flushed = false;
            if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
            continue;
        }

        if (!pkt) pkt = pop_packet(vq_);
        if (pkt) {
            if (pkt->stream_index != vstream_) {
                av_packet_free(&pkt);
                pkt = nullptr;
                continue;
            }
            uint64_t decode_ns = 0;
            int decoded_now = 0;
            uint64_t send_start = SDL_GetTicksNS();
            int sent = avcodec_send_packet(vdec_, pkt);
            decode_ns += SDL_GetTicksNS() - send_start;
            av_packet_free(&pkt);
            pkt = nullptr;
            bool keep_going = true;
            if (sent >= 0) {
                for (;;) {
                    uint64_t receive_start = SDL_GetTicksNS();
                    int ret = avcodec_receive_frame(vdec_, decoded);
                    decode_ns += SDL_GetTicksNS() - receive_start;
                    if (ret < 0) break;
                    ++decoded_now;
                    if (!handle_decoded()) {
                        keep_going = false;
                        break;
                    }
                }
            }
            if (decoded_now > 0) {
                double ms = decode_ns / 1e6 / decoded_now;
                double previous = decode_ms_.load();
                decode_ms_.store(previous > 0 ? previous * 0.8 + ms * 0.2 : ms);
            }
            if (!keep_going) continue;
            continue;
        }

        if (eof_ && !decoder_eof) {
            decoder_eof = true;
            avcodec_send_packet(vdec_, nullptr);
            uint64_t decode_ns = 0;
            int decoded_now = 0;
            for (;;) {
                uint64_t receive_start = SDL_GetTicksNS();
                int ret = avcodec_receive_frame(vdec_, decoded);
                decode_ns += SDL_GetTicksNS() - receive_start;
                if (ret < 0) break;
                ++decoded_now;
                if (!handle_decoded()) break;
            }
            if (decoded_now > 0) {
                double ms = decode_ns / 1e6 / decoded_now;
                double previous = decode_ms_.load();
                decode_ms_.store(previous > 0 ? previous * 0.8 + ms * 0.2 : ms);
            }
            continue;
        }

        if (decoder_eof && !flushed) {
            bool ok = true;
            if (configured) {
                if (!processor.flush(gen))
                    std::fprintf(stderr, "[-] processor flush failed: %s\n",
                                 processor.error().c_str());
                ok = move_outputs();
            }
            if (!ok) continue;   // reset/stop; handled at the top of the loop
            flushed = true;
            {
                std::lock_guard<std::mutex> lk(out_mu_);
                if (generation_ == gen) worker_drained_ = true;
            }
            out_cv_.notify_all();
            continue;
        }

        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(10), [&] {
            return stop_ || worker_stop_ || eof_ || !vq_.q.empty();
        });
    }
    processor.close();
    av_frame_free(&decoded);
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        worker_drained_ = false;
    }
}

// ---------------------------------------------------------------------------
// Clock. Members touched here are main-thread only.

void Player::reset_clock() {
    audio_end_pts_ = -1;
    wall_anchor_pts_ = -1;
    wall_anchor_ns_ = 0;
    starve_on_ = false;
    if (audio_) SDL_ClearAudioStream(audio_);
}

double Player::audio_queued_s() const {
    return audio_ ? SDL_GetAudioStreamQueued(audio_) / kBytesPerSec : 0.0;
}

double Player::master_clock() const {
    if (audio_ && audio_end_pts_ >= 0) {
        double q = audio_queued_s();
        if (q > 0.0) {
            starve_on_ = false;
            return audio_end_pts_ - q - kOutputLatency;
        }
        // Audio ran dry (gap in the broadcast audio): keep time on the wall
        // clock from where the sound stopped, so the picture does not freeze.
        if (!starve_on_) { starve_on_ = true; starve_pts_ = audio_end_pts_ - kOutputLatency; starve_ns_ = now_ns(); }
        return starve_pts_ + (now_ns() - starve_ns_) / 1e9;
    }
    if (wall_anchor_pts_ >= 0) return wall_anchor_pts_ + (now_ns() - wall_anchor_ns_) / 1e9;
    return -1;
}

void Player::decode_audio_packet(AVPacket* pkt) {
    if (avcodec_send_packet(adec_, pkt) < 0) return;
    const AVRational tb = fmt_->streams[astream_]->time_base;
    while (avcodec_receive_frame(adec_, tmp_) == 0) {
        int64_t ts = tmp_->best_effort_timestamp;
        double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(tb) : audio_end_pts_;
        double dur = tmp_->sample_rate ? (double)tmp_->nb_samples / tmp_->sample_rate : 0;
        // A jump in audio time (splice / loop) restarts the queue cleanly.
        if (audio_end_pts_ >= 0 && pts >= 0 && std::fabs(pts - audio_end_pts_) > kDiscontinuity) {
            SDL_ClearAudioStream(audio_);
            ++stats_.resyncs;
        }
        int out_samples = swr_get_out_samples(swr_, tmp_->nb_samples);
        size_t need = (size_t)std::max(0, out_samples) * kChannels * sizeof(float);
        if (audio_out_.size() < need) audio_out_.resize(need + 4096);
        uint8_t* out[1] = {audio_out_.data()};
        int got = swr_convert(swr_, out, out_samples, (const uint8_t**)tmp_->data, tmp_->nb_samples);
        if (got > 0) SDL_PutAudioStreamData(audio_, audio_out_.data(), got * kChannels * (int)sizeof(float));
        if (pts >= 0) audio_end_pts_ = pts + dur;
        av_frame_unref(tmp_);
    }
}

void Player::upload(AVFrame* f) {
    uint64_t start_ns = SDL_GetTicksNS();
    if (!vtex_ || f->width != width_ || f->height != height_) {
        // First frame, or the broadcaster changed resolution.
        if (vtex_) SDL_DestroyTexture(vtex_);
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        if (yuv_) av_frame_free(&yuv_);
        width_ = f->width;
        height_ = f->height;
        vtex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                  width_, height_);
        if (!vtex_) { std::fprintf(stderr, "[-] SDL texture: %s\n", SDL_GetError()); return; }
    }
    if (f->sample_aspect_ratio.num) sar_ = f->sample_aspect_ratio;
    if (f->format == AV_PIX_FMT_YUV420P) {
        SDL_UpdateYUVTexture(vtex_, nullptr, f->data[0], f->linesize[0],
                             f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
    } else {
        if (!sws_) {
            sws_ = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                                  width_, height_, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
            yuv_ = av_frame_alloc();
            yuv_->format = AV_PIX_FMT_YUV420P;
            yuv_->width = width_;
            yuv_->height = height_;
            av_frame_get_buffer(yuv_, 32);
        }
        if (!sws_ || !yuv_) return;
        sws_scale(sws_, f->data, f->linesize, 0, f->height, yuv_->data, yuv_->linesize);
        SDL_UpdateYUVTexture(vtex_, nullptr, yuv_->data[0], yuv_->linesize[0],
                             yuv_->data[1], yuv_->linesize[1], yuv_->data[2], yuv_->linesize[2]);
    }
    have_frame_ = true;
    ++stats_.frames_shown;
    ++rate_output_;
    double upload_ms = (SDL_GetTicksNS() - start_ns) / 1e6;
    stats_.upload_ms = stats_.upload_ms > 0
                           ? stats_.upload_ms * 0.8 + upload_ms * 0.2
                           : upload_ms;
}

void Player::note_frame_info(const AVFrame* f) {
    std::lock_guard<std::mutex> lk(info_mu_);
    if (f->width && f->height &&
        (f->width != info_.active_width || f->height != info_.active_height)) {
        std::printf("[*] video size changed: %dx%d -> %dx%d\n",
                    info_.active_width, info_.active_height, f->width, f->height);
        info_.active_width = f->width;
        info_.active_height = f->height;
        ++info_.size_changes;
    }
    if (f->flags & AV_FRAME_FLAG_INTERLACED) {
        if (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ++tff_frames_;
        else ++bff_frames_;
    } else {
        ++progressive_frames_;
    }
    if (tff_frames_ && bff_frames_) info_.field_order_observed = "mixed";
    else if (tff_frames_) info_.field_order_observed = "tt";
    else if (bff_frames_) info_.field_order_observed = "bb";
    else if (progressive_frames_) info_.field_order_observed = "progressive";
    else info_.field_order_observed = "unknown";
    if (info_.field_order_observed != observed_logged_) {
        observed_logged_ = info_.field_order_observed;
        std::printf("[*] field order observed: %s\n", observed_logged_.c_str());
    }
}

void Player::update_rates() {
    uint64_t now = SDL_GetTicksNS();
    if (!rate_start_ns_) {
        rate_start_ns_ = now;
        rate_decoded_last_ = decoded_total_.load();
        return;
    }
    double elapsed = (now - rate_start_ns_) / 1e9;
    if (elapsed < 0.5) return;
    uint64_t decoded_now = decoded_total_.load();
    stats_.decoded_fps = (decoded_now - rate_decoded_last_) / elapsed;
    stats_.output_fps = rate_output_ / elapsed;
    rate_decoded_last_ = decoded_now;
    rate_output_ = 0;
    rate_start_ns_ = now;
}

double Player::output_span_locked() const {
    if (outq_.size() < 2) return 0;
    double first = outq_.front().pts_seconds();
    double last = outq_.back().pts_seconds();
    if (first < 0 || last <= first) return 0;
    return last - first;
}

bool Player::service() {
    if (!fmt_) return false;
    if (!tmp_) tmp_ = av_frame_alloc();

    // 1. Audio: keep ~kAudioTarget seconds queued in SDL (main thread).
    if (adec_ && audio_) {
        while (audio_queued_s() < kAudioTarget) {
            AVPacket* p = pop_packet(aq_);
            if (!p) break;
            if (p->stream_index == astream_) decode_audio_packet(p);
            av_packet_free(&p);
        }
    } else {
        while (AVPacket* p = pop_packet(aq_)) av_packet_free(&p);
    }
    stats_.audio_latency = audio_queued_s();

    // 2. Show the processed frame whose time has come, dropping late output
    //    that the next picture already superseded.
    double clock = master_clock();
    bool shown = false;
    bool discontinuity = false;
    video::OutputFrame item;
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        while (!outq_.empty()) {
            video::OutputFrame& front = outq_.front();
            double pts = front.pts_seconds();
            bool show = false;
            if (pts < 0) {
                show = true;                                   // unknown: show now
            } else if (clock < 0) {
                if (!audio_) {                                 // no audio: start the wall clock here
                    wall_anchor_pts_ = pts;
                    wall_anchor_ns_ = now_ns();
                    show = true;
                } else if (!have_frame_) {
                    show = true;                               // first picture while audio starts
                } else {
                    break;
                }
            } else if (std::fabs(pts - clock) > kDiscontinuity) {
                discontinuity = true;
                break;
            } else if (pts - clock > 0.003) {
                break;                                         // not due yet
            } else {
                bool superseded = false;
                if (outq_.size() > 1) {
                    double next = outq_[1].pts_seconds();
                    superseded = next >= 0 && next <= clock;
                }
                if (superseded) {
                    outq_bytes_ -= frame_storage_bytes(front.frame);
                    outq_.pop_front();
                    ++stats_.frames_dropped;
                    out_cv_.notify_all();
                    continue;
                }
                stats_.av_offset = pts - clock;
                show = true;
            }
            if (!show) break;
            outq_bytes_ -= frame_storage_bytes(front.frame);
            item = std::move(front);
            outq_.pop_front();
            out_cv_.notify_all();
            shown = true;
            break;
        }
        stats_.queue_video_s = output_span_locked();
    }
    if (item.valid()) {
        upload(item.frame);
        item.reset();
        shown = true;
    }
    if (discontinuity) {
        ++stats_.resyncs;
        request_video_reset("timestamp discontinuity");
        if (!audio_) { wall_anchor_pts_ = -1; wall_anchor_ns_ = 0; }
        shown = false;
    }

    // 3. Safety valve: video far ahead with no audio to move the clock.
    bool stuck;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stuck = vq_.bytes >= kMaxVideoBytes && aq_.q.empty() && audio_queued_s() == 0;
    }
    if (stuck) {
        ++stats_.resyncs;
        request_video_reset("video stalled");
        reset_clock();
    }

    // Diagnostics snapshot (worker counters are atomics; queues are locked).
    stats_.frames_decoded = decoded_total_.load();
    stats_.frames_processed = processed_total_.load();
    stats_.decode_ms = decode_ms_.load();
    stats_.process_ms = process_ms_.load();
    {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.queue_packets_s = std::max(pq_duration(vq_), pq_duration(aq_));
    }
    update_rates();
    return shown;
}

bool Player::ended() const {
    if (!eof_) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!vq_.empty() || !aq_.empty()) return false;
    }
    std::lock_guard<std::mutex> lk(out_mu_);
    return outq_.empty() && worker_drained_;
}

int Player::idle_ms() const {
    double wait = 0;
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        if (outq_.empty()) return 1;
        const video::OutputFrame& front = outq_.front();
        double pts = front.pts_seconds();
        if (pts < 0) return 0;
        double clock = master_clock();
        if (clock < 0) return 1;
        wait = pts - clock;
    }
    return (int)std::clamp(wait * 1000.0 - 1.0, 0.0, 5.0);
}

double Player::display_aspect() const {
    if (!height_) return 0;
    double sar = sar_.num && sar_.den ? av_q2d(sar_) : 1.0;
    return width_ * sar / height_;
}

uint64_t Player::ms_since_data() const { return SDL_GetTicks() - last_data_ms_; }

VideoInfo Player::info() const {
    std::lock_guard<std::mutex> lk(info_mu_);
    return info_;
}

void Player::take_ts(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    out.swap(tap_);
    tap_.clear();
}

} // namespace player
