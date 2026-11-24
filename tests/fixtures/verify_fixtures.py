#!/usr/bin/env python3
"""Verify the M0 synthetic fixtures against manifest.json.

Checks, per fixture:
  * SHA-256 against the manifest (a warning instead of a failure when the local
    FFmpeg version differs from the pinned one)
  * stream codec/profile/size/pixel format/field order/SAR/rate/color metadata
  * decoded frame count, exact PTS list and per-frame size (resolution change)
  * field-parity content: the moving bar's position on even and odd raster rows
    matches the ground truth in the manifest
  * colour bars: sampled Y/U/V values match the expected BT.709 values
  * decoded-frame MD5 reference under reference/<name>.framemd5

Usage:
    python3 tests/fixtures/verify_fixtures.py [--dir DIR] [--skip-checksums]

Missing ffmpeg/ffprobe or an ungenerated fixture directory exits 77 so CTest
reports the test as skipped rather than failed.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BAR_THRESHOLD = 200
POSITION_TOLERANCE = 3
COLOUR_TOLERANCE = 8


def tool(name, env):
    override = os.environ.get(env)
    if override:
        return override
    return shutil.which(name)


def run(argv):
    result = subprocess.run(argv, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError("command failed: " + " ".join(argv) + "\n" +
                           result.stderr.decode(errors="replace")[-2000:])
    return result


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def probe_manifest(path):
    result = run([FFPROBE, "-v", "error", "-show_streams", "-show_format",
                  "-show_frames", "-of", "json", path])
    return json.loads(result.stdout)


def video_stream(probe):
    return next(s for s in probe["streams"] if s["codec_type"] == "video")


def video_frames(probe):
    index = video_stream(probe)["index"]
    return [f for f in probe["frames"] if f.get("stream_index") == index]


def ratio(value):
    if not value:
        return None
    num, _, den = value.partition(":")
    try:
        return float(num) / float(den) if float(den) else None
    except ValueError:
        return None


def bar_edges(manifest):
    """Yield (expected_even_x, expected_odd_x) per coded frame."""
    for frame in manifest["content"]["frames"]:
        yield frame["even_x"], frame["odd_x"]


def decode_sample_rows(path, filter_graph, frames, width, seek_s=None):
    """Decode the given filtergraph at 2-row crops, return [(row0, row1)]."""
    argv = [FFMPEG, "-hide_banner", "-v", "error", "-threads", "1"]
    if seek_s is not None:
        argv += ["-ss", f"{seek_s:.6f}"]
    argv += ["-i", path, "-map", "0:v:0", "-vf", filter_graph,
             "-frames:v", str(frames), "-fps_mode", "passthrough",
             "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]
    result = run(argv)
    stride = width * 3  # 2 luma rows + one 4:2:0 chroma row (W/2 + W/2)
    rows = []
    for index in range(frames):
        chunk = result.stdout[index * stride:(index + 1) * stride]
        if len(chunk) != stride:
            break
        rows.append((chunk[0:width], chunk[width:2 * width]))
    return rows


def find_run_start(row):
    for index, value in enumerate(row):
        if value >= BAR_THRESHOLD:
            return index
    return None


def check_bar_content(path, manifest, errors):
    content = manifest["content"]
    frames = content["frames"]
    if content["kind"] == "resolution_change":
        # Crop filters cannot be configured for two different frame sizes in
        # one graph (and rawvideo/image2pipe outputs pin the first frame size),
        # so decode each segment separately after seeking to its first frame.
        pts = manifest["video"]["pts_ticks"]
        for segment in content["segments"]:
            start = segment["start_frame"]
            graph = f"crop={segment['width']}:2:0:{segment['sample_y']}"
            seek = pts[start] / 90000.0 - 0.0005 if start > 0 else None
            rows = decode_sample_rows(path, graph, segment["frames"],
                                      segment["width"], seek_s=seek)
            if len(rows) != segment["frames"]:
                errors.append(f"{manifest['name']}: decoded {len(rows)} rows for "
                              f"segment at frame {start}, expected "
                              f"{segment['frames']}")
                return
            for i, (even_row, odd_row) in enumerate(rows):
                expected = frames[start + i]
                check_frame_rows(manifest["name"], start + i, even_row, odd_row,
                                 expected["even_x"], expected["odd_x"], errors)
        return
    sample_y = content["sample_y"]
    width = manifest["video"]["width"]
    rows = decode_sample_rows(path, f"crop={width}:2:0:{sample_y}", len(frames), width)
    if len(rows) != len(frames):
        errors.append(f"decoded {len(rows)} rows, expected {len(frames)}")
        return
    for i, (even_row, odd_row) in enumerate(rows):
        expected_even, expected_odd = frames[i]["even_x"], frames[i]["odd_x"]
        check_frame_rows(manifest["name"], i, even_row, odd_row,
                         expected_even, expected_odd, errors)


def check_frame_rows(name, frame_index, even_row, odd_row, expected_even,
                     expected_odd, errors):
    found_even = find_run_start(even_row)
    found_odd = find_run_start(odd_row)
    if found_even is None or found_odd is None:
        errors.append(f"{name}: frame {frame_index}: bar not found "
                      f"(even={found_even}, odd={found_odd})")
        return
    if abs(found_even - expected_even) > POSITION_TOLERANCE:
        errors.append(f"{name}: frame {frame_index}: even-row bar at {found_even}, "
                      f"expected {expected_even}")
    if abs(found_odd - expected_odd) > POSITION_TOLERANCE:
        errors.append(f"{name}: frame {frame_index}: odd-row bar at {found_odd}, "
                      f"expected {expected_odd}")


def check_colour_bars(path, manifest, errors):
    content = manifest["content"]
    pix_fmt = manifest["video"]["pix_fmt"]
    result = run([FFMPEG, "-hide_banner", "-v", "error", "-threads", "1",
                  "-i", path, "-map", "0:v:0", "-frames:v", "1",
                  "-pix_fmt", pix_fmt, "-f", "rawvideo", "-"])
    width = manifest["video"]["width"]
    height = manifest["video"]["height"]
    frame = result.stdout
    if len(frame) < width * height + width * height // 2:
        errors.append(f"{manifest['name']}: short decoded frame ({len(frame)} bytes)")
        return
    u_plane = width * height
    v_plane = u_plane + (width // 2) * (height // 2)
    y = content["sample_y"]
    for column in content["columns"]:
        x = (column["x0"] + column["x1"]) // 2
        expected_y, expected_u, expected_v = column["yuv"]
        actual_y = frame[y * width + x]
        actual_u = frame[u_plane + (y // 2) * (width // 2) + x // 2]
        actual_v = frame[v_plane + (y // 2) * (width // 2) + x // 2]
        for label, expected, actual in (("Y", expected_y, actual_y),
                                        ("U", expected_u, actual_u),
                                        ("V", expected_v, actual_v)):
            if abs(int(actual) - int(expected)) > COLOUR_TOLERANCE:
                errors.append(f"{manifest['name']}: bar rgb={column['rgb']} {label}="
                              f"{actual}, expected {expected}")


def check_framemd5(path, reference, strict, errors, warnings):
    if not os.path.exists(reference):
        warnings.append(f"{os.path.basename(reference)}: reference missing")
        return
    result = run([FFMPEG, "-hide_banner", "-v", "error", "-threads", "1",
                  "-i", path, "-map", "0:v:0", "-f", "framemd5", "-"])
    with open(reference, "r", errors="replace") as handle:
        expected = [line.rstrip("\n") for line in handle
                    if not line.startswith("#")]
    actual = [line for line in result.stdout.decode(errors="replace").splitlines()
              if not line.startswith("#")]
    if actual == expected:
        return
    first = next((i for i, (a, e) in enumerate(zip(actual, expected)) if a != e),
                 min(len(actual), len(expected)))
    detail = (f"decoded-frame checksums differ at line {first + 1} "
              f"({len(actual)} vs {len(expected)} frames)")
    if strict:
        errors.append(detail)
    else:
        warnings.append(detail)


def check_fixture(path, manifest, reference_dir, strict_checksums, errors, warnings):
    name = manifest["name"]
    probe = probe_manifest(path)
    video = video_stream(probe)
    expected = manifest["video"]
    for key in ("profile", "pix_fmt", "field_order"):
        if video.get(key) != expected.get(key):
            errors.append(f"{name}: video {key}={video.get(key)}, "
                          f"expected {expected.get(key)}")
    if video.get("codec_name") != expected.get("codec"):
        errors.append(f"{name}: video codec={video.get('codec_name')}, "
                      f"expected {expected.get('codec')}")
    if int(video["width"]) != expected["width"] or int(video["height"]) != expected["height"]:
        errors.append(f"{name}: size {video['width']}x{video['height']}, "
                      f"expected {expected['width']}x{expected['height']}")
    if video.get("avg_frame_rate") != expected["avg_frame_rate"]:
        errors.append(f"{name}: rate {video.get('avg_frame_rate')}, "
                      f"expected {expected['avg_frame_rate']}")
    if ratio(video.get("sample_aspect_ratio") or "1:1") != ratio(expected["sar"]):
        errors.append(f"{name}: sar {video.get('sample_aspect_ratio')}, "
                      f"expected {expected['sar']}")
    if video.get("color_range") != expected.get("color_range"):
        errors.append(f"{name}: color_range {video.get('color_range')}, "
                      f"expected {expected.get('color_range')}")

    audio = next((s for s in probe["streams"] if s["codec_type"] == "audio"), None)
    if manifest["audio"] is None and audio is not None:
        errors.append(f"{name}: unexpected audio stream")
    if manifest["audio"] is not None:
        if audio is None:
            errors.append(f"{name}: audio stream missing")
        else:
            for key, expected_value in (("codec_name", manifest["audio"]["codec"]),
                                        ("sample_rate", str(manifest["audio"]["sample_rate"])),
                                        ("channels", str(manifest["audio"]["channels"]))):
                if str(audio.get(key)) != expected_value:
                    errors.append(f"{name}: audio {key}={audio.get(key)}, "
                                  f"expected {expected_value}")

    frames = video_frames(probe)
    pts = [int(frame["pts"]) for frame in frames]
    if pts != expected["pts_ticks"]:
        errors.append(f"{name}: decoded PTS list differs from manifest "
                      f"({len(pts)} frames)")
    if manifest["content"]["kind"] == "resolution_change":
        sizes = [(int(f["width"]), int(f["height"])) for f in frames]
        expected_sizes = [(f["w"], f["h"]) for f in manifest["content"]["frames"]]
        if sizes != expected_sizes:
            errors.append(f"{name}: per-frame sizes differ from manifest")
        if len(frames) != len(expected_sizes):
            errors.append(f"{name}: {len(frames)} frames, expected {len(expected_sizes)}")

    if manifest["content"]["kind"] in ("bar", "bar_misflagged", "resolution_change"):
        check_bar_content(path, manifest, errors)
    elif manifest["content"]["kind"] == "bars":
        check_colour_bars(path, manifest, errors)

    check_framemd5(path, os.path.join(reference_dir, name + ".framemd5"),
                   strict_checksums, errors, warnings)


def main():
    global FFMPEG, FFPROBE
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", default=os.path.join(ROOT, "generated"),
                        help="fixture directory (default: tests/fixtures/generated)")
    parser.add_argument("--manifest", default=None,
                        help="manifest to verify against (default: DIR/manifest.json, "
                             "else tests/fixtures/manifest.json)")
    parser.add_argument("--skip-checksums", action="store_true",
                        help="skip SHA-256 and framemd5 comparisons")
    options = parser.parse_args()

    FFMPEG = tool("ffmpeg", "HIBIKI_FFMPEG")
    FFPROBE = tool("ffprobe", "HIBIKI_FFPROBE")
    if not FFMPEG or not FFPROBE:
        print("[fixtures] ffmpeg/ffprobe not available; skipping", file=sys.stderr)
        return 77

    manifest_path = options.manifest or os.path.join(options.dir, "manifest.json")
    if not os.path.exists(manifest_path):
        manifest_path = os.path.join(ROOT, "manifest.json")
    if not os.path.exists(manifest_path):
        print("[fixtures] manifest.json not found; skipping", file=sys.stderr)
        return 77
    with open(manifest_path) as handle:
        manifest = json.load(handle)

    if not os.path.isdir(options.dir):
        print(f"[fixtures] {options.dir} not generated; skipping", file=sys.stderr)
        return 77

    local_version = subprocess.run([FFMPEG, "-version"], capture_output=True,
                                   text=True).stdout.splitlines()[0]
    version = next((token for token in local_version.split()
                    if token[:1].isdigit()), "unknown")
    pinned = manifest.get("ffmpeg_pinned") or manifest.get("ffmpeg_version")
    strict_checksums = (version == pinned) and not options.skip_checksums
    warnings = []
    if version != pinned:
        warnings.append(f"ffmpeg {version} != pinned {pinned}; "
                        "byte-level checks downgraded to warnings")

    reference_dir = os.path.join(options.dir, "reference")
    if not os.path.isdir(reference_dir):
        reference_dir = os.path.join(ROOT, "reference")

    errors = []
    for fixture in manifest["fixtures"]:
        path = os.path.join(options.dir, fixture["file"])
        if not os.path.exists(path):
            errors.append(f"{fixture['name']}: missing {fixture['file']}")
            continue
        if strict_checksums:
            digest = sha256(path)
            if digest != fixture["sha256"]:
                errors.append(f"{fixture['name']}: sha256 {digest}, "
                              f"expected {fixture['sha256']}")
        check_fixture(path, fixture, reference_dir, strict_checksums,
                      errors, warnings)

    # The checked-in manifest is the cross-machine contract; a regenerated
    # manifest may only differ by checksums (from a different FFmpeg build).
    committed_path = os.path.join(ROOT, "manifest.json")
    if (os.path.abspath(manifest_path) != os.path.abspath(committed_path)
            and os.path.exists(committed_path)):
        with open(committed_path) as handle:
            committed = json.load(handle)
        committed_by_name = {f["name"]: f for f in committed["fixtures"]}
        for fixture in manifest["fixtures"]:
            before = committed_by_name.get(fixture["name"])
            if before is None:
                errors.append(f"{fixture['name']}: not in checked-in manifest")
                continue
            for key in ("kind", "duration_frames"):
                if fixture.get(key) != before.get(key):
                    errors.append(f"{fixture['name']}: {key} differs from "
                                  "checked-in manifest")
            if fixture["video"]["pts_ticks"] != before["video"]["pts_ticks"]:
                errors.append(f"{fixture['name']}: PTS list differs from "
                              "checked-in manifest")
            if fixture["content"].get("frames") != before["content"].get("frames"):
                errors.append(f"{fixture['name']}: content ground truth differs "
                              "from checked-in manifest")
            if (not strict_checksums and fixture["sha256"] != before["sha256"]
                    and version != pinned):
                warnings.append(f"{fixture['name']}: file checksum differs from "
                                "checked-in manifest (different FFmpeg build)")

    for message in warnings:
        print(f"[fixtures] warning: {message}", file=sys.stderr)
    if errors:
        for message in errors:
            print(f"[fixtures] FAIL {message}", file=sys.stderr)
        print(f"[fixtures] {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"[fixtures] {len(manifest['fixtures'])} fixture(s) verified "
          f"({manifest_path})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
