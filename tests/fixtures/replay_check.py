#!/usr/bin/env python3
"""Replay the M0 fixtures through the SDL front-end and check diagnostics.

Runs the real application headless (SDL dummy video/audio drivers) over each
generated fixture, parses the `[+] video:`, `[+] presentation:` and `[stats]`
diagnostics, and verifies:

  * the active decoder/renderer and requested values are reported separately
  * input codec, size, declared field order and observed field order match the
    fixture manifest
  * decoded/output rates, queue durations, drops, A/V offset and UI present
    gaps stay within the M0 baseline bounds
  * replay looping, timestamp gaps and resolution changes keep playing
  * screenshots of the moving-bar fixtures show the expected field-parity
    combing (4 px on 1080i TFF, -2 px on 480i BFF, 0 on progressive) and no
    vertical tearing within a field

The baseline JSON/Markdown outputs are records for docs/PLAYBACK_BASELINE.md;
they are not compared between machines.

Usage:
    python3 tests/fixtures/replay_check.py --app build/Hibiki.app/Contents/MacOS/Hibiki \
        --dir build/test-fixtures [--quick] [--baseline-json FILE] [--baseline-md FILE]
"""

import argparse
import json
import os
import platform
import statistics
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))

QUICK_FIXTURES = [
    "progressive_720p5994",
    "interlaced_1080i5994_tff",
    "interlaced_480i5994_bff",
    "telecine_3_2_1080i",
    "mixed_film_video_1080i",
    "timestamp_gap_720p",
    "resolution_change",
    "replay_short_720p",
    "anamorphic_480p5994",
    "bars_full_720p_h264",
]
COMBING_CASES = {
    # fixture: (window width, window height, band row range, expected odd-even px)
    "interlaced_1080i5994_tff": (1920, 1080, (324, 756), 4.0),
    "interlaced_480i5994_bff": (720, 480, (144, 336), -2.0),
    "progressive_720p5994": (1280, 720, (216, 504), 0.0),
}


def run_app(app, fixture, extra_args=(), run_ms=4600, width=1280, height=720):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    argv = [app, "--ts-file", fixture, "--channel", "11", "--stats",
            "--width", str(width), "--height", str(height)]
    argv += list(extra_args)
    argv += ["--keys", f"+{run_ms},q"]
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=run_ms / 1000.0 + 15.0, env=env)
    except subprocess.TimeoutExpired as exc:
        return {"timeout": True,
                "stdout": (exc.stdout or b"").decode(errors="replace")
                          if isinstance(exc.stdout, bytes) else (exc.stdout or ""),
                "stderr": (exc.stderr or b"").decode(errors="replace")
                          if isinstance(exc.stderr, bytes) else (exc.stderr or "")}
    return {"timeout": False, "stdout": result.stdout, "stderr": result.stderr,
            "returncode": result.returncode}


def parse_kv(line, prefix):
    values = {}
    for token in line[len(prefix):].split():
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values


def parse_run(run):
    parsed = {"videos": [], "stats": [], "presentation": None,
              "media_lines": 0, "size_change_lines": 0}
    for line in run["stdout"].splitlines():
        if line.startswith("[+] video: "):
            parsed["videos"].append(parse_kv(line, "[+] video: "))
        elif line.startswith("[+] presentation: "):
            parsed["presentation"] = parse_kv(line, "[+] presentation: ")
        elif line.startswith("[stats] "):
            parsed["stats"].append(parse_kv(line, "[stats] "))
        elif line.startswith("[+] media: "):
            parsed["media_lines"] += 1
        elif line.startswith("[*] video size changed:"):
            parsed["size_change_lines"] += 1
    return parsed


def number(stats, key, default=0.0):
    try:
        return float(stats.get(key, default))
    except (TypeError, ValueError):
        return default


