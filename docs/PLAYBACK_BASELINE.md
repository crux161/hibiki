# Playback baseline (M0)

Recorded 2026-11-24 as part of milestone M0 of
[the ARM64 video pipeline roadmap](VIDEO_PIPELINE_ROADMAP.md). This names the
reference machines and dependency versions, and records baseline throughput,
drops, A/V offset and UI response so later milestones can be compared against a
known starting point. It is a baseline, not a release qualification.

## Reference machines and displays

| Role | Machine | Notes |
| --- | --- | --- |
| Reference Apple Silicon | Mac mini (Mac16,11), Apple M4 Pro, 12 cores (8 performance + 4 efficiency), 24 GB unified memory, macOS 27.0 (build 26A428) | Main display: 1920x1080 @ 60.00 Hz (System Information). 59.94 Hz content runs on a 60.00 Hz display, so under strict VSync a frame is repeated about every 16.7 s; confirm on-display pacing manually. |
| Minimum Linux ARM64 target (proposed) | Ubuntu 24.04 LTS AArch64, 4x Arm Cortex-A76 @ 2.4 GHz (Raspberry Pi 5 class) or better, 8 GB RAM, Mesa 24.x with KMS/DRM, SDL 3.4.x | Proposed for M4; to be confirmed on real hardware. Cross-compilation or emulation is not evidence of real-time performance. |

## Recorded dependencies

Recorded on the reference Apple Silicon machine, 2026-11-24:

| Component | Version / build |
| --- | --- |
| FFmpeg runtime | 9.1 (Homebrew arm64, VideoToolbox enabled) |
| libavcodec / libavformat / libavutil | 63.24.100 / 63.16.100 / 61.19.100 |
| libavfilter / libswscale / libswresample | 12.8.100 / 10.5.100 / 7.6.100 |
| SDL / SDL3_ttf | 3.4.20 / 3.2.2 |
| Compiler / build | Apple clang 21.0.0, CMake 4.5.1, Release |
| Fixture tooling | FFmpeg CLI 9.1 (pinned for reproducible fixtures), Python 3.15.2 |

The fixtures and their generation checksums assume FFmpeg CLI 9.1; see
`tests/fixtures/README.md`.

## Method

* Fixtures: the deterministic synthetic set in `tests/fixtures/` (see its
  README). Every fixture carries exact per-field ground truth in
  `manifest.json`.
* Headless baseline: `replay_check.py` runs the real front-end with
  `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, a 1280x720 window, the SDL
  **software** renderer, `--ts-file --stats`, and quits after 4.6 s per fixture.
  Stats are printed every 2 s plus a final sample at shutdown. `--ts-file`
  loops the capture, which reopens the stream and restarts per-open counters;
  samples taken right after a loop mark are zero and are excluded from rate
  medians. Fixtures shorter than the 0.5 s rate window report no rate (the
  `replay_short_720p` row), which is a measurement artefact, not a stall.
* Physical-presenter baseline: the same application on the built-in display
  with the SDL Metal renderer, 1280x720 window, 7 s per fixture.
* The headless numbers characterize decode/queue/present cost in the current
  pipeline. They are not a display-tearing measurement; follow the physical
  display checklist in `tests/fixtures/README.md` for that.

Reproduce the headless baseline:

```sh
python3 tests/fixtures/generate_fixtures.py --out build/test-fixtures
python3 tests/fixtures/replay_check.py \
    --app build/Hibiki.app/Contents/MacOS/Hibiki --dir build/test-fixtures \
    --baseline-json /tmp/baseline.json --baseline-md /tmp/baseline.md
