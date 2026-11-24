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

## M1 pass-through baseline (worker + processing boundary)

Recorded 2026-11-24 on the reference machine above after introducing
`VideoProcessor` and moving video decode/filtering to a bounded worker thread.
The processing graph is pass-through (`processing=passthrough`); decoding,
filtering and texture upload are now measured separately (`dec_ms`,
`proc_ms`, `up_ms`), and `processed` counts outputs leaving the boundary.

Method: same headless replay run as M0 (`replay_check.py`, software renderer,
4.6 s per fixture). The recorded JSON is
`tests/fixtures/reference/baseline_m1_apple-m4pro-headless.json`.

| Fixture | out fps | dec fps | drops | max A/V (s) | video queue (s) | packet queue (s) | decode ms | process ms | upload ms | present gap (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| progressive_720p5994 | 54.59 | 57.57 | 9 | 0.016 | 0.100 | 1.168 | 0.28 | 0.00 | 2.01 | 61 |
| interlaced_1080i5994_tff | 28.76 | 28.76 | 0 | 0.006 | 0.234 | 1.024 | 0.66 | 0.01 | 3.72 | 99 |
| interlaced_480i5994_bff | 28.84 | 28.84 | 0 | 0.015 | 0.200 | 1.024 | 0.17 | 0.00 | 0.84 | 97 |
| telecine_3_2_1080i | 29.56 | 29.56 | 0 | 0.011 | 0.234 | 1.024 | 0.96 | 0.01 | 5.64 | 100 |
| mixed_film_video_1080i | 29.48 | 29.48 | 0 | 0.011 | 0.234 | 1.024 | 1.00 | 0.01 | 5.98 | 99 |
| flags_progressive_as_tff | 31.89 | 29.90 | 0 | 0.001 | 0.200 | 0.000 | 0.25 | 0.01 | 0.68 | 34 |
| flags_interlaced_as_progressive | 31.90 | 29.90 | 0 | 0.002 | 0.200 | 0.000 | 0.24 | 0.01 | 0.66 | 35 |
| anamorphic_480p5994 | 52.75 | 56.73 | 5 | 0.011 | 0.117 | 1.034 | 0.18 | 0.00 | 1.37 | 86 |
| bars_limited_720p | 53.65 | 55.64 | 4 | 0.014 | 0.117 | 0.684 | 0.36 | 0.00 | 2.54 | 62 |
| bars_full_720p_h264 | 53.56 | 57.52 | 4 | 0.010 | 0.117 | 0.684 | 1.11 | 0.00 | 4.55 | 60 |
| timestamp_gap_720p | 50.87 | 53.86 | 6 | 0.011 | 0.117 | 1.185 | 0.41 | 0.00 | 3.32 | 63 |
| resolution_change | 59.97 | 59.97 | 0 | 0.002 | 0.117 | 0.901 | 0.43 | 0.00 | 3.17 | 38 |
| replay_short_720p | 0.00 | 0.00 | 1 | 0.013 | 0.117 | 0.200 | 0.33 | 0.00 | 2.56 | 78 |

Field-parity results are unchanged from M0 (+4 px 1080i TFF, -2 px 480i BFF,
0 px progressive, no within-field tearing).

M1 exit criteria:

* **Pass-through preserves playback.** All fixtures replay with the expected
  rates, bounded queues and the same field-parity results as M0. Decode is
  0.2–1.0 ms/picture and the pass-through boundary costs ~0.01 ms; 1080i
  software texture upload remains the largest per-frame cost (3.7–6 ms).
* **Synthetic tests** (`video_processor_tests`, `player_worker_tests`) cover
  multiple outputs and a changed sink time base (`fps=60`: 60 inputs → 120
  processed/presented pictures, sink time base 1/60), unknown timestamps
  preserved through the boundary and presented immediately, EOF drain
  (decoder then filter graph, `tpad` flush and real TS), cancellation
  (`close()` while the worker is blocked on a full output queue completes
  immediately) and clean resets (processor reset/reconfigure, a mid-stream
  processing-method change, a 6 s timestamp jump, and the `resolution_change`
  fixture's format change — each bumps the stream generation and resets
  temporal history).
* **Menus stay responsive.** Decoding and filtering run on the worker; the
  headless replay run records present gaps of 34–100 ms while playing.

Known follow-ups recorded for M2/M4: the 59.94 late-frame drop policy still
loses ~5–10% of pictures (unchanged from M0, now applied to processed output
after a generation check); packet queues are bounded by bytes, packet count
and media duration, while the processed queue is bounded to 8 pictures /
32 MiB / 0.6 s.

## M2 CPU processing modes

Recorded 2026-11-24 after adding Auto/BWDIF/YADIF/Bob/Weave/pass-through.
Default product mode is **Auto**: BWDIF (`mode=send_field:parity=auto:
deint=interlaced`) bypasses progressive frames and emits one picture per
field for frames flagged interlaced. BWDIF and YADIF use the roadmap presets;
`--deinterlace-force` switches them to `deint=all`. Bob reconstructs each
field with `separatefields,scale=w=iw:h=2*ih:flags=neighbor` (top field first
according to the frame flags or `--field-order`, nearest-neighbour duplication
of luma and chroma, border lines duplicated). Weave is a pass-through for
full-frame decoder output at the coded frame rate. 8-bit planar 4:2:0/4:2:2
are filtered directly, NV12 is converted with an explicit `format=yuv420p`,
and other formats (for example the full-range `yuvj420p` H.264 bars fixture)
fall back to pass-through with a one-time warning.

Method: the same headless replay run as M0/M1 (software renderer), plus a
mode matrix that captures a screenshot during a 4.6 s replay of each mode and
measures the even/odd bar offset. Recorded JSON:
`tests/fixtures/reference/baseline_m2_apple-m4pro-headless.json`.

### Default Auto runs (software renderer)

| Fixture | out fps | dec fps | drops | max A/V (s) | video queue (s) | packet queue (s) | decode ms | process ms | upload ms | present gap (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| progressive_720p5994 | 53.52 | 55.50 | 8 | 0.012 | 0.117 | 1.151 | 0.45 | 0.01 | 2.78 | 63 |
| interlaced_1080i5994_tff | 53.48 | 27.73 | 8 | 0.011 | 0.117 | 1.068 | 0.71 | 1.31 | 4.18 | 63 |
| interlaced_480i5994_bff | 54.38 | 27.69 | 8 | 0.012 | 0.117 | 1.068 | 0.26 | 0.94 | 1.57 | 63 |
| telecine_3_2_1080i | 53.51 | 27.75 | 8 | 0.011 | 0.117 | 1.068 | 0.78 | 1.59 | 4.28 | 63 |
| mixed_film_video_1080i | 54.38 | 27.68 | 8 | 0.011 | 0.117 | 1.068 | 0.69 | 1.32 | 4.01 | 63 |
| flags_progressive_as_tff | 61.88 | 29.94 | 0 | 0.001 | 0.100 | 0.267 | 0.24 | 1.01 | 0.85 | 6 |
| flags_interlaced_as_progressive | 31.95 | 29.95 | 0 | 0.002 | 0.200 | 0.000 | 0.25 | 0.02 | 0.66 | 36 |
| anamorphic_480p5994 | 50.88 | 54.87 | 5 | 0.009 | 0.117 | 1.101 | 0.24 | 0.01 | 1.27 | 88 |
| bars_limited_720p | 51.65 | 53.64 | 3 | 0.009 | 0.100 | 0.784 | 0.48 | 0.01 | 3.13 | 66 |
| bars_full_720p_h264 | 52.35 | 56.30 | 3 | 0.009 | 0.117 | 0.784 | 0.86 | 0.00 | 3.66 | 64 |
| timestamp_gap_720p | 44.55 | 46.54 | 6 | 0.013 | 0.100 | 1.151 | 0.58 | 0.01 | 3.71 | 64 |
| resolution_change | 59.75 | 60.00 | 0 | 0.002 | 0.117 | 0.868 | 0.48 | 0.01 | 3.07 | 38 |
| replay_short_720p | 0.00 | 0.00 | 1 | 0.010 | 0.117 | 0.250 | 0.35 | 0.01 | 1.92 | 84 |

`flags_progressive_as_tff` is flagged interlaced, so Auto deinterlaces it
(59.94 output from progressive content); `flags_interlaced_as_progressive` is
flagged progressive, so Auto trusts the flags and keeps the coded 29.97 rate
with combing still present until a force/field-order override is used.

### Processing-mode matrix

| Fixture | Mode | out fps | parity offset (px) | expected | sink time base | tear |
| --- | --- | --- | --- | --- | --- | --- |
| interlaced_1080i5994_tff | auto | 53.09 | +0.0 | +0.0 | 1/180000 | no |
| interlaced_1080i5994_tff | bwdif | 53.51 | +0.0 | +0.0 | 1/180000 | no |
| interlaced_1080i5994_tff | yadif | 54.52 | +0.0 | +0.0 | 1/180000 | no |
| interlaced_1080i5994_tff | bob | 52.83 | +0.0 | +0.0 | 1/180000 | no |
| interlaced_1080i5994_tff | weave | 27.76 | +4.0 | +4.0 | 1/90000 | no |
| interlaced_1080i5994_tff | passthrough | 28.84 | +4.0 | +4.0 | 1/90000 | no |
| interlaced_480i5994_bff | bwdif | 53.30 | +0.0 | +0.0 | 1/180000 | no |
| interlaced_480i5994_bff | weave | 28.69 | -2.0 | -2.0 | 1/90000 | no |
| progressive_720p5994 | auto | 53.70 | +0.0 | +0.0 | 1/180000 | no |
| progressive_720p5994 | bwdif (force) | 60.08 | +0.0 | +0.0 | 1/180000 | no |

M2 exit criteria:

* **All modes work through the SDL software renderer.** Every mode above ran
  through the real front-end with the software renderer; screenshots show the
  expected field structure (deinterlaced modes have no parity combing, Weave
  and pass-through keep the paired-field offset).
* **Cadence.** BWDIF, YADIF, Bob and Auto produce the 59.94 field-rate output
  from the 29.97 interlaced fixtures (measured 53–55 fps on the dummy
  software backend after late-frame drops); Auto preserves the 59.94
  progressive timing (53.5 fps measured, same as pass-through within noise).
* **Reference comparisons.** `video_processor_tests` checks frame counts,
  timestamps and sink time bases, top/bottom field order per output, Bob's
  line duplication and border handling, chroma preservation, 4:2:0/4:2:2
  filtering, NV12 conversion, explicit rejection of other formats, field-order
  overrides and mode changes. The replay matrix covers field order, chroma,
  boundaries, frame counts and timestamps end to end.
* **Negotiation.** 8-bit planar 4:2:0 and 4:2:2 are the supported filtered
  formats; NV12 is converted to 4:2:0; other formats fall back to pass-through
  with a one-time warning (the full-range H.264 bars fixture exercises this).

A physical-presenter spot check with the Metal renderer on the reference
display (`--deinterlace auto`, 1080i TFF) sustained 55.5–57.8 output fps with
1.7–1.8 ms decode and 4.4–4.9 ms BWDIF per picture, A/V offset below 6 ms and
presentation gaps of 116–143 ms; the same run under the dummy software backend
reported ~1.3 ms BWDIF, so the filter cost depends on CPU scheduling
alongside presentation.

Performance notes for later milestones: BWDIF costs ~1.3 ms/picture and YADIF
~3 ms/picture on 1080i in this headless run; Bob ~0.7 ms. The forced
progressive-to-field-rate case produces 119.88 field pictures but the dummy
software presenter only sustains ~60 fps with heavy late-frame drops, so that
mode is a correctness/diagnostic path until M4 measures real hardware.

## M3 fieldmatch + BWDIF film mode

Recorded 2026-11-24 after adding the explicit film mode
(`--deinterlace fieldmatch-bwdif`, alias `film`). The graph is
`fieldmatch=order=auto:combmatch=full,bwdif=mode=send_frame:parity=auto:
deint=interlaced` (field-order override maps `order`/`parity`; force selects
BWDIF `deint=all`). It keeps the nominal input cadence: field matching
reconstructs frames from paired fields, one duplicate per cadence group is
retained, and no inverse-telecine decimation is applied. Ordinary Auto stays
on the M2 BWDIF policy.

Method: the same headless replay run and mode matrix as M2, with three film
cases. Recorded JSON:
`tests/fixtures/reference/baseline_m3_apple-m4pro-headless.json`.

| Fixture | Mode | out fps | parity offset (px) | expected | sink time base | tear |
| --- | --- | --- | --- | --- | --- | --- |
| telecine_3_2_1080i | fieldmatch-bwdif | 27.96 | +0.0 | +0.0 | 1/180000 | no |
| mixed_film_video_1080i | fieldmatch-bwdif | 28.77 | +0.0 | +0.0 | 1/180000 | no |
| interlaced_1080i5994_tff | fieldmatch-bwdif | 28.78 | +0.0 | +0.0 | 1/180000 | no |

M3 exit criteria:

* **Telecine fixtures reconstruct cleanly.** The playback output has the same
  frame count as the input (60), every output frame carries matched top and
  bottom fields (zero parity offset), and the film-index sequence derived from
  the bar positions is `0,0,1,2,3,4,4,5,...`: one retained duplicate per
  cadence group, no decimation. `video_processor_tests` compares the exact
  reconstructed film sequence and timing for synthetic 3:2 cadence.
* **Residual combing uses BWDIF.** Film mode on the pure video-rate 1080i
  fixture and on the video half of the mixed edit shows zero combing on the
  reference display path; the unit tests exercise BWDIF send_frame directly on
  frames left flagged interlaced.
* **Film/video transitions preserve duration and A/V sync.** The mixed
  fixture keeps 60 outputs at the coded rate across the hard edit; replayed
  with loop reopens, A/V offset stays below 0.02 s and the mode matrix
  screenshot after the edit has zero parity offset.
* **UI identifies the mode.** Diagnostics report `processing=fieldmatch-bwdif`
  (video line, worker processing line, `I` panel and `--stats`); the docs
  state that the nominal cadence is kept and duplicates are not removed.

Cost note: fieldmatch adds roughly 9.5 ms/picture for 1080i in the headless
software run (versus ~1.3 ms for BWDIF alone). Matching history is discarded
by the same generation reset used for retunes, processing-method changes,
format changes and timestamp discontinuities, so edits restart matching from
a clean state.
