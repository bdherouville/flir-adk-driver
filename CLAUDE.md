# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Portable C driver for the FLIR ADK 75° thermal camera (Boson 640 core, P/N 20640A075-6PADK). Captures 640x512 LWIR frames via V4L2 and controls the camera via CCI serial (FSLP protocol). Designed to cross-compile for multiple Linux targets: x86, ARM (Jetson Orin, NXP i.MX8), MIPS (OpenWrt/QCA9533), RHEL.

## Hardware

- **Sensor**: Boson 640, 640x512, 12µm VOx microbolometer, LWIR 8-14µm
- **Interface**: USB (UVC device → `/dev/videoX`), CCI serial (`/dev/ttyACMx`, 921600 8N1, FSLP protocol)
- **Video modes**: AGC 8-bit (YU12/I420, display-ready), RAW 16-bit (Y16, 14-bit radiometric), NV12
- **Frame rates**: 60fps, 30fps
- **On-camera hardware**: FFC/NUC, color LUTs, gain mode, telemetry. AGC/spot meter/TLinear/radiometry only on radiometric variants (part suffix "R", not "P")
- **This unit** (20640A075-**6P**ADK): non-radiometric. Status 0x0161 for AGC/radiometry/TLinear/spotmeter/scaler CCI modules. Use software spot meter (`flir_sw_spotmeter`) and HEQ AGC (`accel_generic`) instead

## Build

```
make                    # builds libflir_adk.a + flir_grab + flir_calibrate + cci_probe
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
- `src/cci.c` — FSLP serial protocol + CCI command wrappers (IDs from Boson+ SDK IDD Rev410)
- `src/spotmeter.c` — software spot meter (ROI stats from RAW16 frames, works on any camera variant)
- `src/colorbar.c` — thermal OSD overlay (color bar, crosshair, spot meter box, FPA temp, temperature unit conversion). 5x7 bitmap font at 2x scale, zero deps
- `platform/accel_generic.c` — HEQ AGC + iron-bow colorizer (pure C, no deps)
- `platform/accel_cuda.c` / `accel_imx8.c` — per-SoC stubs
- `tools/flir_grab.c` — CLI raw frame capture
- `tools/flir_calibrate.c` — full calibration: CCI report + RAW16 capture + SW spot meter + HEQ + colorize + OSD overlay + stats. `-u C|F|K` for temperature unit display
- `tools/cci_probe.c` — CCI capability probe (discovers which modules camera supports)
- `tools/flir_test.py` — Python capture + PNG output (requires Pillow, ffmpeg)

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
| agcSetMode | 0x00090033 | — |
| colorLutSetId | 0x000B0003 | — |
| telemetrySetState | 0x00040001 | — |
| scalerSetZoom | 0x000D0002 | — |
| spotMeterSetEnable | 0x00430000 | — |
| spotMeterGetTempStats | 0x00430008 | 24 bytes (mean/dev/min/max as floats) |
| TLinearSetControl | 0x003E0001 | — |
| radiometryGetTempFromCounts | 0x00420038 | FLOAT (degK) |

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

Since this camera variant has no hardware radiometry, temperature values shown on the OSD are approximate. The conversion uses a linear model: `temp_C = ref_temp_c + (raw - ref_raw) / counts_per_deg`, where:
- `ref_temp_c` = FPA temperature from CCI (the only real temperature reading)
- `ref_raw` = frame mean raw count (assumed to correspond to FPA temp)
- `counts_per_deg` = ~40 counts/°C (empirical estimate from observed raw range vs scene span)

The `flir_overlay_opts` struct in `flir_adk.h` carries the calibration parameters. `flir_calibrate` auto-populates them from FPA temp and frame statistics.

## Key constraints

- Zero external deps — runs on OpenWrt with musl libc (`-lm` for calibrate tool only)
- `_GNU_SOURCE` required in .c files that include `linux/videodev2.h` or use `cfmakeraw`
- `video` group for `/dev/videoX`, `dialout` group for `/dev/ttyACMx`
- First frame after format switch (AGC8↔RAW16) may be wrong size — `flir_grab` skips it
- FFC shutter cycle takes ~1.5s — wait before reading post-FFC frames
