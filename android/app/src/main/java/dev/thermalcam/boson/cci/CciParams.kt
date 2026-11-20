package dev.thermalcam.boson.cci

import java.nio.ByteBuffer

/**
 * One scalar CCI setting (port of struct flir_param_info, src/cci_params.c).
 * type = wire type, big-endian: 'e' INT_32 enum, 'u' UINT_32, 'h' INT_16, 'w' UINT_16, 'f' FLOAT.
 * setId == 0 means read-only; postId != 0 is sent without payload after a successful Set.
 */
class CciParam(
    val name: String,
    val getId: Int,
    val setId: Int,
    val postId: Int,
    val type: Char,
    val lo: Double,
    val hi: Double,
    val help: String,
) {
    val readOnly: Boolean get() = setId == 0
    val wireSize: Int get() = if (type == 'h' || type == 'w') 2 else 4

    /** Big-endian Set payload, or null (no I/O must happen) if read-only, NaN, outside [lo, hi],
     *  or not a whole number for a non-'f' type. Mirrors flir_cci_param_set(). */
    fun encode(v: Double): ByteArray? {
        if (readOnly || !(v >= lo && v <= hi)) return null
        if (type != 'f' && v != Math.floor(v)) return null
        val b = ByteBuffer.allocate(wireSize)
        when (type) {
            'e' -> b.putInt(v.toInt())
            'u' -> b.putInt(v.toLong().toInt())
            'h' -> b.putShort(v.toInt().toShort())
            'w' -> b.putShort(v.toInt().toShort())
            'f' -> b.putFloat(v.toFloat())
            else -> return null
        }
        return b.array()
    }

    /** Decode a Get response; null if shorter than wireSize. Mirrors flir_cci_param_get(). */
    fun decode(r: ByteArray): Double? {
        if (r.size < wireSize) return null
        val b = ByteBuffer.wrap(r)
        return when (type) {
            'e' -> b.getInt().toDouble()
            'u' -> (b.getInt().toLong() and 0xFFFFFFFFL).toDouble()
            'h' -> b.getShort().toDouble()
            'w' -> (b.getShort().toInt() and 0xFFFF).toDouble()
            'f' -> b.getFloat().toDouble()
            else -> null
        }
    }
}

