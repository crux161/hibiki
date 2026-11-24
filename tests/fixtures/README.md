# Video playback fixtures (M0)

Deterministic synthetic MPEG-TS fixtures for the ARM64 video pipeline work
(see `docs/VIDEO_PIPELINE_ROADMAP.md`, milestone M0). Every fixture is built
from per-field patterns with known motion and field parity, so later milestones
can compare processed output against exact ground truth instead of eyeballing a
capture. `manifest.json` records the stream properties, the exact PTS list, the
per-field bar positions, the filter commands, the SHA-256 of each file and the
decoded-frame checksums under `reference/`.

Generation is byte-for-byte reproducible with the pinned **FFmpeg 9.1**
(`-threads 1`, bitexact flags). Another FFmpeg version produces structurally
identical fixtures, but its bitstream checksums will differ; `verify_fixtures.py`
records that as a warning and still checks structure, timestamps and content.

## Fixture catalog

| Fixture | Picture | Stream | What it exercises |
| --- | --- | --- | --- |
| `progressive_720p5994` | 1280x720p59.94 | MPEG-2 + AC-3, 120 frames | Baseline progressive timing; parity offset must be 0 |
| `interlaced_1080i5994_tff` | 1920x1080, 29.97 coded frames / 59.94 fields, TFF | MPEG-2 + AC-3, 60 frames | Top-field-first interlaced motion; 4 px even/odd combing |
| `interlaced_480i5994_bff` | 720x480 4:3 (SAR 8:9), 29.97/59.94, BFF | MPEG-2 + AC-3, 60 frames | Bottom-field-first and non-square pixels; -2 px parity offset |
| `telecine_3_2_1080i` | 1080i TFF from 23.976 film frames | MPEG-2 + AC-3, 60 frames | Hard 3:2 cadence `A/A, A/B, B/C, C/C, D/D`; hybrid frames comb |
| `mixed_film_video_1080i` | 1080i TFF | MPEG-2 + AC-3, 60 frames | 30 telecine frames then a hard edit to 59.94-field video motion |
| `flags_progressive_as_tff` | 720x480p content, 29.97 | MPEG-2, 30 frames | Progressive content incorrectly flagged TFF interlaced |
| `flags_interlaced_as_progressive` | 720x480 field-rate content, 29.97 | MPEG-2, 30 frames | Interlaced content incorrectly flagged progressive |
| `anamorphic_480p5994` | 720x480p59.94, DAR 16:9 (SAR 32:27) | MPEG-2 + AC-3, 90 frames | Anamorphic sample aspect ratio end to end |
| `bars_limited_720p` | 1280x720p59.94 | MPEG-2 + AC-3, 1 s | 75% BT.709 colour bars, TV range, exact YUV values |
| `bars_full_720p_h264` | 1280x720p59.94 | H.264 + AC-3, 1 s | Same bars in full range (`pc`, yuvj420p) |
| `timestamp_gap_720p` | 1280x720p59.94 | MPEG-2 + AC-3, 90 frames | +0.5 s PTS gap after frame 30 (splice-like timestamp gap) |
| `resolution_change` | 720x480 then 1280x720 at 59.94 | MPEG-2, 75 + 75 frames | Resolution change mid-stream with continuous PTS |
| `replay_short_720p` | 1280x720p59.94 | MPEG-2 + AC-3, 30 frames | Short capture for `--ts-file` replay looping |

Every frame is assembled from two field pictures:

* a white vertical bar sweeping horizontally (32 px wide on HD, 20 px on SD)
  inside rows 30%-70% of the picture; the bar advances by an exact number of
  pixels per **field** (or per film frame for telecine),
* top-left parity markers: block 1 is white in the top field, block 2 in the
  bottom field,
* an 8-bit barcode near the bottom encoding the field/film index, so any frame
  can be identified visually or from a screenshot.

`manifest.json` holds the authoritative ground truth per fixture:

