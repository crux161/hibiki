// Synthetic player-level tests for the M1 worker path: in-memory MPEG-TS
// playback through the real Player (demux thread, decode/filter worker, main
// thread scheduling), EOF drain, a multi-output processing graph, and prompt
// cancellation while the worker is blocked on a full output queue.
//
// Needs an SDL renderer for texture upload, so build the test with the SDL
// dummy video driver (CTest sets SDL_VIDEODRIVER=dummy).

#include "player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct MemorySink {
    std::vector<uint8_t> data;
};

int write_packet(void* opaque, const uint8_t* buffer, int size) {
    auto* sink = static_cast<MemorySink*>(opaque);
    sink->data.insert(sink->data.end(), buffer, buffer + size);
    return size;
}

// Encode a short MPEG-2 TS into memory (no audio, so the player uses its wall
// clock). 320x240 at 30 fps, no B frames.
std::vector<uint8_t> build_ts(int frames, int gap_at = -1, int gap_seconds = 0) {
    AVFormatContext* format = nullptr;
    require(avformat_alloc_output_context2(&format, nullptr, "mpegts", nullptr) >= 0 &&
                format != nullptr, "avformat_alloc_output_context2");
    AVStream* stream = avformat_new_stream(format, nullptr);
    require(stream != nullptr, "avformat_new_stream");
    stream->time_base = {1, 90000};
    stream->avg_frame_rate = {30, 1};

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    require(codec != nullptr, "MPEG-2 encoder missing");
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    require(encoder != nullptr, "avcodec_alloc_context3");
    encoder->width = 320;
    encoder->height = 240;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1, 30};   // MPEG-1/2 encoders want 1/frame_rate
    encoder->framerate = {30, 1};
    encoder->gop_size = 15;
    encoder->max_b_frames = 0;
    encoder->bit_rate = 1'500'000;
    require(avcodec_open2(encoder, codec, nullptr) >= 0, "avcodec_open2");
    require(avcodec_parameters_from_context(stream->codecpar, encoder) >= 0,
            "avcodec_parameters_from_context");

    MemorySink sink;
    constexpr int kBuffer = 65536;
    auto* avio_buffer = static_cast<uint8_t*>(av_malloc(kBuffer));
    require(avio_buffer != nullptr, "av_malloc");
    AVIOContext* avio = avio_alloc_context(avio_buffer, kBuffer, 1, &sink,
                                           nullptr, write_packet, nullptr);
    require(avio != nullptr, "avio_alloc_context");
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    require(avformat_write_header(format, nullptr) >= 0, "avformat_write_header");

    AVFrame* frame = av_frame_alloc();
    require(frame != nullptr, "av_frame_alloc");
    frame->format = encoder->pix_fmt;
    frame->width = encoder->width;
    frame->height = encoder->height;
    require(av_frame_get_buffer(frame, 32) >= 0, "av_frame_get_buffer");
    AVPacket* packet = av_packet_alloc();
    require(packet != nullptr, "av_packet_alloc");

    auto write_available = [&] {
        while (avcodec_receive_packet(encoder, packet) == 0) {
            av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            require(av_interleaved_write_frame(format, packet) >= 0,
                    "av_interleaved_write_frame");
        }
    };
    for (int i = 0; i < frames; ++i) {
        require(av_frame_make_writable(frame) >= 0, "av_frame_make_writable");
        for (int y = 0; y < encoder->height; ++y)
            std::memset(frame->data[0] + y * frame->linesize[0],
                        16 + ((i * 4 + y) % 200), encoder->width);
        std::memset(frame->data[1], 128, frame->linesize[1] * encoder->height / 2);
        std::memset(frame->data[2], 128, frame->linesize[2] * encoder->height / 2);
        frame->pts = i + (gap_at >= 0 && i >= gap_at ? (int64_t)gap_seconds * 30 : 0);
        require(avcodec_send_frame(encoder, frame) >= 0, "avcodec_send_frame");
        write_available();
    }
    require(avcodec_send_frame(encoder, nullptr) >= 0, "avcodec_send_frame flush");
    write_available();
    require(av_write_trailer(format) >= 0, "av_write_trailer");

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    avformat_free_context(format);
    require(!sink.data.empty(), "empty MPEG-TS");
    return sink.data;
}

struct Source {
    const std::vector<uint8_t>* data;
    size_t offset = 0;
    int read(uint8_t* buffer, int size) {
        size_t left = data->size() - offset;
        if (left == 0) return -1;
        size_t take = std::min<size_t>((size_t)size, left);
        std::memcpy(buffer, data->data() + offset, take);
        offset += take;
        return (int)take;
    }
};

player::ReadFn reader_for(Source& source) {
    return [&source](uint8_t* buffer, int size) { return source.read(buffer, size); };
}

void test_playback_and_eof(SDL_Renderer* renderer) {
    std::vector<uint8_t> ts = build_ts(30);
    Source source{&ts};
    player::Player player;
    require(player.open(reader_for(source), renderer), "player open failed");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!player.ended() && std::chrono::steady_clock::now() < deadline) {
        player.service();
        SDL_Delay(2);
    }
    require(player.ended(), "player did not reach EOF drain");
    const player::Stats& stats = player.stats();
    require(stats.frames_decoded >= 30, "decoder lost frames");
    require(stats.frames_processed >= 30, "processor did not preserve every frame");
    require(stats.frames_shown >= 20, "presentation lost too many frames");
    require(stats.frames_shown <= stats.frames_processed + stats.frames_dropped,
            "more pictures shown than processed");
    std::printf("[test] playback: decoded=%llu processed=%llu shown=%llu dropped=%llu\n",
                (unsigned long long)stats.frames_decoded,
                (unsigned long long)stats.frames_processed,
                (unsigned long long)stats.frames_shown,
                (unsigned long long)stats.frames_dropped);
    player.close();
    player.close();   // idempotent
}

