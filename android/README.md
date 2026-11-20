# Boson Thermal (Android)

Standalone Android app for the FLIR Boson 640 thermal core over USB-OTG. Developed against
part 20640A075-6PADK (non-radiometric, USB VID 0x09CB / PID 0x4007, firmware 2.0.15223).
It streams RAW16, renders it with the same C pipeline as `tools/flir_loop.c` and controls the
camera over CCI. The app is not affiliated with Teledyne FLIR. FLIR and Boson are their trademarks.

## What it does

- Video: RAW16 (UVC `Y16 `, 640x512) through libusb, called directly. Java cannot read the video
  stream, so the app gives the `UsbDeviceConnection` file descriptor to native code
  (`libusb_wrap_sys_device`). The app sends PROBE/COMMIT itself and reads 5 bulk transfers of
  16 KB each, parsed the way Linux `uvcvideo` does it (header, FID/EOF, payload end on a short
  transfer or at dwMaxPayloadTransferSize).
- Processing (native, repo C code compiled unchanged): freeze/FFC hold, temporal denoise, HEQ AGC,
  iron-bow / white-hot / black-hot palette, color bar with temperature labels, spot meter.
  Output: 740x512 RGBA (image plus 100 px color bar) drawn on a `SurfaceView`.
- Control (Kotlin): the CCI/FSLP protocol over the camera's CDC-ACM data interface (USB bulk).
  One thread owns the port. FPA and Myriad temperatures are polled every 5 s.