def check_fixture(fixture, parsed):
    errors = []
    name = fixture["name"]
    video = fixture["video"]

    if not parsed["videos"]:
        errors.append(f"{name}: no '[+] video:' diagnostics line")
        return errors
    reported = parsed["videos"][0]
    if reported.get("codec") != video["codec"]:
        errors.append(f"{name}: codec {reported.get('codec')} != {video['codec']}")
    if reported.get("field") != video["field_order"]:
        errors.append(f"{name}: declared field {reported.get('field')} != "
                      f"{video['field_order']}")
    if reported.get("pix_fmt") != video["pix_fmt"]:
        errors.append(f"{name}: pix_fmt {reported.get('pix_fmt')} != {video['pix_fmt']}")
    if reported.get("sar") != video["sar"]:
        errors.append(f"{name}: sar {reported.get('sar')} != {video['sar']}")
    if video.get("color_range") and reported.get("range") != video["color_range"]:
        errors.append(f"{name}: range {reported.get('range')} != "
                      f"{video['color_range']}")
    if video.get("color_space") and reported.get("space") != video["color_space"]:
        errors.append(f"{name}: colour space {reported.get('space')} != "
                      f"{video['color_space']}")
    if reported.get("decoder") != "auto>ffmpeg-cpu":
        errors.append(f"{name}: decoder {reported.get('decoder')} != auto>ffmpeg-cpu")
    if reported.get("processing") != "bypass":
        errors.append(f"{name}: processing {reported.get('processing')} != bypass")

    presentation = parsed["presentation"] or {}
    if not presentation.get("renderer", "").startswith("auto>"):
        errors.append(f"{name}: renderer not reported separately: {presentation}")
    if "vsync" not in presentation:
        errors.append(f"{name}: vsync result not reported")

    if len(parsed["stats"]) < 2:
        errors.append(f"{name}: only {len(parsed['stats'])} stats samples")
        return errors

    stats_list = parsed["stats"]
    shown = [int(number(s, "shown")) for s in stats_list]
    if max(shown) <= 0:
        errors.append(f"{name}: no frames were presented")

    # `--ts-file` loops the capture, which reopens the stream and restarts the
    # per-open counters/rate windows; samples taken right after a loop mark can
    # legitimately read zero, so rates are checked on the non-zero samples.
    coded_fps = fixture["video"]["avg_frame_rate"]
    num, _, den = coded_fps.partition("/")
    expected_fps = float(num) / float(den)
    out_fps = [number(s, "out_fps") for s in stats_list if number(s, "out_fps") > 0]
    dec_fps = [number(s, "dec_fps") for s in stats_list if number(s, "dec_fps") > 0]
    loop_seconds = len(fixture["video"]["pts_ticks"]) / expected_fps
    if not out_fps:
        if loop_seconds >= 0.8:
            errors.append(f"{name}: no output-rate sample survived a loop reset")
    elif not (0.4 * expected_fps <= statistics.median(out_fps) <= 1.4 * expected_fps):
        errors.append(f"{name}: output rate {out_fps} fps away from {expected_fps:.2f}")
    if dec_fps and not (0.4 * expected_fps <= statistics.median(dec_fps)):
        errors.append(f"{name}: decoded rate {dec_fps} fps too low")

    expected_field = fixture["video"]["field_order"]
    for sample in stats_list:
        if sample.get("field") != expected_field:
            errors.append(f"{name}: observed field {sample.get('field')} != "
                          f"{expected_field}")
            break
        if sample.get("proc") != "bypass":
            errors.append(f"{name}: processing {sample.get('proc')} != bypass")
        if abs(number(sample, "av")) > 0.5:
            errors.append(f"{name}: A/V offset {sample.get('av')} exceeds 0.5 s")
            break
        if number(sample, "q_video") > 0.75:
            errors.append(f"{name}: video queue {sample.get('q_video')} s exceeds 0.75 s")
            break
        if number(sample, "q_packets") > 2.0:
            errors.append(f"{name}: packet queue {sample.get('q_packets')} s exceeds 2.0 s")
            break
        dropped = number(sample, "dropped")
        if dropped > max(5.0, 0.10 * number(sample, "shown")):
            errors.append(f"{name}: dropped {dropped:g} of {sample.get('shown')} frames")
            break
        if number(sample, "present_gap_ms") > 500:
            errors.append(f"{name}: presentation gap {sample.get('present_gap_ms')} ms")

    if name == "resolution_change":
        if parsed["size_change_lines"] < 1:
            errors.append("resolution_change: no video size change reported")
        final = stats_list[-1]
        if final.get("size") != "1280x720":
            errors.append(f"resolution_change: final size {final.get('size')} "
                          "!= 1280x720")
        if int(number(final, "size_changes")) < 1:
            errors.append("resolution_change: size_changes counter stayed zero")
    if name == "replay_short_720p" and parsed["media_lines"] < 2:
        errors.append("replay_short_720p: capture did not loop "
                      f"({parsed['media_lines']} media opens)")
    return errors


