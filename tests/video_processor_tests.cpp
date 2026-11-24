// Synthetic tests for the video processing boundary (M1): pass-through
// preservation, multiple outputs with a changed time base, unknown timestamps,
// EOF drain, graph errors and clean resets. No SDL window or device is needed.

#include "video_processor.h"

extern "C" {
#include <libavutil/error.h>
}

#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

AVFrame* make_frame(int width, int height, int64_t pts, int64_t duration,
                    bool interlaced = false, bool tff = false) {
    AVFrame* frame = av_frame_alloc();
    require(frame != nullptr, "av_frame_alloc");
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    require(av_frame_get_buffer(frame, 32) >= 0, "av_frame_get_buffer");
    for (int y = 0; y < height; ++y)
        std::memset(frame->data[0] + y * frame->linesize[0], 16 + (y % 200), width);
    std::memset(frame->data[1], 128, frame->linesize[1] * height / 2);
    std::memset(frame->data[2], 128, frame->linesize[2] * height / 2);
    frame->pts = pts;
    frame->duration = duration;
    frame->sample_aspect_ratio = {4, 3};
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->chroma_location = AVCHROMA_LOC_LEFT;
    if (interlaced) frame->flags |= AV_FRAME_FLAG_INTERLACED;
    if (tff) frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    return frame;
}

std::vector<video::OutputFrame> take_all(video::VideoProcessor& processor,
                                         size_t expected = 0) {
    std::vector<video::OutputFrame> outputs;
    video::OutputFrame output;
    while (processor.take_output(output)) outputs.push_back(std::move(output));
    if (expected) require(outputs.size() == expected, "unexpected output count");
    return outputs;
}

void test_pass_through() {
    video::VideoProcessor processor;
    require(!processor.configured(), "processor starts unconfigured");
    AVFrame* reference = make_frame(64, 48, 0, 3003);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, {}),
            "pass-through configure failed");
    require(processor.configured(), "processor not configured");
    require(processor.info().graph == "passthrough", "graph name");
    require(processor.info().output_time_base.num == 1 &&
                processor.info().output_time_base.den == 90000,
            "sink time base is not the input time base for pass-through");

    std::vector<video::OutputFrame> outputs;
    for (int i = 0; i < 5; ++i) {
        AVFrame* frame = make_frame(64, 48, i * 3003, 3003, i % 2 == 1, i % 2 == 1);
        require(processor.submit(frame, 7), "pass-through submit failed");
        av_frame_free(&frame);
        auto batch = take_all(processor);
        for (auto& output : batch) outputs.push_back(std::move(output));
    }
    require(outputs.size() == 5, "pass-through lost frames");
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& output = outputs[i];
        require(output.valid(), "empty output frame");
        require(output.pts == (int64_t)i * 3003, "PTS not preserved");
        require(output.duration == 3003, "duration not preserved");
        require(output.time_base.num == 1 && output.time_base.den == 90000,
                "output time base not from the sink");
        require(output.generation == 7, "generation not stamped");
        require(output.sample_aspect_ratio.num == 4 &&
                    output.sample_aspect_ratio.den == 3, "SAR not carried");
        require(output.color_range == AVCOL_RANGE_MPEG, "range not carried");
        require(output.color_space == AVCOL_SPC_BT709, "colour space not carried");
        require(output.chroma_location == AVCHROMA_LOC_LEFT, "chroma location not carried");
        require(output.interlaced == (i % 2 == 1), "interlace flag not carried");
        require(output.top_field_first == (i % 2 == 1), "field order flag not carried");
    }
    av_frame_free(&reference);
}

void test_unknown_timestamp() {
    video::VideoProcessor processor;
    AVFrame* reference = make_frame(64, 48, 0, 3003);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, {}),
            "configure for unknown PTS");
    AVFrame* frame = make_frame(64, 48, AV_NOPTS_VALUE, 0);
    require(processor.submit(frame, 1), "submit unknown PTS");
    av_frame_free(&frame);
    auto outputs = take_all(processor, 1);
    require(outputs[0].pts == AV_NOPTS_VALUE, "unknown PTS was not preserved");
    require(outputs[0].pts_seconds() < 0, "unknown PTS seconds");
    av_frame_free(&reference);
}

