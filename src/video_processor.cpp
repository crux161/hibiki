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

const char* processing_mode_name(ProcessingMode mode) {
    switch (mode) {
        case ProcessingMode::Auto: return "auto";
        case ProcessingMode::Bwdif: return "bwdif";
        case ProcessingMode::Yadif: return "yadif";
        case ProcessingMode::Bob: return "bob";
        case ProcessingMode::Weave: return "weave";
        case ProcessingMode::Film: return "fieldmatch-bwdif";
        default: return "passthrough";
    }
}

bool processing_mode_supported(ProcessingMode mode, int pixel_format) {
    if (mode == ProcessingMode::PassThrough || mode == ProcessingMode::Weave)
        return true;
    if (pixel_format == AV_PIX_FMT_NV12) return true;   // converted to yuv420p
    return pixel_format == AV_PIX_FMT_YUV420P ||
           pixel_format == AV_PIX_FMT_YUV422P;
}

namespace {
const char* parity_name(FieldOrder order) {
    if (order == FieldOrder::TopFirst) return "tff";
    if (order == FieldOrder::BottomFirst) return "bff";
    return "auto";
}

// Build the FFmpeg graph for a processing mode. Validated against FFmpeg 9.1:
// send_field on interlaced input doubles the output rate (59.94 from 29.97),
// deint=interlaced preserves progressive timing, and Bob doubles regardless.
std::string mode_graph(const ProcessorConfig& config, int pixel_format,
                       AVRational sar) {
    std::string prefix;
    if (pixel_format == AV_PIX_FMT_NV12) prefix += "format=yuv420p,";
    if (config.field_order != FieldOrder::Auto &&
        config.mode != ProcessingMode::PassThrough)
        prefix += std::string("setfield=") +
                  (config.field_order == FieldOrder::TopFirst ? "tff" : "bff") + ",";
    const std::string deint = config.force ? "all" : "interlaced";
    char sar_option[48] = {};
    if (sar.num > 0 && sar.den > 0)
        std::snprintf(sar_option, sizeof(sar_option), ",setsar=%d/%d", sar.num, sar.den);
    switch (config.mode) {
        case ProcessingMode::Auto:
        case ProcessingMode::Bwdif:
            return prefix + "bwdif=mode=send_field:parity=" +
                   parity_name(config.field_order) + ":deint=" + deint + sar_option;
        case ProcessingMode::Yadif:
            return prefix + "yadif=mode=send_field:parity=" +
                   parity_name(config.field_order) + ":deint=" + deint + sar_option;
        case ProcessingMode::Film:
            // Field matching reconstructs frames from paired fields; BWDIF
            // send_frame then treats anything still combed (mixed film/video
            // edits). Nominal cadence is kept, so duplicates remain until a
            // separate decimation policy exists.
            return prefix +
                   "fieldmatch=order=" + parity_name(config.field_order) +
                   ":combmatch=full,bwdif=mode=send_frame:parity=" +
                   parity_name(config.field_order) + ":deint=" + deint +
                   sar_option;
        case ProcessingMode::Bob:
            // Extract both fields (top field first as flagged, or as forced by
            // setfield above) into half-height pictures, then reconstruct
            // full height by nearest-neighbour line duplication of luma and
            // chroma. Borders are duplicated, not extended.
            return prefix + "separatefields,scale=w=iw:h=2*ih:flags=neighbor" +
                   sar_option;
        case ProcessingMode::Weave:
            // Full-frame decoder output already carries both fields: Weave is
            // a pass-through at the coded frame rate. The optional setfield
            // prefix only corrects metadata.
            if (!prefix.empty()) return prefix.substr(0, prefix.size() - 1);
            return {};
        default:
            return {};
    }
}
} // namespace

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
    std::string graph_spec;
    if (!config.graph.empty()) {
        graph_spec = config.graph;
        info_.graph = config.graph;
    } else {
        if (!processing_mode_supported(config.mode, reference->format)) {
            error_ = std::string("configure: processing mode '") +
                     processing_mode_name(config.mode) +
                     "' does not support the decoded pixel format";
            return false;
        }
        graph_spec = mode_graph(config, reference->format, sar);
        info_.graph = processing_mode_name(config.mode);
    }
    info_.input_time_base = input_time_base;
    info_.width = reference->width;
    info_.height = reference->height;
    info_.pixel_format = reference->format;
    info_.sample_aspect_ratio = sar;
    info_.color_range = reference->color_range;
    info_.color_space = reference->colorspace;
    info_.chroma_location = reference->chroma_location;

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

    if (graph_spec.empty()) {
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
        ret = avfilter_graph_parse_ptr(graph_, graph_spec.c_str(),
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
        output.sample_aspect_ratio = frame->sample_aspect_ratio.num > 0
                                         ? frame->sample_aspect_ratio
                                         : info_.sample_aspect_ratio;
        output.color_space = frame->colorspace != AVCOL_SPC_UNSPECIFIED
                                 ? frame->colorspace : info_.color_space;
        output.color_range = frame->color_range != AVCOL_RANGE_UNSPECIFIED
                                 ? frame->color_range : info_.color_range;
        output.chroma_location = frame->chroma_location != AVCHROMA_LOC_UNSPECIFIED
                                     ? frame->chroma_location
                                     : info_.chroma_location;
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