- UI: palette, units (°C/°F/K/raw), spot meter, noise reduction, Run FFC, gain mode,
  FFC mode, the camera's TF/SCNR/SPNR filters, status (FPA, Myriad, firmware, SN, PN),
  Save settings (asks for confirmation; writes the camera's flash), an "All camera settings"
  dialog generated from the CCI table, and Photo (PNG) / Record video (H.264 MP4) buttons
  under the image.

## Build

Requirements:

- JDK 17 or newer with `javac`, through `JAVA_HOME` (tested: Temurin 21). The Fedora
  `java-*-openjdk-headless` packages have no `javac` and do not work.
- Android SDK through `ANDROID_HOME` (or `sdk.dir` in `android/local.properties`) with
  `platforms;android-35`, `ndk;29.0.14206865` and `cmake;3.22.1`.
- Versions: Gradle 8.11.1 (wrapper), Android Gradle Plugin 8.7.3, Kotlin 2.2.0,
  compileSdk/targetSdk 35, minSdk 26, ABIs arm64-v8a and x86_64.

```
cd android
./fetch_deps.sh                 # libusb v1.0.29 -> app/src/main/cpp/third_party/
./gradlew assembleDebug testDebugUnitTest
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`. Run `fetch_deps.sh` again
whenever you like. It keeps checkouts that are at the pinned commit and replaces anything else.

The native library also compiles these files from the parent repo, through relative paths:
`platform/accel_generic.c`, `src/colorbar.c`, `src/denoise.c`, `src/spotmeter.c`, `include/flir_adk.h`.
It does not compile `src/capture.c` or `src/cci*.c`, which are Linux V4L2/termios code.

## Install and use

```
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

1. Plug the Boson into the phone with a USB-OTG adapter or cable. Android offers to open
   Boson Thermal (from `res/xml/device_filter.xml`).
2. Allow the Camera permission. From Android 9 on, apps can only open a USB video-class device
   if they hold CAMERA.
3. Allow USB access to the device.
4. Video starts. Unplugging stops the stream cleanly, and so does leaving the app. Rotating
   the phone does not restart the stream.

Photos go to `Pictures/BosonThermal/` and videos to `Movies/BosonThermal/` (MediaStore) on
Android 10 and later. On Android 8-9 they go to the app's own `Android/data/dev.thermalcam.boson/
files/Pictures/` and `.../files/Movies/`. Videos record what the screen shows (image, color bar,
overlay) at 736x512, 30 fps, H.264, no audio: `VideoRecorder.kt` draws the latest frame on a
`MediaRecorder` surface with a hardware canvas.

## USB layout (measured on this unit)

| Interface | Class | Endpoints | Used by |
|---|---|---|---|
| 0 | UVC control | none | libusb (claimed; control transfers only) |
| 1 | UVC streaming | 0x81 bulk IN (512) | libusb (bulk mode, no alt settings) |
| 2 | CDC comm | 0x83 interrupt IN | SET_LINE_CODING only (failures ignored) |
| 3 | CDC data | 0x01 bulk OUT, 0x82 bulk IN | `CciClient` (FSLP) |

Native code and Kotlin share one `UsbDeviceConnection` and claim different interfaces.

Video formats (interface 1): 1 = I420, 2 = Y16, 3 = NV12, 4 = NV21, all 640x512, 60 or 30 fps.
bcdUVC is 0x0100, so PROBE/COMMIT are 26 bytes.

## Starting RAW16 (why `nativeStart` starts twice)

The Boson free-runs. It keeps streaming the format it last had (I420 at 60 fps after Android's
`uvcvideo`, the Samsung camera service, or an earlier session) and stores a new COMMIT without
applying it. It applies it only after a stream stop done the way Linux `STREAMOFF` does it:
cancel the bulk transfers, then CLEAR_HALT on endpoint 0x81. SET_INTERFACE or CLEAR_HALT
alone, before the commit, are not enough (all tried, measured).

`nativeStart` therefore:

1. negotiates Y16 itself, like `uvcvideo`: GET_CUR PROBE, change format / frame / interval,
   SET_CUR PROBE, GET_CUR PROBE, then COMMIT (SET_CUR COMMIT), all sent by the app. The probe
   sets every field the app needs. Earlier, libuvc's own probe left fields unset, and the Boson
   answered with its I420 sizes;
2. starts streaming, waits 300 ms, stops (cancel), CLEAR_HALT, waits 100 ms;
3. negotiates again and starts for real. Frames go from 491520 bytes (I420) to 655360 (Y16).

If the switch ever fails, logcat (tag `BosonNative`) shows `dropping 491520-byte frame`.

## Limits

- **Lenovo USB controller.** After the libuvc sessions below, the Lenovo's USB host stopped
  moving any bulk data (video and CCI) while EP0 still worked; only a tablet reboot fixed it.
  On a clean boot the libusb loop streams there. Do not raise the transfer count or size.
- **Why not libuvc.** The libuvc build (8 x 656 KB bulk transfers) corrupted kernel memory on the
  Lenovo Tab K10: logcat and the kernel log showed `BUG: Bad page map` with camera pixel values
  (about 23000) in page-table entries, so USB DMA was writing outside its buffers. The app
  therefore uses small transfers (5 x 16 KB) and does not link libuvc.
- **Frame rate.** The camera sends RAW16 at 60 fps (39 MB/s). Measured: ~60 fps and 3-10 ms of
  native processing per frame on a Galaxy S25 Ultra (libuvc build); ~20 fps and ~19 ms/frame on
  the Lenovo Tab K10 (libusb build, CPU-bound). logcat prints fps and ms/frame every 600 frames.
- **Units and gain.** The calibration is high-gain only. In Low gain the overlay shows raw
  counts; in Auto (the power-up default) it shows temperatures and the status line warns that
  hot scenes may read wrong.
- **Temperatures are approximate.** The 6P variant is not radiometric. The app converts counts
  with the linear FPA-referenced model in `include/flir_adk.h`:
  `temp_C = FPA_C + (raw - FLIR_CAL_RAW_AT_FPA) / FLIR_CAL_COUNTS_PER_DEG`
  (23592 counts, 71.5 counts/°C). This was measured once, on this unit, in high gain. Until
  the first FPA read succeeds, the labels show raw counts. The radiometric CCI modules
  (TLinear, radiometry, spot meter) answer status 0x161 on this unit, so the spot meter is
  computed in software.
- **Power and USB host.** The phone must support USB host mode (OTG) and must supply the
  camera's power (the descriptor declares 500 mA). If the camera keeps disconnecting, use a
  powered OTG hub.
- **Warm-up and FFC.** After power-on the camera's auto FFC fires every ~20 s while the FPA warms.
  The app hides each FFC (holds the image for the frozen frames plus 8 settle frames), smooths the
  FPA reference (~30 s EMA), shows "Warming up" until the FPA moves <= 0.2 degC over 2 min, then runs
  one FFC. Camera settings changed in the app are stored and re-applied on every connect (not
  `freeze`); the camera's flash is not touched.
- One camera at a time. The HEQ state in `accel_generic.c` is per process.
- **Tested on** Lenovo Tab K10 TB-X6C6F (Android 12, MediaTek MT8768 MUSB): current libusb
  build, picture, °C overlay, CCI readout, video recording. Samsung Galaxy S25 Ultra (Android
  17, xHCI): libuvc build only, the libusb build has not run there yet. On the Lenovo, before the double start was added, it once
  rebooted with a kernel panic in MediaTek's `wmt_drv` (Wi-Fi/BT) while the app ran; the cause
  was not established. It has not recurred since.

## Layout

```
android/
  fetch_deps.sh                       pinned third-party fetch (idempotent)
  app/src/main/cpp/CMakeLists.txt     libusb + repo C code -> libbosonthermal.so
  app/src/main/cpp/native-lib.c       JNI: libusb UVC stream, flir_loop pipeline, SurfaceView output
  app/src/main/java/dev/thermalcam/boson/
    MainActivity.kt                   USB attach/permission/detach, UI, snapshot
    NativeBridge.kt                   JNI declarations
    ThermalSurfaceView.kt             740:512 aspect SurfaceView
    cci/Fslp.kt                       FSLP framing, CRC-16, byte stuffing
    cci/CciParams.kt                  port of src/cci_params.c (56 settings)
    cci/CciClient.kt                  CCI over USB bulk, single-thread request queue
  app/src/test/java/dev/thermalcam/boson/cci/
    FslpTest.kt                       wire vectors captured from the C driver (src/cci.c)
    CciParamsTest.kt                  table, range and encoding checks (mirrors tools/test_cci.c)
```

## Third-party code

These sources are fetched by `fetch_deps.sh` and are not committed. They are compiled by
`CMakeLists.txt` only. Their own build scripts are never run.

- libusb v1.0.29 (commit 15a7ebb4d426c5ce196684347d2b7cafad862626), LGPL-2.1