void test_multiple_outputs_changed_time_base() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.graph = "fps=60";
    AVFrame* reference = make_frame(64, 48, 0, 1001);
    // Input at 29.97 fps; fps=60 emits roughly two outputs per input frame and
    // exposes the sink's own time base.
    require(processor.configure(reference, {1, 30000}, {30000, 1001}, config),
            "configure fps graph");
    require(processor.info().output_time_base.den != 30000,
            "fps graph did not change the sink time base");
    std::vector<int64_t> submitted_pts;
    for (int i = 0; i < 10; ++i) {
        submitted_pts.push_back(i * 1001);
        AVFrame* frame = make_frame(64, 48, i * 1001, 1001);
        require(processor.submit(frame, 2), "fps submit");
        av_frame_free(&frame);
    }
    require(processor.flush(2), "fps flush");
    require(processor.drained(), "graph not drained after flush");
    auto outputs = take_all(processor);
    require(outputs.size() >= 18, "fps graph did not produce multiple outputs");
    int64_t previous = AV_NOPTS_VALUE;
    for (const auto& output : outputs) {
        require(output.pts != AV_NOPTS_VALUE, "fps output lost its timestamp");
        require(output.time_base.num == processor.info().output_time_base.num &&
                    output.time_base.den == processor.info().output_time_base.den,
                "fps output time base mismatch");
        require(previous == AV_NOPTS_VALUE || output.pts > previous,
                "fps output timestamps not increasing");
        previous = output.pts;
        require(output.generation == 2, "fps output generation");
    }
    AVFrame* late = make_frame(64, 48, 0, 1001);
    require(!processor.submit(late, 2), "submit after EOF must fail");
    av_frame_free(&late);
    av_frame_free(&reference);
}

void test_settb_time_base() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.graph = "settb=expr=1/60000";
    AVFrame* reference = make_frame(64, 48, 0, 3003);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "configure settb");
    require(processor.info().output_time_base.num == 1 &&
                processor.info().output_time_base.den == 60000,
            "settb time base not reported from the sink");
    AVFrame* frame = make_frame(64, 48, 3003, 3003);
    require(processor.submit(frame, 3), "settb submit");
    av_frame_free(&frame);
    auto outputs = take_all(processor, 1);
    require(outputs[0].pts == 2002, "PTS was not rescaled to the sink time base");
    av_frame_free(&reference);
}

void test_eof_drain() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.graph = "tpad=stop=2:stop_mode=clone";
    AVFrame* reference = make_frame(64, 48, 0, 3003);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "configure tpad");
    for (int i = 0; i < 3; ++i) {
        AVFrame* frame = make_frame(64, 48, i * 3003, 3003);
        require(processor.submit(frame, 4), "tpad submit");
        av_frame_free(&frame);
    }
    require(!processor.drained(), "drained before EOF");
    require(processor.flush(4), "tpad flush");
    require(processor.drained(), "EOF drain did not finish");
    auto outputs = take_all(processor);
    require(outputs.size() == 5, "tpad outputs missing after drain");
    av_frame_free(&reference);
}

void test_clean_reset() {
    video::VideoProcessor processor;
    AVFrame* reference = make_frame(64, 48, 0, 3003);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, {}),
            "configure reset test");
    for (int i = 0; i < 2; ++i) {
        AVFrame* frame = make_frame(64, 48, i * 3003, 3003);
        require(processor.submit(frame, 1), "submit before reset");
        av_frame_free(&frame);
    }
    require(processor.queued_outputs() > 0, "outputs should be pending before reset");
    processor.reset();
    require(!processor.configured(), "reset kept the graph");
    require(processor.queued_outputs() == 0, "reset kept queued outputs");
    video::OutputFrame stale;
    require(!processor.take_output(stale), "reset did not discard outputs");

    AVFrame* second = make_frame(48, 32, 0, 3003);
    require(processor.configure(second, {1, 90000}, {30000, 1001}, {}),
            "reconfigure after reset");
    require(processor.info().width == 48 && processor.info().height == 32,
            "reconfigure kept the old format");
    for (int i = 0; i < 3; ++i) {
        AVFrame* frame = make_frame(48, 32, i * 3003, 3003);
        require(processor.submit(frame, 9), "submit after reset");
        av_frame_free(&frame);
    }
    auto outputs = take_all(processor, 3);
    for (const auto& output : outputs) require(output.generation == 9, "stale generation");
    processor.reset();
    processor.reset();  // idempotent
    av_frame_free(&reference);
    av_frame_free(&second);
}