```

The recorded JSON for the run below is
`tests/fixtures/reference/baseline_apple-m4pro-headless.json`.

## Baseline: headless, software renderer

Renderer reported `auto>software`, VSync `on>on`, decoder `auto>ffmpeg-cpu`,
processing `bypass`. Drops are late-frame drops within the last stats window;
queues are media seconds.

<!-- replay_check.py --baseline-md table, 2026-11-24 -->
| Fixture | out fps | dec fps | drops | max A/V (s) | video queue (s) | packet queue (s) | decode ms | upload ms | present gap (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| progressive_720p5994 | 55.81 | 58.80 | 9 | 0.007 | 0.083 | 1.235 | 0.41 | 3.09 | 77 |
| interlaced_1080i5994_tff | 29.62 | 28.63 | 0 | 0.015 | 0.167 | 1.068 | 0.86 | 5.93 | 96 |
| interlaced_480i5994_bff | 28.84 | 28.84 | 0 | 0.015 | 0.167 | 1.068 | 0.27 | 1.87 | 99 |
| telecine_3_2_1080i | 29.78 | 28.79 | 0 | 0.015 | 0.167 | 1.068 | 0.73 | 5.53 | 98 |
| mixed_film_video_1080i | 28.89 | 28.89 | 0 | 0.015 | 0.167 | 1.068 | 0.82 | 6.50 | 99 |
| flags_progressive_as_tff | 32.00 | 42.00 | 0 | 0.002 | 0.167 | 0.734 | 0.22 | 1.24 | 32 |
| flags_interlaced_as_progressive | 31.91 | 41.88 | 0 | 0.002 | 0.167 | 0.701 | 0.20 | 1.48 | 33 |
| anamorphic_480p5994 | 52.78 | 62.52 | 5 | 0.013 | 0.083 | 1.084 | 0.18 | 1.42 | 77 |
| bars_limited_720p | 54.44 | 69.28 | 4 | 0.005 | 0.083 | 0.767 | 0.39 | 2.90 | 78 |
| bars_full_720p_h264 | 53.56 | 68.18 | 4 | 0.014 | 0.083 | 0.684 | 0.93 | 4.19 | 80 |
| timestamp_gap_720p | 51.62 | 53.62 | 6 | 0.007 | 0.083 | 1.218 | 0.43 | 3.15 | 79 |
| resolution_change | 59.83 | 59.83 | 0 | 0.001 | 0.083 | 0.968 | 0.41 | 2.76 | 9 |
| replay_short_720p | 0.00 | 0.00 | 2 | 0.008 | 0.083 | 0.250 | 0.42 | 3.08 | 72 |

## Baseline: physical display, SDL Metal renderer

`[+] presentation: renderer=auto>metal vsync=on>on`; decoder
`auto>ffmpeg-cpu`; 1280x720 window, 1920x1080 @ 60.00 Hz display.

| Fixture | out fps | drops per 2 s | max abs A/V (s) | video queue (s) | packet queue (s) | decode ms | upload ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| progressive_720p5994 | 51.6–56.0 | 5–8 | 0.012 | 0.083 | 0.62 | 0.4–0.6 | 1.0–1.8 |
| interlaced_1080i5994_tff | 27.8–29.8 | 0 | 0.015 | 0.167 | 0.53 | 1.1–1.7 | 2.4–3.4 |

The physical run reproduces the headless behaviour: 1080i at the coded 29.97
rate with no drops, and 59.94 progressive losing roughly 5–10% of frames to the
current late-frame drop rule (a frame is dropped when the next one is already
due on the audio clock). Fixing that scheduling loss belongs to the M1 output
timeline.

## Field parity (combing) checks

Screenshots of the moving-bar fixtures, captured through the composed back
buffer at 1:1 window size. Combing is parity-structured (even vs odd raster
rows); a torn frame would show a block of rows where both parities jump.

| Fixture | measured even/odd offset | expected | within-field tear |
| --- | --- | --- | --- |
| interlaced_1080i5994_tff | +4 px | +4 px | none |
| interlaced_480i5994_bff | -2 px | -2 px | none |
| progressive_720p5994 | 0 px | 0 px | none |

## M0 exit criteria

* **Replay-based checks distinguish combing from presentation tearing.** The
  replay check proves the decoded/presented frames carry exactly the expected
  parity offset and no within-field discontinuity, so residual line-twitter on
  a physical display is combing until M2 filters land. Physical tearing must
  still be inspected by eye; the checklist is in `tests/fixtures/README.md`.
* **Baseline recorded.** Throughput, drops and A/V offset are in the tables
  above; queue durations, processing time and the maximum gap between presents
  (UI redraw responsiveness proxy, 8–100 ms while playing) are captured per
  fixture in the baseline JSON.
* **Diagnostics.** `--stats` now reports input codec/profile/format/field
  order, requested and active decoder and renderer, VSync request and result,
  decoded/output rates, queue durations, decode/upload time, size changes and
  presentation gaps. `I` shows the same state on screen.

Known limitations recorded here for later milestones: the packet queue is
bounded by bytes, not media duration (M1); 59.94 output on a 60 Hz display
loses frames to late-frame dropping (M1); hardware decode/presentation are not
present yet (M5/M6).
