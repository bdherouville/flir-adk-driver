package dev.thermalcam.boson

import android.graphics.Bitmap
import android.view.Surface

/** JNI entry points of libbosonthermal.so (app/src/main/cpp/native-lib.c). */
object NativeBridge {
    const val OUT_WIDTH = 740
    const val OUT_HEIGHT = 512

    init {
        System.loadLibrary("bosonthermal")
    }

    /** Start RAW16 streaming on the UsbDeviceConnection fd. 0 = ok, else a libuvc uvc_error_t (< 0). */
    @JvmStatic external fun nativeStart(fd: Int): Int

    /** Stop streaming and release libuvc/libusb (the fd stays open, Java closes it). Safe to call twice. */
    @JvmStatic external fun nativeStop()

    /** Output surface, or null when it is destroyed. */
    @JvmStatic external fun nativeSetSurface(surface: Surface?)

    /** lut: 4 iron-bow, 0 white-hot, 1 black-hot. unit: 0 raw, 1 C, 2 F, 3 K. */
    @JvmStatic external fun nativeSetSettings(lut: Int, unit: Int, spot: Boolean, nr: Boolean)

    /** FPA temperature in degC x10 from CCI; 0 = unknown (overlay falls back to raw counts). */
    @JvmStatic external fun nativeSetFpa(fpaCx10: Int)

    /** Copy the last composed frame into a 740x512 ARGB_8888 bitmap. false if no frame yet. */
    @JvmStatic external fun nativeSnapshot(bitmap: Bitmap): Boolean
}