void test_filter_error() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.graph = "definitely-not-a-filter";
    AVFrame* reference = make_frame(64, 48, 0, 3003);
    require(!processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "invalid graph configured successfully");
    require(!processor.error().empty(), "invalid graph left no error");
    require(!processor.configured(), "failed configure left a graph");
    av_frame_free(&reference);
}

} // namespace

// ---------------------------------------------------------------------------
// M2 processing modes: cadence, field order, Bob reconstruction, chroma,
// format negotiation and mode changes.

namespace {

AVFrame* make_bar_frame(int width, int height, int64_t pts,
                        int even_x, int odd_x, bool interlaced, bool tff,
                        AVPixelFormat format = AV_PIX_FMT_YUV420P) {
    AVFrame* frame = av_frame_alloc();
    require(frame != nullptr, "av_frame_alloc");
    frame->format = format;
    frame->width = width;
    frame->height = height;
    require(av_frame_get_buffer(frame, 32) >= 0, "av_frame_get_buffer");
    for (int y = 0; y < height; ++y) {
        uint8_t* row = frame->data[0] + y * frame->linesize[0];
        std::memset(row, 16, width);
        int x0 = y % 2 == 0 ? even_x : odd_x;
        if (x0 >= 0 && x0 + 16 <= width)
            std::memset(row + x0, 235, 16);
    }
    for (int plane = 1; plane < 3; ++plane) {
        int height_div = format == AV_PIX_FMT_YUV422P ? 1 : 2;
        std::memset(frame->data[plane], 128,
                    frame->linesize[plane] * (height / height_div));
    }
    frame->pts = pts;
    frame->duration = 3003;
    frame->sample_aspect_ratio = {4, 3};
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    if (interlaced) frame->flags |= AV_FRAME_FLAG_INTERLACED;
    if (tff) frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    return frame;
}

int bar_start(const AVFrame* frame, int row) {
    const uint8_t* line = frame->data[0] + row * frame->linesize[0];
    for (int x = 0; x < frame->width; ++x)
        if (line[x] >= 200) return x;
    return -1;
}

void submit_bar(video::VideoProcessor& processor, int frame_index, bool interlaced,
                bool tff, uint64_t generation, int width = 320, int height = 240) {
    int even_x = 32 + 8 * frame_index;
    int odd_x = even_x + 4;
    AVFrame* frame = make_bar_frame(width, height, frame_index * 3003,
                                    even_x, odd_x, interlaced, tff);
    require(processor.submit(frame, generation), "mode submit");
    av_frame_free(&frame);
}

std::vector<video::OutputFrame> run_mode(video::ProcessingMode mode, int frames,
                                         bool interlaced, bool tff,
                                         video::FieldOrder order = video::FieldOrder::Auto,
                                         bool force = false) {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.mode = mode;
    config.field_order = order;
    config.force = force;
    AVFrame* reference = make_bar_frame(320, 240, 0, 32, 36, interlaced, tff);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "mode configure");
    for (int i = 0; i < frames; ++i)
        submit_bar(processor, i, interlaced, tff, 11);
    require(processor.flush(11), "mode flush");
    require(processor.drained(), "mode graph not drained");
    av_frame_free(&reference);
    return take_all(processor);
}

