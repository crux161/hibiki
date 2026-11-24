#!/usr/bin/env python3
"""Replay the M0 fixtures through the SDL front-end and check diagnostics.

Runs the real application headless (SDL dummy video/audio drivers) over each
generated fixture, parses the `[+] video:`, `[+] presentation:`, `[*]
processing:` and `[stats]` diagnostics, and verifies:

  * the active decoder/renderer and requested values are reported separately
  * input codec, size, declared field order and observed field order match the
    fixture manifest
  * decoded/output rates, processed-frame counts, queue durations, drops, A/V
    offset and UI present gaps stay within the baseline bounds
  * replay looping, timestamp gaps and resolution changes keep playing
  * the CPU processing mode matrix (M2): Auto/BWDIF/YADIF/Bob produce the
    expected 59.94 output cadence from 29.97 interlaced input, Weave keeps the
    coded rate and its paired-field combing, Auto preserves progressive timing,
    and force-processing doubles a progressive stream. Screenshots confirm the
    combing/parity structure per mode and that deinterlaced output has none.

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
import re
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

# Processing-mode matrix (M2): each case runs through the real SDL software
# renderer and checks cadence plus the screenshot's field structure.
MODE_CASES = [
    {"fixture": "interlaced_1080i5994_tff", "args": ["--deinterlace", "auto"],
     "mode": "auto", "window": (1920, 1080), "band": (324, 756), "parity": 0.0},
    {"fixture": "interlaced_1080i5994_tff", "args": ["--deinterlace", "bwdif"],
     "mode": "bwdif", "window": (1920, 1080), "band": (324, 756), "parity": 0.0},
    {"fixture": "interlaced_1080i5994_tff", "args": ["--deinterlace", "yadif"],
     "mode": "yadif", "window": (1920, 1080), "band": (324, 756), "parity": 0.0},
    {"fixture": "interlaced_1080i5994_tff", "args": ["--deinterlace", "bob"],
     "mode": "bob", "window": (1920, 1080), "band": (324, 756), "parity": 0.0},
    {"fixture": "interlaced_1080i5994_tff", "args": ["--deinterlace", "weave"],
     "mode": "weave", "window": (1920, 1080), "band": (324, 756), "parity": 4.0},
    {"fixture": "interlaced_1080i5994_tff",
     "args": ["--deinterlace", "passthrough"],
     "mode": "passthrough", "window": (1920, 1080), "band": (324, 756),
     "parity": 4.0},
    {"fixture": "interlaced_480i5994_bff", "args": ["--deinterlace", "bwdif"],
     "mode": "bwdif", "window": (720, 480), "band": (144, 336), "parity": 0.0},
    {"fixture": "interlaced_480i5994_bff", "args": ["--deinterlace", "weave"],
     "mode": "weave", "window": (720, 480), "band": (144, 336), "parity": -2.0},
    {"fixture": "progressive_720p5994", "args": ["--deinterlace", "auto"],
     "mode": "auto", "window": (1280, 720), "band": (216, 504), "parity": 0.0},
    {"fixture": "progressive_720p5994",
     "args": ["--deinterlace", "bwdif", "--deinterlace-force"],
     "mode": "bwdif", "force": True, "window": (1280, 720),
     "band": (216, 504), "parity": 0.0},
]


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
              "media_lines": 0, "size_change_lines": 0,
              "processing_lines": []}
    for line in run["stdout"].splitlines():
        if line.startswith("[+] video: "):
            parsed["videos"].append(parse_kv(line, "[+] video: "))
        elif line.startswith("[+] presentation: "):
            parsed["presentation"] = parse_kv(line, "[+] presentation: ")
        elif line.startswith("[stats] "):
            parsed["stats"].append(parse_kv(line, "[stats] "))
        elif line.startswith("[*] processing: "):
            parsed["processing_lines"].append(line)
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


SUPPORTED_PIX_FMTS = {"yuv420p", "yuv422p", "nv12"}


def effective_mode(fixture, mode):
    """The mode the worker can actually run for this fixture's pixel format."""
    if mode in ("auto", "bwdif", "yadif", "bob") and \
            fixture["video"]["pix_fmt"] not in SUPPORTED_PIX_FMTS:
        return "passthrough"
    return mode


