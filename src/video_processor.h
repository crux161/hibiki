// Video processing boundary between decoding and the display queue.
//
// Owns an optional libavfilter graph fed from a buffer source and drained
// through a buffer sink. An empty graph links source to sink directly as a
// pass-through that still exercises the boundary: outputs carry the sink's
// time base, not the input stream's, and metadata survives unchanged. Later
// milestones configure BWDIF, YADIF, Bob, Weave and fieldmatch here.
//
// This module deliberately has no SDL or Apple-framework dependencies so it can
// be built and tested without a window.

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace video {

// CPU processing modes (M2). Auto applies BWDIF only to frames flagged
// interlaced; explicit BWDIF/YADIF honour flags unless force is set; Bob
// always reconstructs each field; Weave keeps paired fields at the coded frame
// rate (a pass-through for full-frame decoder output).
enum class ProcessingMode { PassThrough, Auto, Bwdif, Yadif, Bob, Weave };
enum class FieldOrder { Auto, TopFirst, BottomFirst };

struct ProcessingSettings {
    ProcessingMode mode = ProcessingMode::PassThrough;
    FieldOrder field_order = FieldOrder::Auto;
    bool force = false;   // deinterlace even frames not flagged interlaced
};

// Stable lowercase name for diagnostics/HUD ("passthrough", "auto", ...).
const char* processing_mode_name(ProcessingMode mode);
// Pixel-format negotiation: 8-bit planar 4:2:0/4:2:2 are supported directly,
// NV12 is converted to yuv420p before filtering, Weave/pass-through accept
// everything. Other formats must fall back to pass-through.
bool processing_mode_supported(ProcessingMode mode, int pixel_format);

// One processed picture, ready for the display queue. Owns its AVFrame.
struct OutputFrame {
    AVFrame* frame = nullptr;
    int64_t pts = AV_NOPTS_VALUE;      // integer PTS in time_base (AV_NOPTS_VALUE if unknown)
    AVRational time_base{0, 1};
    int64_t duration = 0;              // in time_base units, 0 when unknown
    uint64_t generation = 0;           // stream generation this picture belongs to
    AVRational sample_aspect_ratio{0, 1};
    AVColorSpace color_space = AVCOL_SPC_UNSPECIFIED;
    AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    bool interlaced = false;
    bool top_field_first = false;

    OutputFrame() = default;
    ~OutputFrame() { reset(); }
    OutputFrame(const OutputFrame&) = delete;
    OutputFrame& operator=(const OutputFrame&) = delete;
    OutputFrame(OutputFrame&& other) noexcept { *this = std::move(other); }
    OutputFrame& operator=(OutputFrame&& other) noexcept;

    void reset();
    bool valid() const { return frame != nullptr; }
    // Seconds from the time base; negative when the timestamp is unknown.
    double pts_seconds() const;
};

struct ProcessorConfig {
    std::string graph;  // explicit libavfilter chain; overrides mode when set
    ProcessingMode mode = ProcessingMode::PassThrough;
    FieldOrder field_order = FieldOrder::Auto;
    bool force = false;
};

struct ProcessorInfo {
    bool configured = false;
    std::string graph = "passthrough";
    AVRational input_time_base{0, 1};
    AVRational output_time_base{0, 1};
    int width = 0;
    int height = 0;
    int pixel_format = -1;
    // Metadata of the configured reference frame; outputs fall back to these
    // when a filter does not propagate per-frame properties.
    AVRational sample_aspect_ratio{0, 1};
    AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace color_space = AVCOL_SPC_UNSPECIFIED;
    AVChromaLocation chroma_location = AVCHROMA_LOC_UNSPECIFIED;
};

class VideoProcessor {
public:
    VideoProcessor() = default;
    ~VideoProcessor() { close(); }
    VideoProcessor(const VideoProcessor&) = delete;
    VideoProcessor& operator=(const VideoProcessor&) = delete;

    // Configure for a decoded reference frame. input_time_base is the time base
    // of frame PTS values passed to submit(); frame_rate is the stream's
    // nominal rate (used by rate-changing filters). False on failure (error()),
    // including an unsupported mode/pixel-format combination.
    bool configure(const AVFrame* reference, AVRational input_time_base,
                   AVRational frame_rate, const ProcessorConfig& config);
    // Drop the graph and every queued output. Safe to call repeatedly.
    void reset();
    void close() { reset(); }
    bool configured() const { return graph_ != nullptr; }

    // Feed one decoded frame (PTS must already be set; AV_NOPTS_VALUE is
    // preserved through the graph and reported on the output). May produce
    // zero, one or many outputs, retrieved with take_output().
    bool submit(const AVFrame* frame, uint64_t generation);
    // Push EOF into the graph so buffered outputs are emitted. Call after the
    // decoder has been drained. generation stamps the emitted outputs.
    bool flush(uint64_t generation);
    bool drained() const { return drained_; }

    bool take_output(OutputFrame& out);
    size_t queued_outputs() const { return outputs_.size(); }

    const ProcessorInfo& info() const { return info_; }
    const std::string& error() const { return error_; }

private:
    void teardown();
    bool receive_outputs(uint64_t generation);
    void set_error(const char* what, int err);

    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* source_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    ProcessorInfo info_;
    std::deque<OutputFrame> outputs_;
    std::string error_;
    bool drained_ = false;
};

} // namespace video