/** Port of flir_params[] (src/cci_params.c), same order. */
object CciParams {
    val all: List<CciParam> = listOf(
        CciParam("ffc-mode", 0x00050013, 0x00050012, 0, 'e', 0.0, 2.0, "FFC mode: 0 manual, 1 auto, 2 external"),
        CciParam("ffc-temp", 0x00050009, 0x00050008, 0, 'w', 1.0, 1000.0, "auto-FFC trigger: FPA drift since last FFC, degC x10"),
        CciParam("ffc-period", 0x0005000B, 0x0005000A, 0, 'u', 1.0, 86400.0, "auto-FFC trigger: seconds since last FFC"),
        CciParam("ffc-in-progress", 0x0005000C, 0, 0, 'h', 0.0, 0.0, "1 while an FFC runs"),
        CciParam("ffc-status", 0x00050054, 0, 0, 'e', 0.0, 0.0, "0 none yet, 1 imminent, 2 in progress, 3 complete"),
        CciParam("ffc-desired", 0x00050055, 0, 0, 'u', 0.0, 0.0, "1 when the camera wants an FFC"),
        CciParam("ffc-last-frame", 0x0005005D, 0, 0, 'u', 0.0, 0.0, "frame count at the last FFC"),
        CciParam("ffc-last-temp-kx10", 0x0005005E, 0, 0, 'w', 0.0, 0.0, "FPA temperature at the last FFC, K x10"),
        CciParam("gain", 0x00050015, 0x00050014, 0, 'e', 0.0, 4.0, "gain mode: 0 high, 1 low, 2 auto, 3 dual, 4 manual"),
        CciParam("lut", 0x000B0004, 0x000B0003, 0, 'e', 0.0, 9.0, "on-camera colour LUT 0..9 (8-bit stream)"),
        CciParam("telemetry", 0x00040002, 0x00040001, 0, 'e', 0.0, 1.0, "telemetry line 0|1"),
        CciParam("freeze", 0x000E0002, 0x000E0001, 0, 'e', 0.0, 1.0, "freeze the video pipeline 0|1"),
        CciParam("fps", 0x000E0007, 0, 0, 'u', 0.0, 0.0, "camera frame rate"),
        CciParam("frame-count", 0x00020002, 0, 0, 'u', 0.0, 0.0, "frames since power-up"),
        CciParam("myriad-temp", 0x00050068, 0, 0, 'f', 0.0, 0.0, "processor (Myriad) temperature, degC"),
        CciParam("low-power", 0x00050062, 0, 0, 'w', 0.0, 0.0, "1 = low-power state after over-temperature"),
        CciParam("overtemp", 0x00050063, 0, 0, 'w', 0.0, 0.0, "1 = an over-temperature event occurred"),
        CciParam("overtemp-count", 0x00050076, 0, 0, 'u', 0.0, 0.0, "number of over-temperature events"),
        CciParam("lens", 0x00050039, 0, 0, 'u', 0.0, 0.0, "current lens number"),
        CciParam("table", 0x0005003B, 0, 0, 'u', 0.0, 0.0, "current NUC table number"),
        CciParam("jffs2-state", 0x00170007, 0, 0, 'e', 0.0, 0.0, "flash file system state, 3 = mounted"),
        CciParam("tf", 0x000A0002, 0x000A0001, 0, 'e', 0.0, 1.0, "temporal noise filter 0|1"),
        CciParam("tf-delta-nf", 0x000A0004, 0x000A0003, 0, 'w', 0.0, 255.0, "TF strength: delta_nf (scales wLUT index)"),
        CciParam("tf-delta-motion", 0x000A0006, 0x000A0005, 0, 'w', 0.0, 65535.0, "TF per-pixel motion threshold"),
        CciParam("tf-motion-threshold", 0x000A000F, 0x000A000E, 0, 'u', 0.0, 327680.0, "moving pixels needed to call a frame motion"),
        CciParam("scnr", 0x00080002, 0x00080001, 0, 'e', 0.0, 1.0, "column noise filter 0|1"),
        CciParam("scnr-th-col-sum", 0x00080004, 0x00080003, 0, 'w', 0.0, 65535.0, "SCNR column-sum threshold"),
        CciParam("scnr-th-pixel", 0x00080006, 0x00080005, 0, 'w', 0.0, 65535.0, "SCNR neighbour pixel threshold"),
        CciParam("scnr-max-corr", 0x00080008, 0x00080007, 0, 'w', 0.0, 65535.0, "SCNR maximum correction"),
        CciParam("spnr", 0x000C0002, 0x000C0001, 0, 'e', 0.0, 1.0, "spatial noise filter 0|1"),
        CciParam("spnr-sf-min", 0x000C001D, 0x000C001C, 0, 'f', 0.0, 10.0, "SPNR scale factor, static scene"),
        CciParam("spnr-sf-max", 0x000C001F, 0x000C001E, 0, 'f', 0.0, 10.0, "SPNR scale factor, moving scene"),
        CciParam("bpr", 0x00030001, 0x00030002, 0, 'e', 0.0, 1.0, "bad pixel replacement 0|1"),
        CciParam("agc-percent-per-bin", 0x00090002, 0x00090001, 0, 'f', 0.0, 100.0, "plateau: max percent of pixels per histogram bin"),
        CciParam("agc-linear-percent", 0x00090004, 0x00090003, 0, 'f', 0.0, 100.0, "0 histogram-shaped .. 100 linear"),
        CciParam("agc-outlier-cut", 0x00090006, 0x00090005, 0, 'f', 0.0, 49.0, "percent of histogram tails ignored"),
        CciParam("agc-max-gain", 0x0009000A, 0x00090009, 0, 'f', 0.25, 8.0, "maximum transfer-function slope"),
        CciParam("agc-df", 0x0009000C, 0x0009000B, 0, 'f', 0.0, 100.0, "damping factor (100 = frozen)"),
        CciParam("agc-gamma", 0x0009000E, 0x0009000D, 0, 'f', 0.5, 4.0, "gamma (<1 more contrast in hot range)"),
        CciParam("agc-detail-headroom", 0x00090014, 0x00090013, 0, 'f', 0.0, 127.0, "8-bit shades reserved for DDE detail"),
        CciParam("agc-d2br", 0x00090016, 0x00090015, 0, 'f', 0.0, 8.0, "DDE detail-to-background ratio"),
        CciParam("agc-sigma-r", 0x00090018, 0x00090017, 0, 'f', 1.0, 8191.0, "DDE smoothing factor (FLIR: keep default)"),
        CciParam("agc-entropy", 0x0009001F, 0x0009001E, 0, 'e', 0.0, 1.0, "1 information-based equalization, 0 plateau"),
        CciParam("stats-mean", 0x001D0007, 0, 0, 'w', 0.0, 0.0, "image mean, counts"),
        CciParam("stats-first-bin", 0x001D0005, 0, 0, 'w', 0.0, 0.0, "lowest populated histogram bin"),
        CciParam("stats-last-bin", 0x001D0006, 0, 0, 'w', 0.0, 0.0, "highest populated histogram bin"),
        CciParam("stats-mean-roi", 0x001D000A, 0, 0, 'w', 0.0, 0.0, "mean inside stats-roi"),
        CciParam("stats-first-bin-roi", 0x001D0008, 0, 0, 'w', 0.0, 0.0, "lowest bin inside stats-roi"),
        CciParam("stats-last-bin-roi", 0x001D0009, 0, 0, 'w', 0.0, 0.0, "highest bin inside stats-roi"),
        CciParam("dvo-analog", 0x00060005, 0x00060004, 0, 'e', 0.0, 1.0, "analog video output 0|1"),
        CciParam("dvo-format", 0x00060007, 0x00060006, 0x0006000C, 'e', 0.0, 2.0, "CMOS output 0 RGB, 1 YCbCr, 2 default (applied)"),
        CciParam("dvo-display-mode", 0x0006000E, 0x0006000D, 0, 'e', 0.0, 1.0, "0 continuous, 1 one-shot"),
        CciParam("sym", 0x00140020, 0x00140000, 0, 'e', 0.0, 1.0, "on-camera symbology overlay 0|1"),
        CciParam("tlinear", 0x003E0002, 0x003E0001, 0, 'e', 0.0, 1.0, "TLinear 0|1 (radiometric only)"),
        CciParam("radiometry", 0x00420001, 0x00420000, 0, 'e', 0.0, 1.0, "radiometry temp-stable 0|1 (radiometric only)"),
        CciParam("spotmeter", 0x00430001, 0x00430000, 0, 'e', 0.0, 1.0, "spot meter 0|1 (radiometric only)"),
    )

    fun find(name: String): CciParam? = all.firstOrNull { it.name == name }
}