void test_auto_and_explicit_field_modes() {
    for (auto mode : {video::ProcessingMode::Auto, video::ProcessingMode::Bwdif,
                      video::ProcessingMode::Yadif, video::ProcessingMode::Bob}) {
        auto outputs = run_mode(mode, 10, true, true);
        const char* name = video::processing_mode_name(mode);
        require(outputs.size() == 20, "field mode did not double the frame count");
        require(outputs[0].time_base.den != 90000 ||
                    outputs[0].time_base.num != 1,
                "field mode kept the stream time base");
        int64_t previous = AV_NOPTS_VALUE;
        for (size_t i = 0; i < outputs.size(); ++i) {
            const auto& out = outputs[i];
            require(out.pts != AV_NOPTS_VALUE && out.pts > previous,
                    "field mode produced non-increasing timestamps");
            previous = out.pts;
            require(out.frame->width == 320 && out.frame->height == 240,
                    "field mode changed the picture size");
            require((out.frame->flags & AV_FRAME_FLAG_INTERLACED) == 0,
                    "field mode output still marked interlaced");
            if (!(out.sample_aspect_ratio.num == 4 &&
                  out.sample_aspect_ratio.den == 3)) {
                std::fprintf(stderr, "mode SAR: %d/%d\n",
                             out.sample_aspect_ratio.num,
                             out.sample_aspect_ratio.den);
                throw std::runtime_error("field mode lost the sample aspect ratio");
            }
            require(out.color_range == AVCOL_RANGE_MPEG,
                    "field mode lost the colour range");
            int expected = 32 + 8 * (int)(i / 2) + (i % 2 ? 4 : 0);
            int found = bar_start(out.frame, 20);
            require(found >= 0 && std::abs(found - expected) <= 2,
                    "field mode output is not in top/bottom field order");
            (void)name;
        }
    }
}

void test_weave_keeps_paired_fields() {
    auto outputs = run_mode(video::ProcessingMode::Weave, 10, true, true);
    require(outputs.size() == 10, "weave changed the coded frame rate");
    for (size_t i = 0; i < outputs.size(); ++i) {
        require(outputs[i].time_base.num == 1 && outputs[i].time_base.den == 90000,
                "weave changed the time base");
        require(outputs[i].sample_aspect_ratio.num == 4 &&
                    outputs[i].sample_aspect_ratio.den == 3,
                "weave lost the sample aspect ratio");
        int even_expected = 32 + 8 * (int)i;
        require(std::abs(bar_start(outputs[i].frame, 20) - even_expected) <= 2,
                "weave lost the top field content");
        require(std::abs(bar_start(outputs[i].frame, 21) - (even_expected + 4)) <= 2,
                "weave did not preserve the bottom field content");
    }
}

void test_auto_preserves_progressive_timing() {
    for (auto mode : {video::ProcessingMode::Auto, video::ProcessingMode::Bwdif,
                      video::ProcessingMode::Yadif}) {
        auto outputs = run_mode(mode, 10, false, false);
        require(outputs.size() == 10, "progressive timing was not preserved");
        // The sink may rescale the time base (bwdif uses half the input tick
        // size), but one output per input must keep the same wall-clock timing.
        for (size_t i = 0; i < outputs.size(); ++i) {
            double expected = i * 3003 / 90000.0;
            require(std::abs(outputs[i].pts_seconds() - expected) < 1e-6,
                    "progressive timing changed");
            require((outputs[i].frame->flags & AV_FRAME_FLAG_INTERLACED) == 0,
                    "progressive frame was marked interlaced");
        }
    }
}

void test_force_and_bob_on_progressive() {
    auto forced = run_mode(video::ProcessingMode::Bwdif, 10, false, false,
                           video::FieldOrder::Auto, true);
    require(forced.size() == 20, "force did not deinterlace progressive frames");
    auto bobbed = run_mode(video::ProcessingMode::Bob, 10, false, false);
    require(bobbed.size() == 20, "bob did not split progressive frames");
}

void test_bob_line_duplication() {
    auto outputs = run_mode(video::ProcessingMode::Bob, 4, true, true);
    require(outputs.size() == 8, "bob output count");
    // Nearest-neighbour reconstruction duplicates each source field line, so
    // consecutive output rows in both the top- and bottom-field pictures match.
    for (const auto& out : outputs) {
        const uint8_t* first = out.frame->data[0] + 20 * out.frame->linesize[0];
        const uint8_t* second = out.frame->data[0] + 21 * out.frame->linesize[0];
        require(std::memcmp(first, second, out.frame->width / 2) == 0,
                "bob did not duplicate lines");
        const uint8_t* top = out.frame->data[0] + 0 * out.frame->linesize[0];
        const uint8_t* next = out.frame->data[0] + 1 * out.frame->linesize[0];
        require(std::memcmp(top, next, out.frame->width / 2) == 0,
                "bob mishandled the top border");
    }
}