def output_multiplier(mode, fixture, force=False):
    """Expected output frames per decoded frame for a processing mode."""
    field = fixture["video"]["field_order"]
    if mode == "bob":
        return 2
    if mode in ("auto", "bwdif", "yadif"):
        if force or field in ("tt", "bb"):
            return 2
    return 1


def processing_time_base(parsed):
    if not parsed["processing_lines"]:
        return None
    match = re.search(r"sink time base (\d+)/(\d+)",
                      parsed["processing_lines"][-1])
    if not match:
        return None
    return int(match.group(1)), int(match.group(2))


def check_fixture(fixture, parsed, mode="auto", force=False):
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
    if reported.get("processing") != mode:
        errors.append(f"{name}: requested processing {reported.get('processing')} "
                      f"!= {mode}")
    active_mode = effective_mode(fixture, mode)
    if not parsed["processing_lines"]:
        errors.append(f"{name}: worker never reported its processing graph")
    elif f"processing: {active_mode}" not in parsed["processing_lines"][-1]:
        errors.append(f"{name}: worker processing line does not report "
                      f"{active_mode}: {parsed['processing_lines'][-1]}")
    if processing_time_base(parsed) is None:
        errors.append(f"{name}: sink time base not reported")

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
    processed = [int(number(s, "processed")) for s in stats_list]
    if max(processed) <= 0:
        errors.append(f"{name}: processing boundary produced no output")
    multiplier = output_multiplier(active_mode, fixture, force)
    if multiplier == 2:
        if not any(int(number(s, "processed")) > int(number(s, "decoded")) + 5
                   for s in stats_list):
            errors.append(f"{name}: field mode did not emit more outputs than "
                          "decoded frames")

    # `--ts-file` loops the capture, which reopens the stream and restarts the
    # per-open counters/rate windows; samples taken right after a loop mark can
    # legitimately read zero, so rates are checked on the non-zero samples.
    coded_fps = fixture["video"]["avg_frame_rate"]
    num, _, den = coded_fps.partition("/")
    coded_rate = float(num) / float(den)
    target_rate = coded_rate * multiplier
    out_fps = [number(s, "out_fps") for s in stats_list if number(s, "out_fps") > 0]
    dec_fps = [number(s, "dec_fps") for s in stats_list if number(s, "dec_fps") > 0]
    loop_seconds = len(fixture["video"]["pts_ticks"]) / coded_rate
    if not out_fps:
        if loop_seconds >= 0.8:
            errors.append(f"{name}: no output-rate sample survived a loop reset")
    elif not (0.4 * target_rate <= statistics.median(out_fps) <= 1.4 * target_rate):
        errors.append(f"{name}: output rate {out_fps} fps away from "
                      f"{target_rate:.2f} ({active_mode})")
    if dec_fps and not (0.4 * coded_rate <= statistics.median(dec_fps)):
        errors.append(f"{name}: decoded rate {dec_fps} fps too low")

    expected_field = fixture["video"]["field_order"]
    for sample in stats_list:
        if sample.get("field") != expected_field:
            errors.append(f"{name}: observed field {sample.get('field')} != "
                          f"{expected_field}")
            break
        if sample.get("proc") != active_mode:
            errors.append(f"{name}: active processing {sample.get('proc')} != "
                          f"{active_mode}")
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
        presented = number(sample, "shown") + dropped
        # Field-rate modes double the presentation load; the dummy software
        # backend is allowed to shed pictures there (the mode test targets
        # cadence and field structure, not the M4 performance gate).
        drop_fraction = 0.60 if multiplier == 2 else 0.12
        if dropped > max(5.0, drop_fraction * presented):
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


