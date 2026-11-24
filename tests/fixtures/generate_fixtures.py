#!/usr/bin/env python3
"""Generate the M0 synthetic video fixtures for the ARM64 video pipeline work.

Every fixture is built from synthetic per-field patterns with known motion and
field parity, encoded to MPEG-TS (ATSC transports). The generator also writes
`manifest.json` with the exact timestamp list, per-field bar positions and the
SHA-256 of each output, plus `reference/<name>.framemd5` decoded-frame
checksums. Keep the generated manifest and reference files in the repository:
they are the ground truth used by verify_fixtures.py, replay_check.py and the
later processing milestones.

Requires ffmpeg/ffprobe (override with HIBIKI_FFMPEG / HIBIKI_FFPROBE). The
checksums in the checked-in manifest were produced with the pinned FFmpeg
version below; another version still produces structurally identical fixtures
but will not reproduce the byte-level checksums.

Usage:
    python3 tests/fixtures/generate_fixtures.py [--out DIR] [--only NAME]...
                                           [--update-reference]

--update-reference also refreshes tests/fixtures/manifest.json and
tests/fixtures/reference/, which is a deliberate maintainer action.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction

ROOT = os.path.dirname(os.path.abspath(__file__))
PINNED_FFMPEG = "9.1"
TICK = 90000  # MPEG-TS clock
FPS_2997 = Fraction(30000, 1001)
FPS_5994 = Fraction(60000, 1001)
FILM_24000 = Fraction(24000, 1001)
BG_LUMA = 16
BAR_LUMA = 235
CHROMA_FLAT = 128


def ffmpeg_tool(name, env):
    override = os.environ.get(env)
    if override:
        return override
    found = shutil.which(name)
    if not found:
        sys.exit(f"error: {name} not found on PATH (set ${env})")
    return found


FFMPEG = None
FFPROBE = None


def run(argv, **kwargs):
    result = subprocess.run(argv, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.stderr.write("command failed: " + " ".join(argv) + "\n")
        sys.stderr.write(result.stderr[-4000:] + "\n")
        raise SystemExit(1)
    return result


def probe(path):
    result = run([FFPROBE, "-v", "error", "-show_streams", "-show_format",
                  "-show_frames", "-of", "json", path])
    return json.loads(result.stdout)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ---------------------------------------------------------------------------
# Synthetic picture builder.
#
# Each coded frame is assembled from two field pictures:
#   * even raster rows come from field picture "even"
#   * odd raster rows come from field picture "odd"
# For TFF the even (top) field is the earlier temporal sample, for BFF the odd
# (bottom) field is earlier. Field pictures carry:
#   * a full-height moving vertical bar inside band_rows
#   * parity marker blocks in the top-left corner (top field / bottom field)
#   * an 8-bit frame-index barcode near the bottom

def field_luma(w, h, bar_x, bar_w, band, marker, code):
    y0, y1 = band
    row = bytearray([BG_LUMA]) * w
    x0 = max(0, bar_x)
    x1 = min(w, bar_x + bar_w)
    if x1 > x0:
        row[x0:x1] = bytes([BAR_LUMA]) * (x1 - x0)
    band_row = bytes(row)
    frame = bytearray([BG_LUMA]) * (w * h)
    frame[y0 * w:y1 * w] = band_row * (y1 - y0)

    # Parity markers: block 1 white in the top field, block 2 white in the
    # bottom field. They stay outside the moving-bar band.
    marker_row = bytearray([BG_LUMA]) * w
    if marker == "top":
        marker_row[16:48] = bytes([BAR_LUMA]) * 32
    elif marker == "bottom":
        marker_row[56:88] = bytes([BAR_LUMA]) * 32
    marker_bytes = bytes(marker_row)
    for y in range(16, 48):
        frame[y * w:(y + 1) * w] = marker_bytes

    # Barcode: 8 data bits, white blocks for set bits.
    code_row = bytearray([BG_LUMA]) * w
    for bit in range(8):
        if code & (1 << bit):
            bx = 16 + 20 * bit
            code_row[bx:bx + 14] = bytes([BAR_LUMA]) * 14
    code_bytes = bytes(code_row)
    for y in range(h - 64, h - 32):
        frame[y * w:(y + 1) * w] = code_bytes
    return bytes(frame)


def weave(even, odd, w, h):
    out = bytearray(w * h)
    for y in range(0, h - 1, 2):
        out[y * w:(y + 1) * w] = even[y * w:(y + 1) * w]
        out[(y + 1) * w:(y + 2) * w] = odd[(y + 1) * w:(y + 2) * w]
    return bytes(out)


def yuv420_frame(w, h, luma):
    chroma = bytes([CHROMA_FLAT]) * (w // 2 * h // 2)
    return luma + chroma + chroma


def bars_planes(w, h, full_range):
    """SMPTE-style 75% bars, BT.709. Returns one YUV420 frame."""
    bars = [(191, 191, 191), (191, 191, 0), (0, 191, 191), (0, 191, 0),
            (191, 0, 191), (191, 0, 0), (0, 0, 191)]

    def to_yuv(r, g, b):
        rp, gp, bp = r / 255.0, g / 255.0, b / 255.0
        y = 0.2126 * rp + 0.7152 * gp + 0.0722 * bp
        cb = (bp - y) / 1.8556
        cr = (rp - y) / 1.5748
        if full_range:
            return (round(255 * y), round(128 + 255 * cb), round(128 + 255 * cr))
        return (round(16 + 219 * y), round(128 + 224 * cb), round(128 + 224 * cr))

    columns = []
    for index, rgb in enumerate(bars):
        columns.append({
            "x0": round(index * w / 7), "x1": round((index + 1) * w / 7),
            "rgb": list(rgb), "yuv": list(to_yuv(*rgb)),
        })

    luma_row = bytearray(w)
    chroma_row_u = bytearray(w // 2)
    chroma_row_v = bytearray(w // 2)
    for col in columns:
        luma_row[col["x0"]:col["x1"]] = bytes([col["yuv"][0]]) * (col["x1"] - col["x0"])
        cx0, cx1 = col["x0"] // 2, (col["x1"] + 1) // 2
        chroma_row_u[cx0:cx1] = bytes([col["yuv"][1]]) * (cx1 - cx0)
        chroma_row_v[cx0:cx1] = bytes([col["yuv"][2]]) * (cx1 - cx0)
    luma = bytes(luma_row) * h
    u = bytes(chroma_row_u) * (h // 2)
    v = bytes(chroma_row_v) * (h // 2)
    return luma + u + v, columns


# ---------------------------------------------------------------------------
# Frame plans. Each entry describes the two field pictures that form one coded
# frame: {"even": {"x", "mark", "code"}, "odd": {...}, "film": [a, b] | None}

def plan_progressive(count, step, first_x, first_index=0):
    frames = []
    for i in range(count):
        index = first_index + i
        x = first_x + step * index
        frames.append({
            "even": {"x": x, "mark": None, "code": index % 256},
            "odd": {"x": x, "mark": None, "code": index % 256},
            "film": None,
        })
    return frames


def plan_interlaced(count, field_step, first_x, order, first_field=0):
    """order 'tt' (TFF) or 'bb' (BFF). Field samples advance at 59.94 Hz."""
    frames = []
    for i in range(count):
        first = first_field + 2 * i       # earlier temporal sample
        second = first_field + 2 * i + 1  # later temporal sample
        x_first = first_x + field_step * first
        x_second = first_x + field_step * second
        even_x, odd_x = (x_first, x_second) if order == "tt" else (x_second, x_first)
        frames.append({
            "even": {"x": even_x, "mark": "top", "code": first % 256},
            "odd": {"x": odd_x, "mark": "bottom", "code": second % 256},
            "film": None,
        })
    return frames


# 3:2 cadence: film frames A B C D expand to coded frames A/A, A/B, B/C, C/C, D/D.
TELECINE_CADENCE = [(0, 0), (0, 1), (1, 2), (2, 2), (3, 3)]


def plan_telecine(count, film_step, first_x, first_film=0):
    frames = []
    for i in range(count):
        cycle, slot = divmod(i, 5)
        a, b = TELECINE_CADENCE[slot]
        film_a = first_film + cycle * 4 + a
        film_b = first_film + cycle * 4 + b
        frames.append({
            "even": {"x": first_x + film_step * film_a, "mark": "top", "code": film_a % 256},
            "odd": {"x": first_x + film_step * film_b, "mark": "bottom", "code": film_b % 256},
            "film": [film_a, film_b],
        })
    return frames


def plan_mixed(count, film_count, field_step, film_step, first_x):
    frames = plan_telecine(film_count, film_step, first_x)
    tail = plan_interlaced(count - film_count, field_step, first_x, "tt")
    return frames + tail


# ---------------------------------------------------------------------------
# Fixture specifications.

def build_specs():
    specs = []

    def spec(name, **kwargs):
        kwargs.setdefault("file", name + ".ts")
        kwargs.setdefault("codec", "mpeg2video")
        kwargs.setdefault("audio", True)
        kwargs.setdefault("color_space", "bt709")
        kwargs.setdefault("color_range", "tv")
        kwargs.setdefault("aspect", None)
        kwargs.setdefault("gap", None)
        kwargs.update(name=name)
        specs.append(kwargs)
        return kwargs

    w, h = 1280, 720
    plan = plan_progressive(120, 8, 32)
    spec("progressive_720p5994", kind="bar", w=w, h=h, rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="6M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=32, sample_y=h // 2)

    w, h = 1920, 1080
    plan = plan_interlaced(60, 4, 64, "tt")
    spec("interlaced_1080i5994_tff", kind="bar", w=w, h=h, rate=FPS_2997,
         field_order="tt", field_flags=True, gop=15, bitrate="8M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=32, sample_y=h // 2,
         field_step_px=4)

    w, h = 720, 480
    plan = plan_interlaced(60, 2, 32, "bb")
    spec("interlaced_480i5994_bff", kind="bar", w=w, h=h, rate=FPS_2997,
         field_order="bb", field_flags=True, gop=15, bitrate="3M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=20, sample_y=h // 2,
         field_step_px=2, display_aspect=Fraction(4, 3))

    w, h = 1920, 1080
    plan = plan_telecine(60, 16, 64)
    spec("telecine_3_2_1080i", kind="bar", w=w, h=h, rate=FPS_2997,
         field_order="tt", field_flags=True, gop=15, bitrate="8M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=32, sample_y=h // 2,
         film_step_px=16)

    w, h = 1920, 1080
    plan = plan_mixed(60, 30, 4, 16, 64)
    spec("mixed_film_video_1080i", kind="bar", w=w, h=h, rate=FPS_2997,
         field_order="tt", field_flags=True, gop=15, bitrate="8M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=32, sample_y=h // 2,
         film_step_px=16, field_step_px=4, cut_frame=30)

    w, h = 720, 480
    plan = plan_progressive(30, 8, 24)
    spec("flags_progressive_as_tff", kind="bar", w=w, h=h, rate=FPS_2997,
         field_order="tt", field_flags=True, gop=15, bitrate="3M", audio=False,
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=20, sample_y=h // 2,
         misflagged="progressive content flagged TFF interlaced")

    w, h = 720, 480
    plan = plan_interlaced(30, 2, 24, "tt")
    spec("flags_interlaced_as_progressive", kind="bar", w=w, h=h, rate=FPS_2997,
         field_order="progressive", field_flags=False, gop=15, bitrate="3M",
         audio=False, plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=20,
         sample_y=h // 2, field_step_px=2,
         misflagged="interlaced content flagged progressive")

    w, h = 720, 480
    plan = plan_progressive(90, 7, 24)
    spec("anamorphic_480p5994", kind="bar", w=w, h=h, rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="3M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=20, sample_y=h // 2,
         display_aspect=Fraction(16, 9))

    w, h = 1280, 720
    spec("bars_limited_720p", kind="bars", w=w, h=h, rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="4M",
         full_range=False, frames=60)
    spec("bars_full_720p_h264", kind="bars", w=w, h=h, rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="4M",
         codec="libx264", full_range=True, frames=60, color_range="pc")

    w, h = 1280, 720
    plan = plan_progressive(90, 8, 32)
    spec("timestamp_gap_720p", kind="bar", w=w, h=h, rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="6M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=32, sample_y=h // 2,
         gap={"at_frame": 30, "seconds": 0.5})

    spec("resolution_change", kind="resolution_change", rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="3M",
         audio=False, low_delay=True,
         segments=[
             {"w": 720, "h": 480, "frames": 75, "bar_w": 20, "sample_y": 240},
             {"w": 1280, "h": 720, "frames": 75, "bar_w": 32, "sample_y": 360},
         ])

    w, h = 1280, 720
    plan = plan_progressive(30, 8, 32)
    spec("replay_short_720p", kind="bar", w=w, h=h, rate=FPS_5994,
         field_order="progressive", field_flags=False, gop=30, bitrate="6M",
         plan=plan, band=(int(h * 0.3), int(h * 0.7)), bar_w=32, sample_y=h // 2)

    return specs


# ---------------------------------------------------------------------------
# Encoding.

def frame_argv(spec, w, h, audio_duration, output, output_args=None):
    rate = spec["rate"]
    color_options = ["-color_range", spec["color_range"]]
    if spec["color_space"]:
        color_options += ["-colorspace", spec["color_space"],
                          "-color_primaries", "bt709", "-color_trc", "bt709"]
    # Tag the synthetic raw frames on the input side too, otherwise FFmpeg
    # converts the samples from its default (BT.601) interpretation to the
    # requested output matrix, changing the bar colours we verify against.
    argv = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y"]
    argv += color_options
    argv += ["-f", "rawvideo", "-pix_fmt", "yuv420p",
             "-s", f"{w}x{h}", "-r", f"{rate.numerator}/{rate.denominator}", "-i", "-"]
    if spec["audio"]:
        argv += ["-f", "lavfi", "-i",
                 f"sine=frequency=440:sample_rate=48000:duration={audio_duration:.3f}"]
    filters = []
    if spec["field_flags"]:
        filters.append("setfield=" + ("tff" if spec["field_order"] == "tt" else "bff"))
    gap = spec.get("gap")
    if gap:
        frame = gap["at_frame"]
        # Pin the filter time base first so the gap is an exact tick count and
        # not truncated to the input frame time base.
        filters.append("settb=1/90000")
        filters.append(
            f"setpts=PTS+if(gte(N\\,{frame})\\,{gap['seconds']}*90000\\,0)")
    if filters:
        argv += ["-vf", ",".join(filters)]
    argv += ["-fps_mode", "passthrough"]

    if spec["codec"] == "libx264":
        argv += ["-c:v", "libx264", "-preset", "veryfast", "-crf", "16",
                 "-pix_fmt", "yuv420p", "-x264-params", "threads=1",
                 "-g", str(spec["gop"])]
    else:
        megabytes = int(spec["bitrate"].rstrip("M"))
        argv += ["-c:v", "mpeg2video", "-pix_fmt", "yuv420p",
                 "-b:v", spec["bitrate"], "-maxrate", spec["bitrate"],
                 "-bufsize", f"{megabytes * 2}M",
                 "-bf", "0", "-g", str(spec["gop"]), "-threads", "1"]
    codec_flags = "+bitexact"
    if spec["field_flags"]:
        codec_flags += "+ildct+ilme"
    if spec.get("low_delay"):
        codec_flags += "+low_delay"
    argv += ["-flags", codec_flags]
    argv += color_options
    if spec.get("display_aspect"):
        aspect = spec["display_aspect"]
        argv += ["-aspect", f"{aspect.numerator}:{aspect.denominator}"]
    if spec["audio"]:
        argv += ["-c:a", "ac3", "-b:a", "192k", "-ac", "2", "-ar", "48000"]
    argv += ["-fflags", "+bitexact", "-shortest", "-f", "mpegts",
             "-muxdelay", "0", "-muxpreload", "0"]
    if output_args:
        argv += output_args
    argv.append(output)
    return argv


def write_bar_stream(stream, spec, plan, w, h, band, bar_w):
    for entry in plan:
        even = field_luma(w, h, entry["even"]["x"], bar_w, band,
                          entry["even"]["mark"], entry["even"]["code"])
        odd = field_luma(w, h, entry["odd"]["x"], bar_w, band,
                         entry["odd"]["mark"], entry["odd"]["code"])
        stream.write(yuv420_frame(w, h, weave(even, odd, w, h)))


def encode_bar_fixture(spec, output):
    w, h = spec["w"], spec["h"]
    duration = len(spec["plan"]) / float(spec["rate"])
    audio_duration = duration + (spec["gap"]["seconds"] if spec.get("gap") else 0.0) + 0.5
    argv = frame_argv(spec, w, h, audio_duration, output)
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        write_bar_stream(proc.stdin, spec, spec["plan"], w, h, spec["band"], spec["bar_w"])
    finally:
        proc.stdin.close()
        stderr = proc.stderr.read().decode(errors="replace")
        rc = proc.wait()
    if rc != 0:
        sys.stderr.write("encode failed: " + " ".join(argv) + "\n" + stderr[-4000:])
        raise SystemExit(1)
    return [argv]


def encode_bars_fixture(spec, output):
    w, h = spec["w"], spec["h"]
    frame, columns = bars_planes(w, h, spec["full_range"])
    duration = spec["frames"] / float(spec["rate"])
    argv = frame_argv(spec, w, h, duration + 0.5, output)
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for _ in range(spec["frames"]):
            proc.stdin.write(frame)
    finally:
        proc.stdin.close()
        stderr = proc.stderr.read().decode(errors="replace")
        rc = proc.wait()
    if rc != 0:
        sys.stderr.write("encode failed: " + " ".join(argv) + "\n" + stderr[-4000:])
        raise SystemExit(1)
    return [argv], columns


def encode_resolution_change(spec, output, tmpdir):
    commands = []
    parts = []
    for index, segment in enumerate(spec["segments"]):
        part = os.path.join(tmpdir, f"segment{index}.ts")
        seg_spec = dict(spec)
        seg_spec.update(w=segment["w"], h=segment["h"], audio=False,
                        plan=plan_progressive(segment["frames"], 8, 32))
        output_args = None
        if index > 0:
            # Continue the timestamp timeline exactly one frame after segment 0.
            info = probe(parts[0])
            rate = spec["rate"]
            tick = Fraction(TICK) * rate.denominator / rate.numerator
            last = video_frames(info)[-1]["pts"]
            offset = (Fraction(last) + tick) / TICK
            output_args = ["-output_ts_offset", f"{float(offset):.9f}"]
        argv = frame_argv(seg_spec, segment["w"], segment["h"], 0.0, part,
                          output_args=output_args)
        commands.append(argv)
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            write_bar_stream(proc.stdin, seg_spec, seg_spec["plan"],
                             segment["w"], segment["h"],
                             (int(segment["h"] * 0.3), int(segment["h"] * 0.7)),
                             segment["bar_w"])
        finally:
            proc.stdin.close()
            stderr = proc.stderr.read().decode(errors="replace")
            rc = proc.wait()
        if rc != 0:
            sys.stderr.write("encode failed: " + " ".join(argv) + "\n" + stderr[-4000:])
            raise SystemExit(1)
        parts.append(part)
    with open(output, "wb") as out:
        for part in parts:
            with open(part, "rb") as handle:
                shutil.copyfileobj(handle, out)
    # The MPEG-2 decoder drops the last frame of the old resolution when the
    # next sequence header arrives, so count decoded frames per segment rather
    # than assuming every encoded frame survives the splice.
    frames = video_frames(probe(output))
    counts, cursor = [], 0
    for segment in spec["segments"]:
        count = 0
        while (cursor < len(frames)
               and (int(frames[cursor]["width"]), int(frames[cursor]["height"]))
               == (segment["w"], segment["h"])):
            count += 1
            cursor += 1
        counts.append(count)
    if cursor != len(frames):
        sys.stderr.write(f"{spec['name']}: undecodable size at frame {cursor}\n")
        raise SystemExit(1)
    return commands, counts


# ---------------------------------------------------------------------------
# Manifest assembly.

def stream_info(probe_data):
    video = next(s for s in probe_data["streams"] if s["codec_type"] == "video")
    audio = next((s for s in probe_data["streams"] if s["codec_type"] == "audio"), None)
    return video, audio


def video_frames(probe_data):
    video, _ = stream_info(probe_data)
    return [f for f in probe_data["frames"] if f.get("stream_index") == video["index"]]


def validate_fixture(spec, probe_data, decoded_counts=None):
    video, audio = stream_info(probe_data)
    problems = []
    if video["codec_name"] != ("h264" if spec["codec"] == "libx264" else "mpeg2video"):
        problems.append(f"codec {video['codec_name']}")
    if video["field_order"] != spec["field_order"]:
        problems.append(f"field_order {video['field_order']} != {spec['field_order']}")
    if spec["kind"] != "resolution_change":
        if int(video["width"]) != spec["w"] or int(video["height"]) != spec["h"]:
            problems.append(f"size {video['width']}x{video['height']}")
    expected_rate = f"{spec['rate'].numerator}/{spec['rate'].denominator}"
    if video["avg_frame_rate"] != expected_rate:
        problems.append(f"rate {video['avg_frame_rate']} != {expected_rate}")
    if spec["audio"] and not audio:
        problems.append("missing audio stream")
    frames = video_frames(probe_data)
    if spec["kind"] == "resolution_change":
        expected_frames = sum(decoded_counts or [s["frames"] for s in spec["segments"]])
    else:
        expected_frames = spec.get("frames", len(spec.get("plan", [])))
    if len(frames) != expected_frames:
        problems.append(f"{len(frames)} frames, expected {expected_frames}")
    pts = [int(frame["pts"]) for frame in frames]
    tick = Fraction(TICK) * spec["rate"].denominator / spec["rate"].numerator
    boundaries = set()
    if spec["kind"] == "resolution_change" and decoded_counts:
        total = 0
        for count in decoded_counts[:-1]:
            total += count
            boundaries.add(total)
    for i in range(1, len(pts)):
        delta = pts[i] - pts[i - 1]
        expected = float(tick) + (spec["gap"]["seconds"] * TICK
                                  if spec.get("gap") and
                                  i == spec["gap"]["at_frame"] else 0.0)
        # The muxer rounds PTS onto the 90 kHz clock and adds a small constant
        # encoder offset; the synthesized gap is therefore validated coarsely.
        tolerance = 100 if expected > float(tick) * 2 else 2
        if i in boundaries:   # decoder drops a frame at the resolution splice
            continue
        if abs(delta - expected) > tolerance:
            problems.append(f"pts delta {delta} at frame {i}, expected {expected:.1f}")
            break
    if problems:
        sys.stderr.write(f"error: {spec['name']}: " + "; ".join(problems) + "\n")
        raise SystemExit(1)
    return video, audio, pts


def frame_ground_truth(spec, pts, decoded_counts=None):
    entries = []
    if spec["kind"] == "resolution_change":
        counts = decoded_counts or [s["frames"] for s in spec["segments"]]
        index = 0
        for segment, count in zip(spec["segments"], counts):
            for i in range(count):
                x = 32 + 8 * i
                entries.append({"pts": pts[index], "even_x": x, "odd_x": x,
                                "w": segment["w"], "h": segment["h"], "film": None})
                index += 1
        return entries
    for entry, pts_ticks in zip(spec["plan"], pts):
        entries.append({"pts": pts_ticks, "even_x": entry["even"]["x"],
                        "odd_x": entry["odd"]["x"], "film": entry["film"]})
    return entries


def manifest_common(spec, video, audio, pts, decoded_counts=None):
    rate = spec["rate"]
    record = {
        "name": spec["name"],
        "file": spec["file"],
        "kind": spec["kind"],
        "duration_frames": len(pts),
        "video": {
            "codec": video["codec_name"],
            "profile": video.get("profile"),
            "width": int(video["width"]),
            "height": int(video["height"]),
            "pix_fmt": video["pix_fmt"],
            "field_order": video["field_order"],
            "sar": video.get("sample_aspect_ratio", "1:1"),
            "display_aspect_ratio": video.get("display_aspect_ratio"),
            "avg_frame_rate": video["avg_frame_rate"],
            "r_frame_rate": video.get("r_frame_rate"),
            "time_base": video["time_base"],
            "color_range": video.get("color_range"),
            "color_space": video.get("color_space"),
            "pts_ticks": pts,
            "pts_rule": {
                "clock_hz": TICK,
                "frame_duration_ticks": f"{TICK * rate.denominator}/{rate.numerator}",
                "gap_at_frame": spec["gap"]["at_frame"] if spec.get("gap") else None,
                "gap_seconds": spec["gap"]["seconds"] if spec.get("gap") else None,
            },
        },
        "audio": None if not audio else {
            "codec": audio["codec_name"],
            "sample_rate": int(audio["sample_rate"]),
            "channels": int(audio["channels"]),
        },
    }
    if spec["kind"] == "bar":
        record["content"] = {
            "kind": spec.get("misflagged") and "bar_misflagged" or "bar",
            "note": spec.get("misflagged"),
            "bar_width": spec["bar_w"],
            "band_rows": list(spec["band"]),
            "sample_y": spec["sample_y"],
            "interlaced_fields": spec["field_flags"],
            "field_order": spec["field_order"],
            "field_step_px": spec.get("field_step_px"),
            "film_step_px": spec.get("film_step_px"),
            "cut_frame": spec.get("cut_frame"),
            "frames": frame_ground_truth(spec, pts),
        }
    elif spec["kind"] == "resolution_change":
        counts = decoded_counts or [s["frames"] for s in spec["segments"]]
        record["content"] = {
            "kind": "resolution_change",
            "segments": [{"start_frame": sum(counts[:i]), "frames": count,
                          "width": s["w"], "height": s["h"],
                          "bar_width": s["bar_w"], "sample_y": s["sample_y"]}
                         for i, (s, count) in enumerate(zip(spec["segments"], counts))],
            "frames": frame_ground_truth(spec, pts, counts),
        }
    return record


def portable_command(command, target, tmpdir, filename):
    """Strip machine-specific paths from a recorded generation command."""
    shown = []
    for index, argument in enumerate(command):
        if index == 0:
            shown.append("ffmpeg")
        elif argument == target:
            shown.append(filename)
        elif tmpdir and os.path.dirname(argument) == tmpdir:
            shown.append(os.path.basename(argument))
        else:
            shown.append(argument)
    return shown


def process_fixture(spec, outdir, tmpdir):
    target = os.path.join(outdir, spec["file"])
    decoded_counts = None
    if spec["kind"] == "bars":
        commands, columns = encode_bars_fixture(spec, target)
    elif spec["kind"] == "resolution_change":
        commands, decoded_counts = encode_resolution_change(spec, target, tmpdir)
    else:
        commands = encode_bar_fixture(spec, target)
    probe_data = probe(target)
    video, audio, pts = validate_fixture(spec, probe_data, decoded_counts)
    record = manifest_common(spec, video, audio, pts, decoded_counts)
    record["bytes"] = os.path.getsize(target)
    record["sha256"] = sha256(target)
    # Record portable commands (the ffmpeg binary itself is resolved at run time).
    record["commands"] = [portable_command(command, target, tmpdir, spec["file"])
                          for command in commands]
    if spec["kind"] == "bars":
        record["content"] = {
            "kind": "bars",
            "range": "pc" if spec["full_range"] else "tv",
            "colorspace": "bt709",
            "columns": [{"x0": c["x0"], "x1": c["x1"], "rgb": c["rgb"], "yuv": c["yuv"]}
                        for c in columns],
            "sample_y": spec["h"] // 2,
        }
    # Decoded-frame reference checksums (video only, single-threaded decode).
    reference = os.path.join(outdir, "reference", spec["name"] + ".framemd5")
    os.makedirs(os.path.dirname(reference), exist_ok=True)
    result = subprocess.run(
        [FFMPEG, "-hide_banner", "-v", "error", "-y", "-threads", "1", "-i", target,
         "-map", "0:v:0", "-f", "framemd5", reference],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(f"framemd5 failed for {spec['name']}: {result.stderr}\n")
        raise SystemExit(1)
    record["reference_framemd5"] = os.path.relpath(reference, outdir)
    return record


def ffmpeg_version():
    result = subprocess.run([FFMPEG, "-version"], capture_output=True, text=True)
    first = result.stdout.splitlines()[0] if result.stdout else "unknown"
    for token in first.split():
        if token[:1].isdigit():
            return token
    return first


def main():
    global FFMPEG, FFPROBE
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(ROOT, "generated"),
                        help="output directory (default: tests/fixtures/generated)")
    parser.add_argument("--only", action="append", default=[],
                        help="generate only the named fixture (repeatable)")
    parser.add_argument("--update-reference", action="store_true",
                        help="also refresh manifest.json and reference/ in tests/fixtures/")
    options = parser.parse_args()

    FFMPEG = ffmpeg_tool("ffmpeg", "HIBIKI_FFMPEG")
    FFPROBE = ffmpeg_tool("ffprobe", "HIBIKI_FFPROBE")
    version = ffmpeg_version()
    if version != PINNED_FFMPEG:
        print(f"[fixtures] warning: ffmpeg {version} != pinned {PINNED_FFMPEG}; "
              "checksums will differ from the checked-in manifest", file=sys.stderr)

    specs = build_specs()
    if options.only:
        unknown = set(options.only) - {s["name"] for s in specs}
        if unknown:
            sys.exit("unknown fixture(s): " + ", ".join(sorted(unknown)))
        specs = [s for s in specs if s["name"] in options.only]

    os.makedirs(options.out, exist_ok=True)
    records = []
    with tempfile.TemporaryDirectory() as tmpdir:
        for spec in specs:
            print(f"[fixtures] {spec['name']}", file=sys.stderr)
            records.append(process_fixture(spec, options.out, tmpdir))

    manifest = {
        "format": 1,
        "description": "Synthetic playback fixtures for the ARM64 video pipeline roadmap (M0).",
        "generator": "tests/fixtures/generate_fixtures.py",
        "ffmpeg_version": version,
        "ffmpeg_pinned": PINNED_FFMPEG,
        "field_rate": "60000/1001",
        "film_rate": "24000/1001",
        "fixtures": records,
    }
    manifest_path = os.path.join(options.out, "manifest.json")
    with open(manifest_path, "w") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=False)
        handle.write("\n")
    print(f"[fixtures] wrote {manifest_path}", file=sys.stderr)

    if options.update_reference:
        with open(os.path.join(ROOT, "manifest.json"), "w") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=False)
            handle.write("\n")
        reference_dir = os.path.join(ROOT, "reference")
        os.makedirs(reference_dir, exist_ok=True)
        for record in records:
            shutil.copyfile(os.path.join(options.out, record["reference_framemd5"]),
                            os.path.join(reference_dir,
                                         os.path.basename(record["reference_framemd5"])))
        print("[fixtures] updated tests/fixtures/manifest.json and reference/", file=sys.stderr)


if __name__ == "__main__":
    main()