void test_field_order_override() {
    // Content is TFF but the stream is misflagged; forcing TFF must keep the
    // top field first in field-sending modes.
    auto outputs = run_mode(video::ProcessingMode::Bwdif, 4, false, false,
                            video::FieldOrder::TopFirst, true);
    require(outputs.size() == 8, "override did not deinterlace");
    require(std::abs(bar_start(outputs[0].frame, 20) - 32) <= 2,
            "TFF override did not put the top field first");
    require(std::abs(bar_start(outputs[1].frame, 20) - 36) <= 2,
            "TFF override did not put the bottom field second");
}

void test_chroma_and_format_negotiation() {
    // 4:2:0 chroma block survives filtering.
    {
        video::VideoProcessor processor;
        video::ProcessorConfig config;
        config.mode = video::ProcessingMode::Bwdif;
        config.force = true;   // progressive frame: force field output
        AVFrame* reference = make_bar_frame(320, 240, 0, 32, 36, false, false);
        for (int y = 20; y < 60; ++y)
            std::memset(reference->data[1] + y * reference->linesize[1] + 20, 100, 40);
        require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
                "chroma configure");
        AVFrame* frame = make_bar_frame(320, 240, 0, 32, 36, false, false);
        for (int y = 20; y < 60; ++y)
            std::memset(frame->data[1] + y * frame->linesize[1] + 20, 100, 40);
        require(processor.submit(frame, 1), "chroma submit");
        av_frame_free(&frame);
        require(processor.flush(1), "chroma flush");
        auto outputs = take_all(processor);
        require(outputs.size() == 2, "chroma field outputs");
        for (const auto& out : outputs) {
            uint8_t value = out.frame->data[1][40 * out.frame->linesize[1] + 40];
            require(std::abs((int)value - 100) <= 8,
                    "chroma block did not survive deinterlacing");
            uint8_t background = out.frame->data[1][5 * out.frame->linesize[1] + 5];
            require(std::abs((int)background - 128) <= 8,
                    "chroma background changed");
            uint8_t luma = out.frame->data[0][20 * out.frame->linesize[0] + 5];
            require(luma == 16, "luma background changed");
        }
        av_frame_free(&reference);
    }
    // 4:2:2 is filtered without a conversion.
    {
        video::VideoProcessor processor;
        video::ProcessorConfig config;
        config.mode = video::ProcessingMode::Bwdif;
        AVFrame* reference = make_bar_frame(320, 240, 0, 32, 36, true, true,
                                            AV_PIX_FMT_YUV422P);
        require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
                "422 configure");
        AVFrame* frame = make_bar_frame(320, 240, 0, 32, 36, true, true,
                                        AV_PIX_FMT_YUV422P);
        require(processor.submit(frame, 1), "422 submit");
        av_frame_free(&frame);
        require(processor.flush(1), "422 flush");
        auto outputs = take_all(processor);
        require(outputs.size() == 2, "422 output count");
        require(outputs[0].frame->format == AV_PIX_FMT_YUV422P, "422 format changed");
        av_frame_free(&reference);

        // Bob keeps 4:2:2 as well (nearest-neighbour field reconstruction).
        video::VideoProcessor bob;
        config.mode = video::ProcessingMode::Bob;
        reference = make_bar_frame(320, 240, 0, 32, 36, true, true,
                                   AV_PIX_FMT_YUV422P);
        require(bob.configure(reference, {1, 90000}, {30000, 1001}, config),
                "422 bob configure");
        frame = make_bar_frame(320, 240, 0, 32, 36, true, true,
                               AV_PIX_FMT_YUV422P);
        require(bob.submit(frame, 2), "422 bob submit");
        av_frame_free(&frame);
        require(bob.flush(2), "422 bob flush");
        auto bobbed = take_all(bob);
        require(bobbed.size() == 2, "422 bob output count");
        require(bobbed[0].frame->format == AV_PIX_FMT_YUV422P,
                "422 bob format changed");
        av_frame_free(&reference);
    }
    // NV12 is negotiated through an explicit yuv420p conversion.
    {
        require(video::processing_mode_supported(video::ProcessingMode::Bwdif,
                                                 AV_PIX_FMT_NV12),
                "NV12 should be accepted by negotiation");
        video::VideoProcessor processor;
        video::ProcessorConfig config;
        config.mode = video::ProcessingMode::Bwdif;
        AVFrame* reference = av_frame_alloc();
        require(reference != nullptr, "nv12 frame");
        reference->format = AV_PIX_FMT_NV12;
        reference->width = 320;
        reference->height = 240;
        require(av_frame_get_buffer(reference, 32) >= 0, "nv12 buffer");
        std::memset(reference->data[0], 16, reference->linesize[0] * 240);
        std::memset(reference->data[1], 128, reference->linesize[1] * 120);
        reference->pts = 0;
        reference->duration = 3003;
        reference->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
        require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
                "nv12 configure");
        require(processor.submit(reference, 2), "nv12 submit");
        require(processor.flush(2), "nv12 flush");
        auto outputs = take_all(processor);
        require(outputs.size() == 2, "nv12 output count");
        require(outputs[0].frame->format == AV_PIX_FMT_YUV420P,
                "NV12 was not converted to yuv420p");
        av_frame_free(&reference);
    }
    // Other formats are rejected explicitly, so the player can fall back.
    require(!video::processing_mode_supported(video::ProcessingMode::Bwdif,
                                              AV_PIX_FMT_YUV444P),
            "yuv444p should not be advertised as supported");
    {
        video::VideoProcessor processor;
        video::ProcessorConfig config;
        config.mode = video::ProcessingMode::Bwdif;
        AVFrame* reference = make_bar_frame(320, 240, 0, 32, 36, true, true,
                                            AV_PIX_FMT_YUV444P);
        require(!processor.configure(reference, {1, 90000}, {30000, 1001}, config),
                "unsupported format was configured");
        require(!processor.error().empty(), "unsupported format left no error");
        av_frame_free(&reference);
    }
}

