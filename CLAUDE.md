# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Portable C driver for the FLIR ADK 75° thermal camera (Boson 640 core, P/N 20640A075-6PADK). Captures 640x512 LWIR frames via V4L2 and controls the camera via CCI serial (FSLP protocol). Designed to cross-compile for multiple Linux targets: x86, ARM (Jetson Orin, NXP i.MX8), MIPS (OpenWrt/QCA9533), RHEL.

## Hardware

- **Sensor**: Boson 640, 640x512, 12µm VOx microbolometer, LWIR 8-14µm
- **Interface**: USB (UVC device → `/dev/videoX`), CCI serial (`/dev/ttyACMx`, 921600 8N1, FSLP protocol)
- **Video modes**: AGC 8-bit (YU12/I420, display-ready), RAW 16-bit (Y16, 14-bit radiometric), NV12 (`FLIR_MODE_NV12`, `flir_grab -m nv12`). 60 or 30 fps via `flir_open_ex(..., fps)` (VIDIOC_S_PARM), `flir_grab -r`, `flir_loop -f`
- **Frame rates**: 60fps, 30fps
- **On-camera hardware**: FFC/NUC, color LUTs, gain mode, telemetry, camera AGC for the 8-bit stream (parameters `agc*` 0x0009xxxx), noise filters (TF / SCNR / SPNR / BPR), e-zoom, symbology. Radiometry / TLinear / spot meter only on radiometric or Boson+ cameras
- **This unit** (20640A075-**6P**ADK, firmware 2.0.15223, sensor PN 500-0773, 60 fps, max zoom 49): non-radiometric. Status 0x0161 for radiometry / TLinear / spot meter and for the Boson+ `agcSetMode` 0x00090033 — plain Boson has no AGC *mode* command, but the AGC *parameters* work. Every Get in the Boson Software IDD (102-2013-44, 9 Hz edition; IDs match this 60 Hz unit) answered OK. Use the software spot meter (`flir_sw_spotmeter`) and HEQ AGC (`accel_generic`) for RAW16
- **Power-up defaults read from this unit**: FFC auto, `ffc-temp` 10 (1.0 °C), `ffc-period` 300 s; TF/SCNR/SPNR/BPR on; AGC entropy on, linear 20 %, plateau 7 %, max gain 1.38, gamma 0.97, df 85, detail headroom 12, d2br 1.05, sigmaR 5000; telemetry on

## Build

```
make                    # builds libflir_adk.a + flir_grab + flir_calibrate + cci_probe + flir_ctl + flir_loop
make test               # test_accel (AGC / labels / denoise) + test_cci (CCI payload encoding, no hardware)
make CC=aarch64-linux-gnu-gcc   # cross-compile for ARM64
make clean
```

No external dependencies beyond libc, libm, and Linux kernel headers (V4L2, termios).

## Test with hardware

```
# Probe camera CCI capabilities
./cci_probe -d /dev/video5 -c /dev/ttyACM0

# Grab frames
./flir_grab -d /dev/video5 -c /dev/ttyACM0 -m raw16 -n 5 > frames.raw

# Full calibration test (HEQ AGC + SW spot meter + iron-bow colorizer + CCI report)
./flir_calibrate -d /dev/video5 -c /dev/ttyACM0 -n 10 -o output

# With temperature display in Celsius (also accepts F or K)
./flir_calibrate -d /dev/video5 -c /dev/ttyACM0 -n 10 -o output -u C

# Python test (outputs PNGs directly)
python3 tools/flir_test.py -d /dev/video5 -c /dev/ttyACM0 -n 5 -o output
```

The camera appears as two V4L2 devices: the lower-numbered one is video capture, the higher one is UVC metadata.

## Architecture

