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

int main() {
    try {
        test_pass_through();
        test_unknown_timestamp();
        test_multiple_outputs_changed_time_base();
        test_settb_time_base();
        test_eof_drain();
        test_clean_reset();
        test_filter_error();
        std::cout << "Video processor pass-through, multi-output, time base, drain, "
                     "reset and error tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