def bmp_pixels(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    header_size, width, height, planes, bpp = struct.unpack_from("<IiiHH", data, 14)
    if bpp not in (24, 32):
        raise ValueError(f"unsupported BMP depth {bpp}")
    bytes_pp = bpp // 8
    stride = (width * bytes_pp + 3) // 4 * 4

    def row(y):
        source = offset + (height - 1 - y if height > 0 else y) * stride
        return data[source:source + width * bytes_pp]

    return width, abs(height), bytes_pp, row


def first_bright_x(pixels, width, bytes_pp, threshold=180):
    for x in range(width):
        base = x * bytes_pp
        blue, green, red = pixels[base], pixels[base + 1], pixels[base + 2]
        if 0.2126 * red + 0.7152 * green + 0.0722 * blue > threshold:
            return x
    return None


def analyze_combing(path, band):
    width, height, bytes_pp, row = bmp_pixels(path)
    positions = {}
    for y in range(band[0], min(band[1], height)):
        x = first_bright_x(row(y), width, bytes_pp)
        if x is not None:
            positions[y] = x
    even = [x for y, x in positions.items() if y % 2 == 0]
    odd = [x for y, x in positions.items() if y % 2 == 1]
    if not even or not odd:
        return None
    middle = (band[0] + band[1]) // 2

    def median_range(parity, low, high):
        values = [x for y, x in positions.items()
                  if y % 2 == parity and low <= y < high]
        return statistics.median(values) if values else None

    upper_even, lower_even = median_range(0, band[0], middle), median_range(0, middle, band[1])
    upper_odd, lower_odd = median_range(1, band[0], middle), median_range(1, middle, band[1])
    tear = False
    for upper, lower in ((upper_even, lower_even), (upper_odd, lower_odd)):
        if upper is not None and lower is not None and abs(upper - lower) > 3:
            tear = True
    return {
        "parity_offset_px": statistics.median(odd) - statistics.median(even),
        "tear_suspected": tear,
    }


def run_combing_check(app, fixture_dir, fixture_name, workdir):
    width, height, band, expected = COMBING_CASES[fixture_name]
    shot = os.path.join(workdir, fixture_name + ".bmp")
    path = os.path.join(fixture_dir, fixture_name + ".ts")
    run = run_app(app, path, extra_args=["--screenshot", shot, "--shot-after", "1800",
                                         "--no-hud"],
                  run_ms=3200, width=width, height=height)
    if run.get("timeout") or not os.path.exists(shot):
        return None, [f"{fixture_name}: combing screenshot not captured"]
    try:
        analysis = analyze_combing(shot, band)
    except ValueError as exc:
        return None, [f"{fixture_name}: {exc}"]
    if analysis is None:
        return None, [f"{fixture_name}: moving bar not found in screenshot"]
    errors = []
    offset = analysis["parity_offset_px"]
    if abs(offset - expected) > 2:
        errors.append(f"{fixture_name}: parity offset {offset:+.1f} px, "
                      f"expected {expected:+.1f} px")
    if analysis["tear_suspected"]:
        errors.append(f"{fixture_name}: vertical tearing found inside one field")
    analysis["expected_parity_offset_px"] = expected
    analysis["combing"] = abs(offset) >= 2
    return analysis, errors


def machine_info():
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
    }