def run_mode_case(app, fixture_dir, case, manifest, workdir):
    fixture = manifest["fixtures_by_name"][case["fixture"]]
    name = fixture["name"]
    tag = case["mode"] + ("-force" if case.get("force") else "")
    shot = os.path.join(workdir, f"{name}-{tag}.bmp")
    path = os.path.join(fixture_dir, fixture["file"])
    width, height = case["window"]
    run = run_app(app, path,
                  extra_args=list(case["args"]) +
                             ["--screenshot", shot, "--shot-after", "1800", "--no-hud"],
                  run_ms=4600, width=width, height=height)
    errors = []
    if run.get("timeout"):
        errors.append(f"{name}/{tag}: application did not quit")
    parsed = parse_run(run)
    errors += check_fixture(fixture, parsed, mode=case["mode"],
                            force=case.get("force", False))
    analysis = None
    if not os.path.exists(shot):
        errors.append(f"{name}/{tag}: screenshot not captured")
    else:
        try:
            analysis = analyze_combing(shot, case["band"])
        except ValueError as exc:
            errors.append(f"{name}/{tag}: {exc}")
        if analysis is None:
            errors.append(f"{name}/{tag}: moving bar not found in screenshot")
    if analysis:
        offset = analysis["parity_offset_px"]
        if abs(offset - case["parity"]) > 2:
            errors.append(f"{name}/{tag}: parity offset {offset:+.1f} px, "
                          f"expected {case['parity']:+.1f} px")
        if analysis["tear_suspected"]:
            errors.append(f"{name}/{tag}: vertical tearing inside one field")
        analysis["expected_parity_offset_px"] = case["parity"]
        analysis["combing"] = abs(offset) >= 2
        out_rates = [number(s, "out_fps") for s in parsed["stats"]
                     if number(s, "out_fps") > 0]
        analysis["out_fps"] = round(statistics.median(out_rates), 2) if out_rates else 0.0
        tb = processing_time_base(parsed)
        analysis["sink_time_base"] = f"{tb[0]}/{tb[1]}" if tb else None
        analysis["mode"] = case["mode"]
        analysis["force"] = case.get("force", False)
    return errors, analysis


def machine_info():
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
    }


def baseline_markdown(baseline):
    lines = ["| Fixture | out fps | dec fps | drops | max A/V (s) | video queue (s) "
             "| packet queue (s) | decode ms | process ms | upload ms | present gap (ms) |",
             "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for name, record in baseline["fixtures"].items():
        lines.append("| {name} | {out:.2f} | {dec:.2f} | {drops:g} | {av:.3f} | "
                     "{qv:.3f} | {qp:.3f} | {dms:.2f} | {pms:.2f} | {ums:.2f} | {gap:g} |".format(
                         name=name, out=record["out_fps"], dec=record["dec_fps"],
                         drops=record["drops"], av=record["max_abs_av"],
                         qv=record["max_queue_video"], qp=record["max_queue_packets"],
                         dms=record["decode_ms"], pms=record["process_ms"],
                         ums=record["upload_ms"],
                         gap=record["max_present_gap_ms"]))
    modes = baseline.get("modes", {})
    if modes:
        lines += ["", "| Fixture | Mode | out fps | parity offset (px) | expected | "
                      "sink time base | tear |",
                  "| --- | --- | --- | --- | --- | --- | --- |"]
        for key, record in modes.items():
            lines.append("| {} | {}{} | {:.2f} | {:+.1f} | {:+.1f} | {} | {} |".format(
                record["fixture"], record["mode"],
                " (force)" if record.get("force") else "",
                record["out_fps"], record["parity_offset_px"],
                record["expected_parity_offset_px"],
                record.get("sink_time_base") or "-",
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
    manifest["fixtures_by_name"] = by_name

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
                "fixtures": {}, "modes": {}}
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
                    "process_ms": round(statistics.median(number(s, "proc_ms")
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

        if not options.only:
            for case in MODE_CASES:
                case_errors, analysis = run_mode_case(options.app, options.dir,
                                                      case, manifest, workdir)
                errors += case_errors
                if analysis:
                    key = case["fixture"] + "/" + analysis["mode"] + \
                          ("-force" if analysis["force"] else "")
                    analysis["fixture"] = case["fixture"]
                    baseline["modes"][key] = analysis

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
          f"diagnostics, {len(baseline['modes'])} processing-mode checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
