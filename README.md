# Hibiki (響)

Hibiki is a modern RE project dedicated to making the MyGiga USB-C ATSC tuner usable
outside of the NewPadTVHD app for Android.

## Build & run (macOS)

Requires `libusb`, and for the TV front-end `sdl3`, `sdl3_ttf` and `ffmpeg`
(Homebrew). The FX2 bridge firmware must already be loaded (`scripts/wake_tuner.py`).

```sh
./scripts/build.sh          # CMake -> ./hibiki (CLI), ./Hibiki.app and ./hibiki-tv
./scripts/sign.sh           # ad-hoc codesign Hibiki.app (debug entitlements)
./hibiki --scan             # CLI: sweep ATSC channels
./hibiki --freq 201028615   # CLI: tune + serve MPEG-TS over HTTP (VLC)
open ./Hibiki.app           # TV front-end (video + audio + HUD)
./hibiki-tv --demo          # TV front-end from a shell, with flags (no tuner needed)
```

`hibiki` is the headless CLI. The TV front-end is the binary inside the app
bundle; `hibiki-tv` (also created in the CMake build dir, `build/hibiki-tv`)
is a symlink to it so front-end flags can be passed directly. From Finder or
`open`, pass flags with `open Hibiki.app --args --demo`.

On first run (or `--rescan` / `S`) the app scans the US ATSC channel plan,
remembers the channels that lock (per-user `channels.txt`) and reads each
one's PSIP channel table, so the channel list, channel up/down and the HUD all
use the broadcast's virtual channels (`13-1 KEET-HD`, `13-2 PBSKids`, ...).
Sub-channels on the RF channel already tuned switch instantly. It reopens on
the last channel and sub-channel watched.

### Playback pipeline

`src/player.{h,cpp}`: a demux thread reads the TS (tuner ring or file) through
libavformat into video and audio packet queues; the UI thread decodes, keeps
~0.25 s of audio queued in SDL, and shows each video frame when its PTS comes
due on the audio clock (wall clock when there is no audio), dropping late
frames and resynchronising on timestamp jumps. The UI never blocks on the
signal, so a dropout freezes the picture with `NO SIGNAL` instead of hanging
the menus. The status HUD (`H`) shows the measured A/V offset, audio buffer
and dropped frames; `--stats` prints them every 2 s.

The on-screen display (`src/osd.{h,cpp}`) uses the bundled VCR OSD font
(`res/font/`): a VHS-style playback HUD (channel, clock, volume, signal) and a
blue VCR set-up menu. With no picture the screen falls back to VCR "blue back"
with `NO SIGNAL` instead of exiting.

Front-end controls (remote-style):

