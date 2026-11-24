// Video processing boundary (see video_processor.h).

#include "video_processor.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cstdio>

namespace video {

OutputFrame& OutputFrame::operator=(OutputFrame&& other) noexcept {
    if (this != &other) {
        reset();
        frame = other.frame;
        pts = other.pts;
        time_base = other.time_base;
        duration = other.duration;
        generation = other.generation;
        sample_aspect_ratio = other.sample_aspect_ratio;
        color_space = other.color_space;
        color_range = other.color_range;
        chroma_location = other.chroma_location;
        interlaced = other.interlaced;
        top_field_first = other.top_field_first;
        other.frame = nullptr;
        other.pts = AV_NOPTS_VALUE;
    }
    return *this;
}

void OutputFrame::reset() {
    if (frame) av_frame_free(&frame);
    pts = AV_NOPTS_VALUE;
    time_base = {0, 1};
    duration = 0;
    generation = 0;
    sample_aspect_ratio = {0, 1};
    color_space = AVCOL_SPC_UNSPECIFIED;
    color_range = AVCOL_RANGE_UNSPECIFIED;
    chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    interlaced = false;
    top_field_first = false;
}

double OutputFrame::pts_seconds() const {
    if (pts == AV_NOPTS_VALUE || time_base.num <= 0 || time_base.den <= 0) return -1.0;
    return pts * av_q2d(time_base);
}

void VideoProcessor::set_error(const char* what, int err) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, text, sizeof(text));
    error_ = std::string(what) + ": " + text;
}

void VideoProcessor::teardown() {
    outputs_.clear();
    if (graph_) avfilter_graph_free(&graph_);
    graph_ = nullptr;
    source_ = nullptr;
    sink_ = nullptr;
    info_ = ProcessorInfo{};
    drained_ = false;
}

void VideoProcessor::reset() {
    teardown();
    error_.clear();
}

bool VideoProcessor::configure(const AVFrame* reference, AVRational input_time_base,
                               AVRational frame_rate, const ProcessorConfig& config) {
    reset();
    if (!reference || reference->width <= 0 || reference->height <= 0 ||
        reference->format < 0) {
        error_ = "configure: invalid reference frame";
        return false;
    }
    AVRational sar = reference->sample_aspect_ratio.num > 0
                         ? reference->sample_aspect_ratio : AVRational{1, 1};
    AVRational fps = frame_rate.num > 0 && frame_rate.den > 0 ? frame_rate
                                                              : AVRational{0, 1};
    info_.graph = config.graph.empty() ? "passthrough" : config.graph;
    info_.input_time_base = input_time_base;
    info_.width = reference->width;
    info_.height = reference->height;
    info_.pixel_format = reference->format;

    graph_ = avfilter_graph_alloc();
    if (!graph_) {
        error_ = "configure: out of memory";
        return false;
    }
    const AVFilter* buffer = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    source_ = avfilter_graph_alloc_filter(graph_, buffer, "in");
    sink_ = avfilter_graph_alloc_filter(graph_, buffersink, "out");
    if (!source_ || !sink_) {
        error_ = "configure: cannot create buffer filters";
        teardown();
        return false;
    }
    char options[384];
    std::snprintf(options, sizeof(options),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d"
                  ":colorspace=%d:range=%d:chroma_location=%d"
                  ":frame_rate=%d/%d",
                  reference->width, reference->height, reference->format,
                  input_time_base.num, input_time_base.den, sar.num, sar.den,
                  reference->colorspace, reference->color_range,
                  reference->chroma_location,
                  fps.num > 0 ? fps.num : 0, fps.num > 0 ? fps.den : 1);
    int ret = avfilter_init_str(source_, options);
    if (ret >= 0) ret = avfilter_init_str(sink_, nullptr);
    if (ret < 0) {
        set_error("configure: init filters", ret);
        teardown();
        return false;
    }

    if (config.graph.empty()) {
        ret = avfilter_link(source_, 0, sink_, 0);
    } else {
        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            set_error("configure: out of memory", AVERROR(ENOMEM));
            teardown();
            return false;
        }
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_;
        inputs->pad_idx = 0;
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_;
        outputs->pad_idx = 0;
        ret = avfilter_graph_parse_ptr(graph_, config.graph.c_str(),
                                       &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    }
    if (ret >= 0) ret = avfilter_graph_config(graph_, nullptr);
    if (ret < 0) {
        set_error("configure: build graph", ret);
        teardown();
        return false;
    }
    info_.configured = true;
    info_.output_time_base = av_buffersink_get_time_base(sink_);
    if (info_.output_time_base.num <= 0 || info_.output_time_base.den <= 0)
        info_.output_time_base = input_time_base;
    return true;
}

bool VideoProcessor::submit(const AVFrame* frame, uint64_t generation) {
    if (!graph_) {
        error_ = "submit: processor not configured";
        return false;
    }
    if (drained_) {
        error_ = "submit: graph already drained";
        return false;
    }
    int ret = av_buffersrc_add_frame_flags(source_, const_cast<AVFrame*>(frame),
                                           AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        set_error("submit: buffer source rejected frame", ret);
        return false;
    }
    return receive_outputs(generation);
}

bool VideoProcessor::flush(uint64_t generation) {
    if (!graph_ || drained_) return true;
    int ret = av_buffersrc_add_frame_flags(source_, nullptr, 0);
    if (ret < 0) {
        set_error("flush: buffer source rejected EOF", ret);
        return false;
    }
    return receive_outputs(generation);
}

bool VideoProcessor::receive_outputs(uint64_t generation) {
    for (;;) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            error_ = "receive: out of memory";
            return false;
        }
        int ret = av_buffersink_get_frame(sink_, frame);
        if (ret == AVERROR(EAGAIN)) {
            av_frame_free(&frame);
            break;
        }
        if (ret == AVERROR_EOF) {
            av_frame_free(&frame);
            drained_ = true;
            break;
        }
        if (ret < 0) {
            av_frame_free(&frame);
            set_error("receive: buffer sink failed", ret);
            return false;
        }
        OutputFrame output;
        output.frame = frame;
        output.pts = frame->pts;
        output.time_base = info_.output_time_base;
        output.duration = frame->duration > 0 ? frame->duration : 0;
        output.generation = generation;
        output.sample_aspect_ratio = frame->sample_aspect_ratio;
        output.color_space = frame->colorspace;
        output.color_range = frame->color_range;
        output.chroma_location = frame->chroma_location;
        output.interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        output.top_field_first = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
        outputs_.push_back(std::move(output));
    }
    return true;
}

bool VideoProcessor::take_output(OutputFrame& out) {
    if (outputs_.empty()) return false;
    out = std::move(outputs_.front());
    outputs_.pop_front();
    return true;
}

} // namespace video