void test_mode_change_resets() {
    video::VideoProcessor processor;
    AVFrame* reference = make_bar_frame(320, 240, 0, 32, 36, true, true);
    video::ProcessorConfig config;
    config.mode = video::ProcessingMode::Bwdif;
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "mode change first configure");
    for (int i = 0; i < 2; ++i) submit_bar(processor, i, true, true, 1);
    require(processor.flush(1), "mode change first flush");
    auto first = take_all(processor);
    require(first.size() == 4, "mode change first outputs");
    av_frame_free(&reference);

    processor.reset();
    config.mode = video::ProcessingMode::Weave;
    reference = make_bar_frame(320, 240, 0, 32, 36, true, true);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "mode change second configure");
    submit_bar(processor, 0, true, true, 2);
    auto second = take_all(processor);
    require(second.size() == 1, "weave mode change output count");
    require(second[0].generation == 2, "mode change generation");
    require(second[0].time_base.den == 90000, "weave mode time base");
    av_frame_free(&reference);
}

// ---------------------------------------------------------------------------
// M3 film mode: fieldmatch + BWDIF reconstructs telecine at nominal cadence.

namespace {

std::pair<int, int> telecine_films(int frame_index) {
    static const int cadence[5][2] = {{0, 0}, {0, 1}, {1, 2}, {2, 2}, {3, 3}};
    int cycle = frame_index / 5;
    int slot = frame_index % 5;
    return {cycle * 4 + cadence[slot][0], cycle * 4 + cadence[slot][1]};
}

AVFrame* make_telecine_frame(int frame_index, int64_t pts) {
    auto films = telecine_films(frame_index);
    return make_bar_frame(320, 240, pts, 32 + 16 * films.first,
                          32 + 16 * films.second, true, true);
}

// Dense moving bars give fieldmatch's block-based comb detector enough energy
// to flag residual video-rate combing, unlike the single narrow bar.
AVFrame* make_multibar_video_frame(int64_t pts, int shift_even, int shift_odd) {
    AVFrame* frame = av_frame_alloc();
    require(frame != nullptr, "av_frame_alloc");
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 320;
    frame->height = 240;
    require(av_frame_get_buffer(frame, 32) >= 0, "av_frame_get_buffer");
    for (int y = 0; y < 240; ++y) {
        uint8_t* row = frame->data[0] + y * frame->linesize[0];
        std::memset(row, 16, 320);
        int shift = y % 2 == 0 ? shift_even : shift_odd;
        for (int k = 0; k < 4; ++k) {
            int x = 20 + 70 * k + shift;
            if (x + 40 <= 320) std::memset(row + x, 235, 40);
        }
    }
    for (int plane = 1; plane < 3; ++plane)
        std::memset(frame->data[plane], 128, frame->linesize[plane] * 120);
    frame->pts = pts;
    frame->duration = 3003;
    frame->sample_aspect_ratio = {4, 3};
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    return frame;
}

void test_fieldmatch_reconstructs_telecine() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.mode = video::ProcessingMode::Film;
    AVFrame* reference = make_telecine_frame(0, 0);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "fieldmatch configure");
    for (int i = 0; i < 10; ++i) {
        AVFrame* frame = make_telecine_frame(i, i * 3003);
        require(processor.submit(frame, 3), "fieldmatch submit");
        av_frame_free(&frame);
    }
    require(processor.flush(3), "fieldmatch flush");
    require(processor.drained(), "fieldmatch graph not drained");
    auto outputs = take_all(processor);
    av_frame_free(&reference);
    require(outputs.size() == 10, "fieldmatch changed the nominal cadence");
    // Field matching pairs fields from the same film frame; one duplicate per
    // cadence group remains because this milestone does not decimate.
    const int expected_films[10] = {0, 0, 1, 2, 3, 4, 4, 5, 6, 7};
    double previous_time = -1.0;
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& out = outputs[i];
        int even = bar_start(out.frame, 20);
        int odd = bar_start(out.frame, 21);
        require(even == odd, "fieldmatch left combed output");
        int film = (even - 32) / 16;
        if (film != expected_films[i]) {
            std::fprintf(stderr, "frame %zu film %d expected %d (x=%d)\n", i, film,
                         expected_films[i], even);
            require(false, "fieldmatch paired the wrong film frames");
        }
        require((out.frame->flags & AV_FRAME_FLAG_INTERLACED) == 0,
                "fieldmatch output marked interlaced");
        require(out.pts_seconds() > previous_time,
                "fieldmatch timestamps not increasing");
        previous_time = out.pts_seconds();
        require(std::abs(out.pts_seconds() - i * 3003 / 90000.0) < 1e-6,
                "fieldmatch changed the output timing");
    }
}