| Key | Action |
| --- | --- |
| `Enter` / `Tab` | open MENU (in menus: set) |
| `Esc` / `Backspace` | back / close menu (no menu: quit) |
| `↑` `↓` | volume (in menus: select) |
| `←` `→`, `PageUp` `PageDown` | virtual channel down / up (in menus: adjust value) |
| `0`–`9`, `-` / `.` | direct entry: `13` = first 13-x, `13-2` = that sub-channel, else RF 13 (commits on `Enter` or 1.5 s) |
| `C` / `L` | channel list (`Space` toggles favourite) |
| `G` | program guide (in a channel's listing `←` `→` switch sub-channel, `Enter` = info) |
| `A` | next audio track |
| `Z` | screen scale: FIT / FULL / STRETCH |
| `I` | show channel info banner |
| `H` | persistent status HUD (signal meter, lock flags, A/V sync) |
| `F` / `M` / `S` / `Q` | fullscreen / mute / auto channel scan / quit |

MENU: channel list, program guide, sub channel (the RF channel's MPEG
programs), audio track, screen scale, set up feature (HUD, scanlines, 12/24 h
clock, volume, favourites-only surfing), set up channel (scan range 2-36 or
2-51, auto scan, clear favourites) and about. Items marked `SOON` (closed
captions, record/timer, parental lock) are placeholders for PadTV parity work.
Preferences persist in `settings.txt` next to `channels.txt`.

### Program guide (ATSC PSIP)

libavformat ignores ATSC PSIP, so `src/psip.{h,cpp}` decodes it from the raw
TS as the demuxer reads it: MGT, terrestrial/cable VCT (virtual channel
`major-minor` numbers and names, mapped to MPEG programs), STT (broadcast
time), EIT-k (events, content-advisory rating) and ETT (descriptions). The HUD
shows `CH 3-1`, the station name and what is on now; the guide lists each
sub-channel's now-playing title, its schedule and per-event info. Guide data is
cached per RF channel in `psip/<rf>.txt` (next to `channels.txt`), so names and
listings appear as soon as a channel is tuned and the channel list shows
station names. Huffman-compressed strings (A/65 Annex C) are not decoded yet
and show as `[COMPRESSED]`. `ctest` (or `build/psip_test`) runs the decoder
against a synthetic PSIP stream; `--demo` feeds the same stream to the UI.

### Testing without an antenna

- `./hibiki-tv --demo` runs the UI with colour bars, fake channel memory and a
  synthetic PSIP stream. With `--screenshot out.bmp` it saves a frame and exits.
- `./hibiki-tv --dump-ts cap.ts` records what the tuner delivers; later
  `./hibiki-tv --ts-file cap.ts --channel 11` replays it in real time (looping)
  through the same player, PSIP decoder and HUD, without touching the tuner or
  your saved settings.
- `--keys G,+1000,Return` scripts key presses (SDL key names; `+ms` waits),
  e.g. to screenshot the guide once PSIP has arrived.

**Firmware is not redistributable and is NOT in this repo.** Obtain the blobs
separately (verify against the SHA-256 table below) and place them in `fw/`, or
point the app at them with `--fw <path>` / `$HIBIKI_FW` /
`~/Library/Application Support/hibiki/fw/`. The Eagle blob
(`mbin_mxl_eagle_fw.bin`) is what the tuner loads at runtime.
A local build also looks in the source tree's `fw/` and, when built from a git
worktree, in the main checkout's `fw/`. If the firmware or the stick is
missing, the app stays open on a `TUNER ERROR` page (with `RETRY`, or `R`)
rather than quitting, since a Finder launch has no terminal to show errors.

### Carrying the firmware as base64

`scripts/fw_bundle.py` packs the two blobs needed to run — the Eagle runtime
firmware and the PT930 FX2 bootstrap used by `wake_tuner.py` — into one
SHA-256-pinned base64 bundle, and verifies/restores from either that bundle,
a raw `fw/` tree, or a bare base64 snippet of one blob:

```sh
./scripts/fw_bundle.py pack                      # fw/ -> fw_package/hibiki-fw-b64.json
./scripts/fw_bundle.py verify  --src fw_package  # decode + check size/SHA-256
./scripts/fw_bundle.py install --src fw_package  # verify, then write into fw/
./scripts/fw_bundle.py verify  --src fw          # check a raw tree in place
./scripts/fw_bundle.py show    --src fw_package mbin_mxl_eagle_fw.bin > eagle.b64
```

`verify`/`install` also accept a lone `.b64` snippet or a directory of them.
The bundle is only base64-encoded, so `fw_package/` is gitignored exactly like
`fw/`: do not commit or redistribute it.

## Extracted Firmware Candidates

Source tree: `MyGica-atsc/NewPad`

## Primary candidates

### Silicon Labs patch streams

Extracted from `lib/armeabi-v7a/libPadTV_jni.so`, section `.data`.

File offset = symbol value - `0x1000`.

| Symbol | VA | File offset | Size | Notes |
| --- | ---: | ---: | ---: | --- |
| `Si2144_FW_2_1b2` | `0x000d61f8` | `0x000d51f8` | 510 | `30 * 17` framed rows |
| `Si2141_FW_1_1b12` | `0x000d63f6` | `0x000d53f6` | 2159 | `127 * 17` framed rows |
| `Si2168B_PATCH16_4_0b11` | `0x000d6c65` | `0x000d5c65` | 6919 | `407 * 17` framed rows |
| `Si2183_PATCH16_6_0b13` | `0x000d876c` | `0x000d776c` | 10829 | `637 * 17` framed rows |

For each symbol:

- `*.table.bin` is the exact 17-byte-row table from the shared object.
- `*.commands.bin` is the concatenated command stream with each row's leading length byte removed and zero padding discarded.

### MaxLinear Eagle blob

Extracted from `lib/armeabi-v7a/libPadTV_jni.so`.

| Symbol | VA | File offset | Size | Notes |
| --- | ---: | ---: | ---: | --- |
| `mbin_mxl_eagle_fw` | `0x000ecd48` | `0x000ebd48` | 43997 | Starts with `M1`; contains ThreadX Xtensa strings |

This is probably firmware for the MxL692/Eagle ATSC frontend, not 8051.

### PT930 pre-symbol 8051-like region

Extracted from `lib/armeabi-v7a/libPT930Drv.so`.

| Region | File offset | Size | Notes |
| --- | ---: | ---: | --- |
| `.data` pre-symbol region | `0x00016000` | 4212 | Contains 8051-like opcodes and loader-style records before exported `MN88553` config symbols |

The first exported PT930 symbols at `.data` are text/config resources such as `MN88553`, `MN88553_chfile_20160317X26_3`, `PSEQ1_MN88553_160304_bprty`, and `TMM3_TNCTL_*`. Those are extracted under `PT930_MN88553/` as exact bytes.

## SHA-256

```text
32403bbb46b673dd4e24ddcf458f79dc16e8663f06c404cc2d01981d14edf13f  Si2141_FW_1_1b12.commands.bin
5df0cb9fbc18f12a420c6741afe5e8f5322d48e620b5f16858f5cdb9e4a40a53  Si2141_FW_1_1b12.table.bin
2642da7d8b1e64a7a5778871aec89bd6840687fd046ba59e643c515b0a443437  Si2144_FW_2_1b2.commands.bin
ec993282e01f7173c60362e60a41711d813da1d06005d7659abc59541c296a3e  Si2144_FW_2_1b2.table.bin
72c91339f5246b4bbe468c4774c59b57a74b09a48e787adaaee98eca8ebc427f  Si2168B_PATCH16_4_0b11.commands.bin
8507536630d75a316d0719d6b95c04b90c36baa5b457ad457c9bacadafcef134  Si2168B_PATCH16_4_0b11.table.bin
82bae47f82e6c3b2829d487ca701684e60ef3a705ee162dcf22827f384ccddbd  Si2183_PATCH16_6_0b13.commands.bin
5e1c860e95a8d92b2716b0075dd00e55f99c2c368e35340f6b527c366ff27ee7  Si2183_PATCH16_6_0b13.table.bin
8f77d5cb0de9111ed63c113cb2fd54a63e07a30040ae0b562cb025ab9c4e600d  mbin_mxl_eagle_fw.bin
0c3348a7f30fba3c9d3a1463f2583ce64f6af7922da4e60531f05473ce711d2a  PT930_MN88553/PT930_pre_symbol_8051_region.bin
```