void test_multiple_outputs(SDL_Renderer* renderer) {
    std::vector<uint8_t> ts = build_ts(60);
    Source source{&ts};
    player::Player player;
    require(player.open(reader_for(source), renderer), "player open failed");
    // Double the output rate; the processor boundary must emit more pictures
    // than were decoded and keep scheduling them.
    player.set_processing_graph("fps=60");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!player.ended() && std::chrono::steady_clock::now() < deadline) {
        player.service();
        SDL_Delay(2);
    }
    require(player.ended(), "multi-output playback did not finish");
    const player::Stats& stats = player.stats();
    require(stats.frames_processed > stats.frames_decoded + 10,
            "fps graph did not produce multiple outputs per input");
    require(stats.frames_shown >= 30, "multi-output presentation stalled");
    std::printf("[test] multi-output: decoded=%llu processed=%llu shown=%llu\n",
                (unsigned long long)stats.frames_decoded,
                (unsigned long long)stats.frames_processed,
                (unsigned long long)stats.frames_shown);
    player.close();
}

void test_method_change_continues(SDL_Renderer* renderer) {
    std::vector<uint8_t> ts = build_ts(60);
    Source source{&ts};
    player::Player player;
    require(player.open(reader_for(source), renderer), "player open failed");
    // Play briefly, then change the processing method mid-stream; the worker
    // resets temporal history and playback must continue to EOF.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    while (std::chrono::steady_clock::now() < deadline) {
        player.service();
        SDL_Delay(2);
    }
    uint64_t shown_before = player.stats().frames_shown;
    player.set_processing_graph("");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!player.ended() && std::chrono::steady_clock::now() < deadline) {
        player.service();
        SDL_Delay(2);
    }
    require(player.ended(), "playback did not finish after the method change");
    require(player.stats().frames_shown > shown_before,
            "no pictures after the processing method change");
    std::printf("[test] method change: shown %llu -> %llu\n",
                (unsigned long long)shown_before,
                (unsigned long long)player.stats().frames_shown);
    player.close();
}

void test_film_mode_playback(SDL_Renderer* renderer) {
    std::vector<uint8_t> ts = build_ts(30);
    Source source{&ts};
    player::Player player;
    video::ProcessingSettings film{video::ProcessingMode::Film,
                                   video::FieldOrder::Auto, false};
    require(player.open(reader_for(source), renderer, -1, {},
                        player::DecoderPreference::Auto, film),
            "film-mode player open failed");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!player.ended() && std::chrono::steady_clock::now() < deadline) {
        player.service();
        SDL_Delay(2);
    }
    require(player.ended(), "film-mode playback did not reach EOF");
    const player::Stats& stats = player.stats();
    require(stats.frames_processed >= 28, "film mode lost frames");
    require(stats.frames_processed <= stats.frames_decoded + 8,
            "film mode changed the nominal cadence");
    require(stats.frames_shown > 0, "film mode presented nothing");
    std::printf("[test] film mode: decoded=%llu processed=%llu shown=%llu\n",
                (unsigned long long)stats.frames_decoded,
                (unsigned long long)stats.frames_processed,
                (unsigned long long)stats.frames_shown);
    player.close();
}

void test_discontinuity_reset(SDL_Renderer* renderer) {
    std::vector<uint8_t> ts = build_ts(60, 30, 6);
    Source source{&ts};
    player::Player player;
    require(player.open(reader_for(source), renderer), "player open failed");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!player.ended() && std::chrono::steady_clock::now() < deadline) {
        player.service();
        SDL_Delay(2);
    }
    require(player.ended(), "playback did not finish after the timestamp jump");
    require(player.stats().resyncs >= 1, "timestamp jump did not reset the worker");
    std::printf("[test] discontinuity: resyncs=%llu shown=%llu\n",
                (unsigned long long)player.stats().resyncs,
                (unsigned long long)player.stats().frames_shown);
    player.close();
}

void test_cancellation_while_blocked(SDL_Renderer* renderer) {
    std::vector<uint8_t> ts = build_ts(60);
    Source source{&ts};
    player::Player player;
    require(player.open(reader_for(source), renderer), "player open failed");
    // Do not service: the worker fills the bounded processed queue and blocks.
    SDL_Delay(400);
    auto start = std::chrono::steady_clock::now();
    player.close();
    auto elapsed = std::chrono::steady_clock::now() - start;
    require(elapsed < std::chrono::milliseconds(800),
            "close() did not cancel a blocked worker promptly");
    std::printf("[test] cancellation: close took %lld ms\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

} // namespace

int main() {
    try {
        require(SDL_Init(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_AUDIO),
                "SDL init failed");
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        require(SDL_CreateWindowAndRenderer("hibiki worker test", 320, 240, 0,
                                            &window, &renderer),
                "SDL renderer creation failed");
        test_playback_and_eof(renderer);
        test_multiple_outputs(renderer);
        test_method_change_continues(renderer);
        test_film_mode_playback(renderer);
        test_discontinuity_reset(renderer);
        test_cancellation_while_blocked(renderer);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::cout << "Player worker playback, multi-output, EOF drain and "
                     "cancellation tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