```json
"video": {"pts_ticks": [...], "pts_rule": {...}, "field_order": "tt", ...},
"content": {"frames": [{"pts": 3003, "even_x": 64, "odd_x": 68, "film": null},
                       ...]}
```

`even_x`/`odd_x` are the bar's left edge on even (top-field) and odd
(bottom-field) raster rows of each coded frame. For progressive frames they are
equal; for interlaced frames they differ by exactly one field of motion in the
direction implied by the field order. `film` carries the two film-frame indices
for telecine/mixed fixtures. MPEG-TS timestamps start one frame after zero (the
muxer's encoder delay); timestamps are recorded exactly rather than assumed.

## Generate and verify

```sh
python3 tests/fixtures/generate_fixtures.py --out build/test-fixtures
python3 tests/fixtures/verify_fixtures.py --dir build/test-fixtures
```

`generate_fixtures.py` writes the media, `manifest.json` and
`reference/*.framemd5`. Override the tools with `HIBIKI_FFMPEG` /
`HIBIKI_FFPROBE`. `verify_fixtures.py` checks stream metadata, decoded frame
counts, the exact PTS list, per-frame sizes for the resolution change, the
moving bar's even/odd row positions, colour-bar YUV values and decoded-frame
checksums. Both exit 77 when their inputs are unavailable, so CTest reports a
skip instead of a failure.

After deliberately changing a fixture, refresh the checked-in ground truth:

```sh
python3 tests/fixtures/generate_fixtures.py --out build/test-fixtures --update-reference
```

That rewrites `tests/fixtures/manifest.json` and `tests/fixtures/reference/`.
Review those diffs like code. CI/CTest only ever generates into the build tree.

## Replay diagnostics and combing checks

```sh
python3 tests/fixtures/replay_check.py \
    --app build/Hibiki.app/Contents/MacOS/Hibiki \
    --dir build/test-fixtures [--quick] \
    [--baseline-json baseline.json --baseline-md baseline.md]
```

The script runs the real front-end headless (`SDL_VIDEODRIVER=dummy`,
`SDL_AUDIODRIVER=dummy`) with `--ts-file --stats`, parses the `[+] video:`,
`[+] presentation:` and `[stats]` lines, and verifies the reported codec,
declared/observed field order, requested-vs-active decoder and renderer,
VSync result, decoded/output rates, drops, A/V offset, queue durations and
presentation gaps. It also forces `--decoder cpu` and `--vsync off` once to
prove the requested values are reported separately, checks that the 0.5 s
fixture loops, and that `resolution_change` reports a size change.

Finally it captures screenshots of the 1080i TFF, 480i BFF and progressive
fixtures and measures the moving bar on even vs odd raster rows:

* **combing** is parity-structured: even and odd rows show the bar one field
  apart (+4 px for 1080i TFF, -2 px for 480i BFF, 0 for progressive),
* **tearing** would instead split the picture into horizontal blocks where
  *both* parities jump between two frames. The check asserts that no such
  within-field discontinuity exists in the captured frame.

`SDL_RenderReadPixels` captures the composed back buffer, not the display
scanout, so these checks cannot observe real presentation tearing. They
establish that what remains on screen is combing (a processing artefact with
parity structure) rather than a torn picture. Confirm tearing on physical
hardware:

1. Run on the reference display without `--no-hud`, e.g.
   `./hibiki-tv --ts-file build/test-fixtures/interlaced_1080i5994_tff.ts --stats`.
2. Press `I` and confirm `REN ... VSYNC ON`; compare with `--vsync off`.
3. Watch the sweeping bar at the display's native refresh rate. A parity
   fringe that alternates line-by-line is combing (expected until M2); a
   horizontal break where the bar jumps between two positions for a block of
   scanlines is tearing. The progressive fixture must show neither in steady
   playback.
4. Repeat with the 480i BFF and mixed film/video fixtures, and check menus
   stay responsive while the picture is moving.

Recorded baseline results and the named reference machines live in
`docs/PLAYBACK_BASELINE.md`.