void test_fieldmatch_handles_film_video_edit() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.mode = video::ProcessingMode::Film;
    AVFrame* reference = make_telecine_frame(0, 0);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "fieldmatch mixed configure");
    for (int i = 0; i < 5; ++i) {
        AVFrame* frame = make_telecine_frame(i, i * 3003);
        require(processor.submit(frame, 4), "mixed telecine submit");
        av_frame_free(&frame);
    }
    // Hard cut to 59.94-field video motion: each field is a new sample.
    for (int j = 0; j < 7; ++j) {
        AVFrame* frame = make_multibar_video_frame((5 + j) * 3003, 8 * j,
                                                   8 * j + 4);
        require(processor.submit(frame, 4), "mixed video submit");
        av_frame_free(&frame);
    }
    require(processor.flush(4), "mixed flush");
    auto outputs = take_all(processor);
    av_frame_free(&reference);
    require(outputs.size() == 12, "film/video edit changed the duration");
    const int expected_films[5] = {0, 0, 1, 2, 3};
    for (int i = 0; i < 5; ++i) {
        int even = bar_start(outputs[i].frame, 20);
        int odd = bar_start(outputs[i].frame, 21);
        require(even == odd, "telecine half still combed after fieldmatch");
        require((even - 32) / 16 == expected_films[i],
                "telecine half paired the wrong film frames");
    }
    // The video half keeps the nominal cadence; BWDIF's send_frame stage is
    // covered separately with frames that remain flagged interlaced, which is
    // how the real mixed fixture reaches it after the edit's scene change.
    double previous = outputs[4].pts_seconds();
    for (int j = 0; j < 7; ++j) {
        const auto& out = outputs[5 + j];
        require(out.pts_seconds() > previous,
                "film/video edit timestamps not increasing");
        previous = out.pts_seconds();
        require(bar_start(out.frame, 20) >= 0,
                "film/video edit output lost its picture");
    }
}

