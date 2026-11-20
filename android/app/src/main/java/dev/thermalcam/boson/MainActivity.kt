package dev.thermalcam.boson

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Bitmap
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.MediaStore
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowInsets
import android.text.InputType
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import dev.thermalcam.boson.cci.CciClient
import dev.thermalcam.boson.cci.CciParams
import android.os.SystemClock
import android.util.Log
import java.io.File
import kotlin.math.roundToInt
import java.util.concurrent.ScheduledFuture

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        const val VID = 0x09CB
        const val PID = 0x4007
        const val ACTION_USB_PERMISSION = "dev.thermalcam.boson.USB_PERMISSION"
        const val FPA_PERIOD_MS = 5000L
        const val FPA_ALPHA = 0.17           // EMA over the 5 s polls, ~30 s time constant
        const val WARM_WINDOW_MS = 120_000L  // warm when FPA moved <= WARM_SPAN_CX10 over this window
        const val WARM_SPAN_CX10 = 10        // 1.0 degC = camera FFC trigger: <= ~1 auto FFC per 2 min
        const val REQ_CAMERA = 1
        const val GAIN_LOW = 1  // bosonSetGainMode: 0 high, 1 low, 2 auto
        const val GAIN_AUTO = 2
    }

    private lateinit var usb: UsbManager
    private lateinit var root: LinearLayout
    private lateinit var thermalView: ThermalSurfaceView
    private lateinit var mediaColumn: LinearLayout
    private lateinit var recordButton: Button
    private val recorder by lazy { VideoRecorder(this) }
    private lateinit var controlsScroll: ScrollView
    private lateinit var statusText: TextView
    private lateinit var paletteGroup: RadioGroup
    private lateinit var unitGroup: RadioGroup
    private lateinit var spotSwitch: Switch
    private lateinit var nrSwitch: Switch
    private lateinit var gainGroup: RadioGroup
    private lateinit var ffcModeGroup: RadioGroup
    private lateinit var tfSwitch: Switch
    private lateinit var scnrSwitch: Switch
    private lateinit var spnrSwitch: Switch

    private var conn: UsbDeviceConnection? = null
    private var device: UsbDevice? = null
    private var cci: CciClient? = null
    private var fpaTask: ScheduledFuture<*>? = null
    private var started = false
    private var updatingUi = false
    private var settingsDialog: CameraSettingsDialog? = null
    private var gainMode: Int? = null // camera gain mode from CCI, null = unknown
    private val prefs by lazy { getSharedPreferences("camera", MODE_PRIVATE) } // camera settings to re-apply on connect
    private val fpaHist = ArrayDeque<Pair<Long, Int>>() // (time ms, FPA cx10) during warm-up
    private var fpaSmooth: Double? = null
    private var warm = false
    private val display by lazy { getSharedPreferences("display", MODE_PRIVATE) } // palette, units, spot...
    private val paletteIds by lazy { listOf(R.id.palette_iron, R.id.palette_white, R.id.palette_black) }
    private val unitIds by lazy { listOf(R.id.unit_c, R.id.unit_f, R.id.unit_k, R.id.unit_raw) }
    private var fullscreen = false

    // status readout parts
    private var fpaText = "FPA --"
    private var myriadText = "Myriad --"
    private var idText = ""
    private var message = "Plug in the Boson (USB-OTG)"

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION ->
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        if (started) connect()
                    } else {
                        setMessage("USB permission denied")
                    }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    @Suppress("DEPRECATION")
                    val d = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                    if (d != null && d.deviceName == device?.deviceName) {
                        disconnect()
                        setMessage("Camera detached")
                    }
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        usb = getSystemService(UsbManager::class.java)
        root = findViewById(R.id.root)
        thermalView = findViewById(R.id.thermal_view)
        mediaColumn = findViewById(R.id.media_column)
        recordButton = findViewById(R.id.record_button)
        controlsScroll = findViewById(R.id.controls_scroll)
        statusText = findViewById(R.id.status_text)
        paletteGroup = findViewById(R.id.palette_group)
        unitGroup = findViewById(R.id.unit_group)
        spotSwitch = findViewById(R.id.spot_switch)
        nrSwitch = findViewById(R.id.nr_switch)
        gainGroup = findViewById(R.id.gain_group)
        ffcModeGroup = findViewById(R.id.ffc_mode_group)
        tfSwitch = findViewById(R.id.tf_switch)
        scnrSwitch = findViewById(R.id.scnr_switch)
        spnrSwitch = findViewById(R.id.spnr_switch)

        thermalView.holder.addCallback(this)
        applyOrientation(resources.configuration)

        restoreDisplay()
        setupImageGestures()

        // display settings: native only
        paletteGroup.setOnCheckedChangeListener { _, _ -> pushSettings() }
        unitGroup.setOnCheckedChangeListener { _, _ -> pushSettings() }
        spotSwitch.setOnCheckedChangeListener { _, _ -> pushSettings() }
        nrSwitch.setOnCheckedChangeListener { _, _ -> pushSettings() }

        // camera settings: CCI
        gainGroup.setOnCheckedChangeListener { _, id ->
            val v = when (id) { R.id.gain_high -> 0.0; R.id.gain_low -> 1.0; R.id.gain_auto -> 2.0; else -> return@setOnCheckedChangeListener }
            setCameraParam("gain", v)
        }
        ffcModeGroup.setOnCheckedChangeListener { _, id ->
            val v = when (id) { R.id.ffc_manual -> 0.0; R.id.ffc_auto -> 1.0; else -> return@setOnCheckedChangeListener }
            setCameraParam("ffc-mode", v)
        }
        tfSwitch.setOnCheckedChangeListener { _, on -> setCameraParam("tf", if (on) 1.0 else 0.0) }
        scnrSwitch.setOnCheckedChangeListener { _, on -> setCameraParam("scnr", if (on) 1.0 else 0.0) }
        spnrSwitch.setOnCheckedChangeListener { _, on -> setCameraParam("spnr", if (on) 1.0 else 0.0) }

        findViewById<Button>(R.id.ffc_button).setOnClickListener {
            val c = cci ?: return@setOnClickListener setMessage("CCI not connected")
            c.enqueue({ c.runFfc() to c.lastStatus }) { (ok, st) ->
                setMessage(if (ok) "FFC done" else "FFC failed (status 0x%08X)".format(st))
            }
        }
        findViewById<Button>(R.id.calibrate_button).setOnClickListener { calibrateDialog() }
        findViewById<Button>(R.id.save_button).setOnClickListener { confirmSave() }
        findViewById<Button>(R.id.all_settings_button).setOnClickListener {
            val c = cci ?: return@setOnClickListener setMessage("CCI not connected")
            settingsDialog = CameraSettingsDialog(this, c) { n, v ->
                if (cci === c) { onParamSet(n, v); startCci(c, poll = false) } // refresh main controls
            }
                .also { it.show() }
        }
        // retry after a failed start (connect() is a no-op while connected)
        statusText.setOnClickListener { if (started) connect() }
        findViewById<Button>(R.id.photo_button).setOnClickListener { snapshot() }
        recordButton.setOnClickListener { if (recorder.isRecording) stopRecording() else startRecording() }

        val filter = IntentFilter(ACTION_USB_PERMISSION).apply { addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(usbReceiver, filter)
        pushSettings()
    }

    override fun onStart() {
        super.onStart()
        started = true
        connect()
    }

    override fun onStop() {
        started = false
        stopRecording()
        disconnect()
        super.onStop()
    }

    override fun onDestroy() {
        unregisterReceiver(usbReceiver)
        super.onDestroy()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (started) connect() // USB_DEVICE_ATTACHED while running (singleTop)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_CAMERA) return
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
            if (started) connect()
        } else {
            setMessage("Camera permission denied: USB video cannot be opened")
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        applyOrientation(newConfig)
    }

    // --- SurfaceHolder.Callback ---
    override fun surfaceCreated(holder: SurfaceHolder) = NativeBridge.nativeSetSurface(holder.surface)
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) = NativeBridge.nativeSetSurface(null)

    /**
     * Portrait: image, capture buttons, then controls. Landscape: image with the capture buttons
     * under it on the left (2/3), controls on the right (1/3).
     */
    private fun applyOrientation(cfg: Configuration) {
        val match = LinearLayout.LayoutParams.MATCH_PARENT
        val wrap = LinearLayout.LayoutParams.WRAP_CONTENT
        if (cfg.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            root.orientation = LinearLayout.HORIZONTAL
            mediaColumn.layoutParams = LinearLayout.LayoutParams(0, match, 2f)
            // image takes what the buttons leave (it keeps 740:512 inside that space)
            thermalView.layoutParams = LinearLayout.LayoutParams(match, 0, 1f)
            controlsScroll.layoutParams = LinearLayout.LayoutParams(0, match, 1f)
        } else {
            root.orientation = LinearLayout.VERTICAL
            mediaColumn.layoutParams = LinearLayout.LayoutParams(match, wrap, 0f)
            thermalView.layoutParams = LinearLayout.LayoutParams(match, wrap, 0f)
            controlsScroll.layoutParams = LinearLayout.LayoutParams(match, 0, 1f)
        }
    }

    private fun startRecording() {
        try {
            recorder.start()
            recordButton.text = "■ Stop recording"
            setMessage("Recording ${recorder.label}")
        } catch (e: Exception) {
            setMessage("Recording failed: ${e.message}")
        }
    }

    private fun stopRecording() {
        if (!recorder.isRecording) return
        val saved = recorder.stop()
        recordButton.text = "● Record video"
        setMessage(if (saved != null) "Saved $saved" else "Recording discarded (no frames)")
        saved?.let { Toast.makeText(this, "Saved $it", Toast.LENGTH_SHORT).show() }
    }

    private fun pushSettings() {
        val lut = when (paletteGroup.checkedRadioButtonId) { R.id.palette_white -> 0; R.id.palette_black -> 1; else -> 4 }
        // FLIR_CAL_* was measured in high gain: in low gain show raw counts instead of wrong temps.
        // Auto (power-up default) stays in high gain for normal scenes, so units stay available.
        val unit = if (gainMode == GAIN_LOW) 0
            else when (unitGroup.checkedRadioButtonId) { R.id.unit_c -> 1; R.id.unit_f -> 2; R.id.unit_k -> 3; else -> 0 }
        NativeBridge.nativeSetSettings(lut, unit, spotSwitch.isChecked, nrSwitch.isChecked)
        display.edit()
            .putInt("palette", paletteIds.indexOf(paletteGroup.checkedRadioButtonId))
            .putInt("unit", unitIds.indexOf(unitGroup.checkedRadioButtonId))
            .putBoolean("spot", spotSwitch.isChecked)
            .putBoolean("nr", nrSwitch.isChecked)
            .apply()
    }

    /** Display settings from the last run (before the listeners are attached). */
    private fun restoreDisplay() {
        paletteIds.getOrNull(display.getInt("palette", -1))?.let { paletteGroup.check(it) }
        unitIds.getOrNull(display.getInt("unit", -1))?.let { unitGroup.check(it) }
        spotSwitch.isChecked = display.getBoolean("spot", spotSwitch.isChecked)
        nrSwitch.isChecked = display.getBoolean("nr", nrSwitch.isChecked)
        NativeBridge.nativeSetSpot(display.getInt("spotX", NativeBridge.SENSOR_WIDTH / 2),
            display.getInt("spotY", NativeBridge.OUT_HEIGHT / 2))
        NativeBridge.nativeSetCalOffset(display.getFloat("calOffset", 0f))
    }

    /** Put the cross on something of known temperature, type it in: the offset applies to every reading. */
    private fun calibrateDialog() {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
            hint = "e.g. 34 (skin), or a thermometer reading"
        }
        AlertDialog.Builder(this)
            .setTitle("Calibrate at the spot")
            .setMessage("True temperature under the cross, in °C. Current offset: %+.1f °C".format(display.getFloat("calOffset", 0f)))
            .setView(input)
            .setNegativeButton(android.R.string.cancel, null)
            .setNeutralButton("Reset") { _, _ -> setCalOffset(0f) }
            .setPositiveButton("Set") { _, _ ->
                val t = input.text.toString().replace(',', '.').toFloatOrNull()
                    ?: return@setPositiveButton setMessage("Calibration: not a number")
                val off = NativeBridge.nativeCalibrate(t)
                if (off.isNaN()) setMessage("Calibration needs a live image and the camera temperature (CCI)")
                else setCalOffset(off)
            }
            .show()
    }

    private fun setCalOffset(off: Float) {
        NativeBridge.nativeSetCalOffset(off)
        display.edit().putFloat("calOffset", off).apply()
        setMessage(if (off == 0f) "Calibration reset" else "Calibrated: offset %+.1f °C".format(off))
    }

    /** Tap: move the spot meter there. Long press: spot back to the centre. Double tap: full screen. */
    @Suppress("ClickableViewAccessibility")
    private fun setupImageGestures() {
        val gd = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                val x = (e.x * NativeBridge.OUT_WIDTH / thermalView.width).toInt()
                val y = (e.y * NativeBridge.OUT_HEIGHT / thermalView.height).toInt()
                if (x < NativeBridge.SENSOR_WIDTH) placeSpot(x, y) // taps on the colour bar do nothing
                return true
            }
            override fun onLongPress(e: MotionEvent) = placeSpot(NativeBridge.SENSOR_WIDTH / 2, NativeBridge.OUT_HEIGHT / 2)
            override fun onDoubleTap(e: MotionEvent): Boolean { setFullscreen(!fullscreen); return true }
        })
        thermalView.setOnTouchListener { _, e -> gd.onTouchEvent(e) }
    }

    private fun placeSpot(x: Int, y: Int) {
        NativeBridge.nativeSetSpot(x, y)
        display.edit().putInt("spotX", x).putInt("spotY", y).apply()
        if (!spotSwitch.isChecked) spotSwitch.isChecked = true // listener pushes + saves
    }

    private fun setFullscreen(on: Boolean) {
        fullscreen = on
        val vis = if (on) View.GONE else View.VISIBLE
        controlsScroll.visibility = vis
        findViewById<View>(R.id.capture_row).visibility = vis
        if (Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let { if (on) it.hide(WindowInsets.Type.systemBars()) else it.show(WindowInsets.Type.systemBars()) }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = if (on) View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY else 0
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (fullscreen) setFullscreen(false) else @Suppress("DEPRECATION") super.onBackPressed()
    }

    private fun connect() {
        if (conn != null) return
        val dev = usb.deviceList.values.firstOrNull { it.vendorId == VID && it.productId == PID }
            ?: return setMessage("Plug in the Boson (USB-OTG)")
        // Android 9+: USB permission for a UVC (video class) device needs the CAMERA permission first
        if (checkSelfPermission(android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(android.Manifest.permission.CAMERA), REQ_CAMERA)
            return setMessage("Camera permission is required to open a USB camera")
        }
        if (!usb.hasPermission(dev)) {
            val pi = PendingIntent.getBroadcast(this, 0,
                Intent(ACTION_USB_PERMISSION).setPackage(packageName), PendingIntent.FLAG_MUTABLE)
            usb.requestPermission(dev, pi)
            return setMessage("Waiting for USB permission")
        }
        val c = usb.openDevice(dev) ?: return setMessage("Cannot open USB device")
        conn = c
        device = dev
        val r = NativeBridge.nativeStart(c.fileDescriptor)
        if (r != 0) {
            disconnect() // closes the connection so the next connect() retries
            return setMessage("Video start failed ($r). Tap here to retry")
        }
        setMessage("Streaming RAW16")
        cci = try { CciClient(c, dev) } catch (e: Exception) { setMessage("CCI unavailable: ${e.message}"); null }
        cci?.let { startCci(it) }
    }

    private fun disconnect() {
        stopRecording()
        settingsDialog?.dismiss()
        settingsDialog = null
        fpaTask?.cancel(false)
        fpaTask = null
        cci?.close()
        cci = null
        NativeBridge.nativeStop()
        conn?.close()
        conn = null
        device = null
        NativeBridge.nativeSetFpa(0)
        gainMode = null
        fpaHist.clear()
        fpaSmooth = null
        warm = false
        fpaText = "FPA --"
        myriadText = "Myriad --"
        idText = ""
        pushSettings()
        refreshStatus()
    }

    private class CamState(val id: String, val values: Map<String, Double?>)

    private fun startCci(c: CciClient, poll: Boolean = true) {
        // the camera powers up with its defaults: re-apply what the user set last time (poll = fresh connect)
        val saved = if (poll) prefs.all.mapNotNull { (n, v) -> CciParams.find(n)?.let { it to (v as Float).toDouble() } } else emptyList()
        c.enqueue({
            saved.forEach { (p, v) -> if (!c.setParam(p, v)) Log.w("BosonApp", "re-apply ${p.name}=$v failed") }
            val id = "fw ${c.softwareRev() ?: "?"} · SN ${c.serialNumber() ?: "?"} · PN ${c.partNumber() ?: "?"}"
            CamState(id, listOf("gain", "ffc-mode", "tf", "scnr", "spnr").associateWith { n -> c.getParam(CciParams.find(n)!!) })
        }) { s ->
            if (cci !== c) return@enqueue
            idText = s.id
            updatingUi = true
            when (s.values["gain"]?.toInt()) { 0 -> gainGroup.check(R.id.gain_high); 1 -> gainGroup.check(R.id.gain_low); 2 -> gainGroup.check(R.id.gain_auto) }
            when (s.values["ffc-mode"]?.toInt()) { 0 -> ffcModeGroup.check(R.id.ffc_manual); 1 -> ffcModeGroup.check(R.id.ffc_auto) }
            s.values["tf"]?.let { tfSwitch.isChecked = it != 0.0 }
            s.values["scnr"]?.let { scnrSwitch.isChecked = it != 0.0 }
            s.values["spnr"]?.let { spnrSwitch.isChecked = it != 0.0 }
            updatingUi = false
            gainMode = s.values["gain"]?.toInt()
            pushSettings()
            refreshStatus()
        }
        if (!poll) return
        val myriad = CciParams.find("myriad-temp")!!
        fpaTask = c.every(FPA_PERIOD_MS) {
            val fpa = c.fpaTempCx10()
            val myr = c.getParam(myriad)
            runOnUiThread {
                if (cci !== c) return@runOnUiThread
                onFpa(c, fpa)
                fpaText = if (fpa != null) "FPA %.1f °C".format(fpa / 10.0) else "FPA --"
                myriadText = if (myr != null) "Myriad %.1f °C".format(myr) else "Myriad --"
                refreshStatus()
            }
        }
    }

    /** Smooth the FPA reference, and detect the end of warm-up (then one FFC). UI thread. */
    private fun onFpa(c: CciClient, fpa: Int?) {
        if (fpa == null) {
            fpaSmooth = null
            NativeBridge.nativeSetFpa(0) // labels fall back to raw counts
            return
        }
        val s = fpaSmooth?.let { it + (fpa - it) * FPA_ALPHA } ?: fpa.toDouble()
        fpaSmooth = s
        NativeBridge.nativeSetFpa(s.roundToInt())
        if (warm) return
        val now = SystemClock.elapsedRealtime()
        fpaHist.addLast(now to fpa)
        while (now - fpaHist.first().first > WARM_WINDOW_MS) fpaHist.removeFirst()
        val span = fpaHist.maxOf { it.second } - fpaHist.minOf { it.second }
        if (now - fpaHist.first().first < WARM_WINDOW_MS - FPA_PERIOD_MS || span > WARM_SPAN_CX10) return
        warm = true
        fpaHist.clear()
        c.enqueue({ c.runFfc() }) { ok -> setMessage(if (ok) "Warmed up: FFC done, temperatures stable" else "Warmed up (FFC failed)") }
        refreshStatus()
    }

    private fun setCameraParam(name: String, v: Double) {
        if (updatingUi) return
        val c = cci ?: return setMessage("CCI not connected")
        val p = CciParams.find(name)!!
        c.enqueue({ c.setParam(p, v) to c.lastStatus }) { (ok, st) ->
            if (cci !== c) return@enqueue
            if (ok) onParamSet(name, v) else startCci(c, poll = false) // re-sync controls
            setMessage(if (ok) "$name = ${v.toInt()}" else "$name failed (status 0x%08X)".format(st))
        }
    }

    private fun onParamSet(name: String, v: Double) {
        if (cci == null) return // late reply after disconnect
        if (name != "freeze") prefs.edit().putFloat(name, v.toFloat()).apply() // never come up frozen
        if (name != "gain") return
        gainMode = v.toInt()
        pushSettings()
        refreshStatus()
    }

    private fun confirmSave() {
        val c = cci ?: return setMessage("CCI not connected")
        AlertDialog.Builder(this)
            .setTitle("Save settings to camera?")
            .setMessage("Writes the current camera settings to flash as power-up defaults.")
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton("Save") { _, _ ->
                c.enqueue({ c.saveSettings() to c.lastStatus }) { (ok, st) ->
                    setMessage(if (ok) "Settings saved" else "Save failed (status 0x%08X)".format(st))
                }
            }
            .show()
    }

    private fun snapshot() {
        val bmp = Bitmap.createBitmap(NativeBridge.OUT_WIDTH, NativeBridge.OUT_HEIGHT, Bitmap.Config.ARGB_8888)
        if (!NativeBridge.nativeSnapshot(bmp)) return setMessage("No frame yet")
        val name = "boson_${System.currentTimeMillis()}.png"
        Thread {
            val msg = try {
                if (Build.VERSION.SDK_INT >= 29) {
                    val values = ContentValues().apply {
                        put(MediaStore.Images.Media.DISPLAY_NAME, name)
                        put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                        put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/BosonThermal")
                    }
                    val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                        ?: throw IllegalStateException("MediaStore insert failed")
                    contentResolver.openOutputStream(uri)!!.use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
                    "Saved Pictures/BosonThermal/$name"
                } else {
                    // API 26-28: app-specific Pictures dir, no storage permission needed
                    val f = File(getExternalFilesDir(Environment.DIRECTORY_PICTURES), name)
                    f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
                    "Saved ${f.absolutePath}"
                }
            } catch (e: Exception) {
                "Snapshot failed: ${e.message}"
            }
            runOnUiThread { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show() }
        }.start()
    }

    private fun setMessage(m: String) {
        message = m
        refreshStatus()
    }

    private fun refreshStatus() {
        statusText.text = buildString {
            appendLine(message)
            appendLine("$fpaText · $myriadText")
            if (cci != null && !warm) appendLine("Warming up: temperatures approximate")
            if (gainMode == GAIN_LOW) appendLine("Low gain: overlay shows raw counts (calibration is high gain only)")
            else if (gainMode == GAIN_AUTO) appendLine("Auto gain: temperatures assume high gain (hot scenes may read wrong)")
            display.getFloat("calOffset", 0f).let { if (it != 0f) appendLine("Calibration offset %+.1f °C".format(it)) }
            appendLine(idText)
            append("Image: tap = move spot · long-press = centre spot · double-tap = full screen")
        }
    }
}