- `include/flir_adk.h` — public API, enums for AGC modes / color LUTs / gain modes
- `src/internal.h` — shared struct layout for opaque `flir_ctx`
- `src/capture.c` — V4L2 MMAP streaming (open/close/start/stop/grab)
- `src/cci.c` — FSLP core + original wrappers (IDs from Boson+ SDK IDD Rev410); zoom sends 12-byte FLR_SCALER_ZOOM_PARAMS_T
- `src/cci_util.h` — static inline big-endian / float helpers + `cci_get`
- `src/cci_params.c` — `flir_params[]`: one table of every scalar Get/Set (name, IDs, wire type, range, help). Drives the C API, flir_ctl, `status` and the web whitelist
- `src/cci_system.c` — fw rev, sensor PN, save/factory/reboot, AGC & stats ROI, splash, read-only fileOps
- `src/cci_symbology.c` — on-camera symbology: text / rectangles / line / show / delete / move
- `src/spotmeter.c` — software spot meter (ROI stats from RAW16 frames, works on any camera variant)
- `src/colorbar.c` — thermal OSD overlay (color bar, crosshair, spot meter box, FPA temp, temperature unit conversion). 5x7 bitmap font at 2x scale, zero deps
- `platform/accel_generic.c` — HEQ AGC (20 % linear blend, `LINEAR`) + iron-bow colorizer (pure C, no deps)
- `platform/accel_cuda.c` / `accel_imx8.c` — per-SoC stubs
- `tools/flir_grab.c` — CLI raw frame capture
- `tools/flir_calibrate.c` — full calibration: CCI report + RAW16 capture + SW spot meter + HEQ + colorize + OSD overlay + stats. `-u C|F|K` for temperature unit display
- `tools/cci_probe.c` — CCI capability probe, read-only, CCI-only (discovers which modules camera supports)
- `tools/test_cci.c` — socketpair fake camera, checks FSLP payloads (no hardware)
- `tools/flir_test.py` — Python capture + PNG output (requires Pillow, ffmpeg)
- `src/denoise.c` — motion-adaptive temporal denoise on RAW16
- `tools/flir_ctl.c` — one CCI command per run, one JSON line out. Uses CCI-only `flir_open(NULL, cci, …)` so it works while another process streams. See `## flir_ctl commands`
- `tools/flir_web.py` — stdlib web control panel (127.0.0.1:8080) wrapping flir_ctl. Sections: FFC, Camera AGC, Noise filters, System, Image statistics, DVO, Symbology, Persist. Destructive actions (Persist/save, factory, reboot) ask for a confirm dialog
- `tools/flir_loop.c` — RAW16 → denoise → HEQ → palette → OSD → YUYV 740x512 into a v4l2loopback device for VLC/OBS. `-f 30|60` selects the camera frame rate. Live settings via control file `/tmp/flir_loop.ctl` (`lut=4 unit=C spot=1 nr=1`). Frozen input during FFC/freeze holds the last output and restarts denoise; no CCI polling
- `tools/obs_flir.lua` / `tools/vlc_flir.lua` — OBS script / VLC extension: display settings → control file; FFC mode, camera filters, status, and save (with confirm) → flir_ctl
- `android/` — standalone Android app (USB-OTG): libusb used directly for RAW16 video (uvcvideo-style: app sends PROBE/COMMIT, 5 x 16 KB bulk transfers; libuvc removed because its 8 x 656 KB transfers corrupted kernel memory on the Lenovo Tab K10; streams on the Lenovo after a clean boot; its USB controller can wedge until reboot, see README "Limits"), repo C pipeline via JNI (`app/src/main/cpp/native-lib.c`), Kotlin CCI over the CDC bulk interface (`cci/CciClient.kt`, `cci/CciParams.kt` = port of `flir_params[]`, keep both tables identical), full settings dialog generated from that table, Photo (PNG) and Record video (`VideoRecorder.kt`, MediaRecorder surface, H.264 MP4) under the image. Build: `cd android && ./fetch_deps.sh && ./gradlew assembleDebug testDebugUnitTest`. The Boson free-runs its last USB format and only applies a new UVC COMMIT after a Linux-style stop (cancel transfers + CLEAR_HALT 0x81), so `nativeStart` negotiates Y16 itself (PROBE + COMMIT), starts, stops, re-negotiates and starts again — see `android/README.md` before touching it. Never reintroduce large bulk transfers (8 x 656 KB) on the Lenovo
- `FLIR/` (git-ignored) — FLIR downloads: Boson SDK/IDD 2.0.6 (matches this unit's firmware; `FunctionCodes.h` confirms every ID in `flir_params[]`), 3.0, Boson+ Rev410, Windows GUIs. Licensed: reference only, never commit

Virtual camera setup: `sudo v4l2loopback-ctl add -n "FLIR Thermal" -x 1 /dev/video10`, then `./flir_loop -d /dev/video3 -o /dev/video10 -c /dev/ttyACM0 -u C`, then `vlc v4l2:///dev/video10`. The Boson exposes no V4L2 controls, so all control goes through CCI.

The `struct flir_accel` interface lets platform-specific code provide accelerated AGC and colorization.

## CCI protocol (verified)

FSLP frame layout (from rawBoson reference implementation + Boson+ SDK IDD Rev410):
```
[0]      0x8E start flag
[1]      channel (0x00)
[2..5]   sequence (big-endian, byte[5] cycles 0-9)
[6..9]   function ID (big-endian)
[10..13] status (TX: 0xFFFFFFFF, RX: 0x00000000 = OK)
[14..N]  payload
[N+1..2] CRC-16/AUG-CCITT (init 0x1D0F, poly 0x1021)
[last]   0xAE end flag
```
Byte-stuffing: `0x8E→0x9E,0x81` / `0x9E→0x9E,0x91` / `0xAE→0x9E,0xA1` (unstuff: +0x0D)

Key verified command IDs:
| Command | FunctionID | Response |
|---|---|---|
| bosonGetCameraSN | 0x00050002 | UINT_32 |
| bosonGetSensorSN | 0x00050006 | UINT_32 |
| bosonRunFFC | 0x00050007 | — |
| bosonSetGainMode | 0x00050014 | — |
| bosonlookupFPATempDegCx10 | 0x00050030 | INT_16 (°C × 10) |
| agcSetMode | 0x00090033 | — (Boson+ only; 0x161 on Boson) |
| colorLutSetId | 0x000B0003 | — |
| telemetrySetState | 0x00040001 | — |
| scalerSetZoom | 0x000D0002 | — (payload 12 B: zoom, xCenter, yCenter) |
| spotMeterSetEnable | 0x00430000 | — |
| spotMeterGetTempStats | 0x00430008 | 24 bytes (mean/dev/min/max as floats) |
| TLinearSetControl | 0x003E0001 | — |
| radiometryGetTempFromCounts | 0x00420038 | FLOAT (degK) |
| bosonSetFFCMode | 0x00050012 | — |
| bosonGetFFCMode | 0x00050013 | INT_32 |
| bosonWriteDynamicHeaderToFlash | 0x00050018 | — |
| bosonReboot | 0x00050010 | — |
| bosonGetSoftwareRev | 0x00050022 | 3 x UINT_32 |
| agcSetGamma | 0x0009000D | FLOAT |
| sysctrlGetCameraFrameRate | 0x000E0007 | UINT_32 |

Every scalar Get/Set lives in flir_params[] (src/cci_params.c) — that table is the reference. Splash getters need a UINT_32 screen number as input (no payload -> 0x0000017F); fileOpsGetFileSize needs a 128-byte path.

## flir_ctl commands

`flir_ctl [-c /dev/ttyACM0] COMMAND [ARGS] [--confirm]` prints one JSON line; exit 0 ok, 1 open_failed, 2 usage / unknown_command / bad_value / read_only / confirm_required, 3 cci_failed (+ `"status":"0x..."`).

- Scalar settings: `NAME` reads, `NAME VALUE` writes. Examples: `ffc-mode 0|1|2`, `ffc-temp 10`, `ffc-period 300`, `tf 0|1`, `scnr`, `spnr`, `bpr`, `tf-delta-nf`, `spnr-sf-min 0.5`, `agc-gamma 0.95`, `agc-linear-percent 20`, `agc-entropy 0|1`, `freeze 0|1`, `gain`, `lut`, `dvo-format`, `sym 0|1`
- Read-only: `fps`, `frame-count`, `myriad-temp`, `ffc-status`, `ffc-in-progress`, `overtemp`, `lens`, `table`, `stats-mean`
- `status` = every readable value in one JSON object (read-only)
- Identity: `fw`, `sensor-pn`, `sn`, `sensor-sn`, `pn`, `fpa`, `ffc`
- `zoom N [X Y]`, `agc-roi [R0 R1 C0 C1]`, `stats-roi [...]`, `splash-duration SCREEN [MS]`, `splash-bg SCREEN [0xRRGGBB]`, `ls`, `file-size PATH`
- Symbology: `sym-text ID X Y W H SIZE ALIGN 0xAARRGGBB TEXT`, `sym-rect|sym-box ID X Y W H COLOR`, `sym-line ID X1 Y1 X2 Y2 COLOR`, `sym-show ID 0|1`, `sym-delete ID`, `sym-move ID DX DY`
- Destructive (needs `--confirm`): `save --confirm` (writes flash), `factory-reset --confirm`, `reboot --confirm`

`flir_ctl -h` lists everything.

## RAW16 → temperature

Two approaches:
1. **TLinear mode**: `sysctrlSetUsbVideoIR16Mode(2)` via `0x000E000D` + `TLinearSetControl(1)` — pixel values become linearly proportional to temperature
2. **RBFO formula**: `temp_K = B / ln(R / (S - O) + F)` using params from `radiometryGetRBFOHighGainDefault` (`0x0042001B`)

## Cross-compilation targets

| Target | SoC | Accelerator | Notes |
|--------|-----|-------------|-------|
| x86_64 | any | generic (CPU) | Development/laptop |
| aarch64 | Jetson Orin | CUDA/NPP (stub) | GPU-accelerated AGC |
| aarch64 | NXP i.MX8 | Vivante/OpenCL (stub) | VPU + GPU |
| mips | QCA9533 | generic (CPU) | OpenWrt, capture + stream only |

## Temperature display (non-radiometric)

Since this camera variant has no hardware radiometry, temperature values shown on the OSD are approximate. The conversion uses a linear model referenced to the FPA temperature: `temp_C = fpa_C + (raw - FLIR_CAL_RAW_AT_FPA) / FLIR_CAL_COUNTS_PER_DEG` (defaults in `flir_adk.h`, override in `flir_loop` with `-r` / `-s`).

- Measured 2026-11-18 on this unit, high gain, two-point: ceiling @ 21°C = 22383 counts, skin @ ~34°C = 23313 counts → 71.5 counts/°C, raw at FPA (37.9°C) = 23592
- Do NOT use the frame mean as reference: FPA temp (~38°C) is far above the scene, which made faces read ~57°C
- Unverified: whether `RAW_AT_FPA` stays valid after warm-up / FFC. Re-measure with a known reference if readings drift. 2026-11-20 (window heater on, warm): hand read ~43 °C, so the Android app has a user offset (**Calibrate spot…**, `nativeCalibrate`, stored in prefs)
- RAW16 values are full 16-bit (~22000), not 14-bit — never mask with 0x3FFF

## Key constraints

- Zero external deps — runs on OpenWrt with musl libc (`-lm` for calibrate tool only)
- `_GNU_SOURCE` required in .c files that include `linux/videodev2.h` or use `cfmakeraw`
- `video` group for `/dev/videoX`, `dialout` group for `/dev/ttyACMx`
- First frame after format switch (AGC8↔RAW16) may be wrong size — `flir_grab` skips it
- FFC shutter cycle takes ~1.5s — wait before reading post-FFC frames
- Destructive CCI (save, factory-reset, reboot) needs `--confirm` in flir_ctl and a confirm step in web/OBS/VLC. fileOps write/erase and mem* flash commands are intentionally not wrapped
- flir_loop never polls CCI for FFC: identical consecutive frames = FFC/freeze
