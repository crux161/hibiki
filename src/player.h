// hibiki media player: MPEG-TS in, synchronised picture + sound out.
//
//   TS source --(demux thread)--> libavformat --> video / audio packet queues
//                                     |
//                                     +--> raw TS copy for the PSIP decoder
//
//   main thread: service() decodes audio into SDL (keeping ~0.25 s queued),
//   decodes video into a small frame queue, and uploads each frame to the
//   texture when its PTS comes due on the master clock.
//
// The master clock is the audio clock (PTS at the end of the audio handed to
// SDL minus what SDL still holds), so the picture follows the sound the
// listener actually hears. Without audio it is a wall clock anchored on the
// first video frame. A timestamp jump of more than a few seconds (splice,
// wrap, replay loop) is treated as a discontinuity and resynchronises.
//
// The demux thread never runs on the UI thread, so a stalled signal cannot
// freeze the menus; it applies back-pressure through the queue limits, which
// also paces file playback to real time.

#pragma once

#include <SDL3/SDL.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace player {

// Reads up to `size` bytes of TS. Return >0 bytes, 0 if no data yet (it will
// be called again), or <0 at end of stream. Called on the demux thread.
using ReadFn = std::function<int(uint8_t* buf, int size)>;
using PumpFn = std::function<bool()>;  // main-thread UI pump; false cancels probing

struct Stats {
    double av_offset = 0;        // last (video pts - clock) at display, seconds
    double audio_latency = 0;    // seconds of audio queued in SDL
    uint64_t frames_shown = 0, frames_dropped = 0, resyncs = 0;
};

class Player {
public:
    Player() = default;
    ~Player() { close(); }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Probe the stream (blocking, on the caller's thread) and start decoding
    // `program_id` (-1 = pick the main service). Returns false on failure.
    bool open(ReadFn read, SDL_Renderer* ren, int program_id = -1, PumpFn pump = {});
    void close();
    bool is_open() const { return fmt_ != nullptr; }

    // Switch sub-channel (MPEG program) / audio track without re-probing.
    bool select_program(int program_id);
    bool select_audio(int stream_index);

    // Call every main-loop iteration. Returns true when a new frame was put
    // into texture() (i.e. the screen should be redrawn).
    bool service();
    // Milliseconds the caller may sleep before service() has work again.
    int idle_ms() const;

    void set_gain(float g);

    // ---- state for the UI ----
    SDL_Texture* texture() const { return have_frame_ ? vtex_ : nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    double display_aspect() const;
    const AVFormatContext* format() const { return fmt_; }
    int program() const { return program_; }
    int video_stream() const { return vstream_; }
    int audio_stream() const { return astream_; }
    bool has_frame() const { return have_frame_; }
    bool ended() const { return eof_ && vq_.empty() && aq_.empty() && vfq_.empty(); }
    // ms since the source last delivered bytes (signal stall detection).
    uint64_t ms_since_data() const;
    const Stats& stats() const { return stats_; }

    // Raw TS seen by the demuxer since the last call (for PSIP decoding).
    void take_ts(std::vector<uint8_t>& out);

private:
    struct PacketQueue {
        std::deque<AVPacket*> q;
        size_t bytes = 0;
        bool empty() const { return q.empty(); }
    };
    void demux_loop();
    static int avio_read(void* opaque, uint8_t* buf, int size);
    bool open_video(int idx);
    bool open_audio(int idx);
    void close_video();
    void close_audio();
    void flush_queues();
    void reset_clock();
    double master_clock() const;
    double audio_queued_s() const;
    void decode_audio_packet(AVPacket* pkt);
    void upload(AVFrame* f);
    double pq_duration(const PacketQueue& q) const;

    // source / demux
    ReadFn read_;
    AVFormatContext* fmt_ = nullptr;
    AVIOContext* avio_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    std::thread demux_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> eof_{false};
    std::atomic<uint64_t> last_data_ms_{0};
    std::atomic<int> sel_v_{-1}, sel_a_{-1};   // streams the demux thread keeps
    std::atomic<bool> paused_{false};          // demux holds off while streams switch
    mutable std::mutex mu_;              // guards vq_, aq_, tap_
    std::condition_variable cv_;         // demux waits for room
    PacketQueue vq_, aq_;
    std::vector<uint8_t> tap_;

    // decode (main thread only)
    int program_ = -1, vstream_ = -1, astream_ = -1;
    AVCodecContext* vdec_ = nullptr;
    AVCodecContext* adec_ = nullptr;
    AVFrame* tmp_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* yuv_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::vector<uint8_t> audio_out_;
    SDL_AudioStream* audio_ = nullptr;
    float gain_ = 1.0f;
    std::deque<AVFrame*> vfq_;           // decoded, waiting for their PTS
    SDL_Texture* vtex_ = nullptr;
    int width_ = 0, height_ = 0;
    AVRational sar_{0, 1};
    bool have_frame_ = false;

    // clock
    double audio_end_pts_ = -1;          // PTS (s) at the end of audio given to SDL
    double wall_anchor_pts_ = -1;        // no-audio clock: pts at anchor time
    uint64_t wall_anchor_ns_ = 0;
    // audio ran dry: continue on the wall clock from where the sound stopped
    mutable bool starve_on_ = false;
    mutable double starve_pts_ = 0;
    mutable uint64_t starve_ns_ = 0;
    Stats stats_;
};

} // namespace player