void test_bwdif_send_frame_cleans_residual() {
    // The residual stage used by film mode: one output per frame, deinterlace
    // frames still flagged interlaced.
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.graph = "bwdif=mode=send_frame:parity=auto:deint=interlaced";
    AVFrame* reference = make_bar_frame(320, 240, 0, 32, 36, true, true);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "send_frame configure");
    for (int i = 0; i < 6; ++i) {
        AVFrame* frame = make_bar_frame(320, 240, i * 3003, 32 + 8 * i,
                                        36 + 8 * i, true, true);
        require(processor.submit(frame, 8), "send_frame submit");
        av_frame_free(&frame);
    }
    require(processor.flush(8), "send_frame flush");
    auto outputs = take_all(processor);
    av_frame_free(&reference);
    require(outputs.size() == 6, "send_frame changed the frame count");
    for (size_t i = 0; i < outputs.size(); ++i) {
        int even = bar_start(outputs[i].frame, 20);
        int odd = bar_start(outputs[i].frame, 21);
        require(even == odd, "send_frame left residual combing");
        require(even == 32 + 8 * (int)i, "send_frame field phase");
        require((outputs[i].frame->flags & AV_FRAME_FLAG_INTERLACED) == 0,
                "send_frame output still marked interlaced");
    }
}

void test_fieldmatch_history_resets() {
    video::VideoProcessor processor;
    video::ProcessorConfig config;
    config.mode = video::ProcessingMode::Film;
    AVFrame* reference = make_telecine_frame(0, 0);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "film reset configure");
    for (int i = 0; i < 5; ++i) {
        AVFrame* frame = make_telecine_frame(i, i * 3003);
        require(processor.submit(frame, 5), "film reset submit");
        av_frame_free(&frame);
    }
    require(processor.flush(5), "film reset flush");
    auto first = take_all(processor);
    require(first.size() == 5, "film reset first outputs");
    av_frame_free(&reference);

    // A pipeline reset (retune/edit/discontinuity path) must discard matching
    // history; the new generation reconstructs from a fresh start.
    processor.reset();
    reference = make_telecine_frame(0, 0);
    require(processor.configure(reference, {1, 90000}, {30000, 1001}, config),
            "film reconfigure");
    for (int i = 0; i < 5; ++i) {
        AVFrame* frame = make_telecine_frame(i, i * 3003);
        require(processor.submit(frame, 6), "film reset new submit");
        av_frame_free(&frame);
    }
    require(processor.flush(6), "film reset new flush");
    auto second = take_all(processor);
    av_frame_free(&reference);
    require(second.size() == 5, "film reset lost the nominal cadence");
    const int expected_films[5] = {0, 0, 1, 2, 3};
    for (size_t i = 0; i < second.size(); ++i) {
        require(second[i].generation == 6, "film reset generation");
        int even = bar_start(second[i].frame, 20);
        require(even == bar_start(second[i].frame, 21),
                "film reset output combed");
        require((even - 32) / 16 == expected_films[i],
                "film reset kept stale matching history");
    }
}

} // namespace

} // namespace

int main() {
    try {
        test_pass_through();
        test_unknown_timestamp();
        test_multiple_outputs_changed_time_base();
        test_settb_time_base();
        test_eof_drain();
        test_clean_reset();
        test_filter_error();
        test_auto_and_explicit_field_modes();
        test_weave_keeps_paired_fields();
        test_auto_preserves_progressive_timing();
        test_force_and_bob_on_progressive();
        test_bob_line_duplication();
        test_field_order_override();
        test_chroma_and_format_negotiation();
        test_mode_change_resets();
        test_fieldmatch_reconstructs_telecine();
        test_fieldmatch_handles_film_video_edit();
        test_bwdif_send_frame_cleans_residual();
        test_fieldmatch_history_resets();
        std::cout << "Video processor pass-through, multi-output, time base, drain, "
                     "reset, mode/cadence, field-order, Bob, chroma, format "
                     "negotiation and fieldmatch tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