def baseline_markdown(baseline):
    lines = ["| Fixture | out fps | dec fps | drops | max A/V (s) | video queue (s) "
             "| packet queue (s) | decode ms | upload ms | present gap (ms) |",
             "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for name, record in baseline["fixtures"].items():
        lines.append("| {name} | {out:.2f} | {dec:.2f} | {drops:g} | {av:.3f} | "
                     "{qv:.3f} | {qp:.3f} | {dms:.2f} | {ums:.2f} | {gap:g} |".format(
                         name=name, out=record["out_fps"], dec=record["dec_fps"],
                         drops=record["drops"], av=record["max_abs_av"],
                         qv=record["max_queue_video"], qp=record["max_queue_packets"],
                         dms=record["decode_ms"], ums=record["upload_ms"],
                         gap=record["max_present_gap_ms"]))
    combing = baseline.get("combing", {})
    if combing:
        lines += ["", "| Fixture | parity offset (px) | expected (px) | tear |",
                  "| --- | --- | --- | --- |"]
        for name, record in combing.items():
            lines.append("| {} | {:+.1f} | {:+.1f} | {} |".format(
                name, record["parity_offset_px"], record["expected_parity_offset_px"],
                "yes" if record["tear_suspected"] else "no"))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, help="Hibiki SDL front-end binary")
    parser.add_argument("--dir", default=os.path.join(ROOT, "generated"),
                        help="generated fixture directory")
    parser.add_argument("--quick", action="store_true",
                        help="replay the core subset only")
    parser.add_argument("--only", action="append", default=[],
                        help="replay only the named fixture (repeatable)")
    parser.add_argument("--baseline-json", help="write machine-readable baseline")
    parser.add_argument("--baseline-md", help="write the baseline as a Markdown table")
    options = parser.parse_args()

    if not os.path.exists(options.app):
        print(f"[replay] app not found: {options.app}", file=sys.stderr)
        return 77
    manifest_path = os.path.join(options.dir, "manifest.json")
    if not os.path.exists(manifest_path):
        print(f"[replay] {manifest_path} not found; run generate_fixtures.py",
              file=sys.stderr)
        return 77
    with open(manifest_path) as handle:
        manifest = json.load(handle)
    by_name = {fixture["name"]: fixture for fixture in manifest["fixtures"]}

    wanted = QUICK_FIXTURES if options.quick else list(by_name)
    if options.only:
        wanted = options.only
    fixtures = [by_name[name] for name in wanted if name in by_name]
    missing = [name for name in wanted if name not in by_name]
    if missing:
        print(f"[replay] missing fixtures: {', '.join(missing)}", file=sys.stderr)
        return 77

    errors = []
    baseline = {"machine": machine_info(), "app": os.path.basename(options.app),
                "fixtures": {}, "combing": {}}
    with tempfile.TemporaryDirectory() as workdir:
        for fixture in fixtures:
            path = os.path.join(options.dir, fixture["file"])
            if not os.path.exists(path):
                errors.append(f"{fixture['name']}: missing media file")
                continue
            run = run_app(options.app, path)
            parsed = parse_run(run)
            if run.get("timeout"):
                errors.append(f"{fixture['name']}: application did not quit")
            errors += check_fixture(fixture, parsed)
            if parsed["stats"]:
                samples = parsed["stats"]
                out_rates = [number(s, "out_fps") for s in samples
                             if number(s, "out_fps") > 0]
                dec_rates = [number(s, "dec_fps") for s in samples
                             if number(s, "dec_fps") > 0]
                baseline["fixtures"][fixture["name"]] = {
                    "out_fps": round(statistics.median(out_rates), 2) if out_rates else 0.0,
                    "dec_fps": round(statistics.median(dec_rates), 2) if dec_rates else 0.0,
                    "drops": int(max(number(s, "dropped") for s in samples)),
                    "max_abs_av": round(max(abs(number(s, "av")) for s in samples), 3),
                    "max_queue_video": round(max(number(s, "q_video") for s in samples), 3),
                    "max_queue_packets": round(max(number(s, "q_packets")
                                                   for s in samples), 3),
                    "decode_ms": round(statistics.median(number(s, "dec_ms")
                                                         for s in samples), 2),
                    "upload_ms": round(statistics.median(number(s, "up_ms")
                                                         for s in samples), 2),
                    "max_present_gap_ms": int(max(number(s, "present_gap_ms")
                                                  for s in samples)),
                    "resyncs": int(max(number(s, "resyncs") for s in samples)),
                    "renderer": parsed["presentation"].get("renderer")
                                if parsed["presentation"] else None,
                    "vsync": parsed["presentation"].get("vsync")
                             if parsed["presentation"] else None,
                }

        for name in COMBING_CASES:
            if name not in wanted:
                continue
            analysis, combing_errors = run_combing_check(options.app, options.dir,
                                                         name, workdir)
            errors += combing_errors
            if analysis:
                baseline["combing"][name] = analysis

        # Requested decoder is reported separately from the active one.
        probe_path = os.path.join(options.dir, by_name["progressive_720p5994"]["file"])
        forced = parse_run(run_app(options.app, probe_path,
                                   extra_args=["--decoder", "cpu"], run_ms=2600))
        forced_video = forced["videos"][0] if forced["videos"] else {}
        if forced_video.get("decoder") != "cpu>ffmpeg-cpu":
            errors.append(f"forced CPU decoder reported "
                          f"{forced_video.get('decoder')} instead of cpu>ffmpeg-cpu")
        novsync = parse_run(run_app(options.app, probe_path,
                                    extra_args=["--vsync", "off"], run_ms=1200))
        if (novsync["presentation"] or {}).get("vsync", "").split(">")[0] != "off":
            errors.append("--vsync off was not accepted")

    if options.baseline_json:
        with open(options.baseline_json, "w") as handle:
            json.dump(baseline, handle, indent=2)
            handle.write("\n")
    if options.baseline_md:
        with open(options.baseline_md, "w") as handle:
            handle.write(baseline_markdown(baseline))

    if errors:
        for message in errors:
            print(f"[replay] FAIL {message}", file=sys.stderr)
        print(f"[replay] {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"[replay] {len(baseline['fixtures'])} fixture(s) replayed with expected "
          f"diagnostics ({len(baseline['combing'])} combing checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
