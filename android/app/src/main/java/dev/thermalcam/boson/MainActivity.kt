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
import android.view.SurfaceHolder
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import dev.thermalcam.boson.cci.CciClient
import dev.thermalcam.boson.cci.CciParams
import java.io.File
import java.util.concurrent.ScheduledFuture

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        const val VID = 0x09CB
        const val PID = 0x4007
        const val ACTION_USB_PERMISSION = "dev.thermalcam.boson.USB_PERMISSION"
        const val FPA_PERIOD_MS = 5000L
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
        fpaText = "FPA --"
        myriadText = "Myriad --"
        idText = ""
        pushSettings()
        refreshStatus()
    }

    private class CamState(val id: String, val values: Map<String, Double?>)

    private fun startCci(c: CciClient, poll: Boolean = true) {
        c.enqueue({
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
            NativeBridge.nativeSetFpa(fpa ?: 0)
            runOnUiThread {
                if (cci !== c) return@runOnUiThread
                fpaText = if (fpa != null) "FPA %.1f °C".format(fpa / 10.0) else "FPA --"
                myriadText = if (myr != null) "Myriad %.1f °C".format(myr) else "Myriad --"
                refreshStatus()
            }
        }
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
        if (name != "gain" || cci == null) return // late reply after disconnect
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
            if (gainMode == GAIN_LOW) appendLine("Low gain: overlay shows raw counts (calibration is high gain only)")
            else if (gainMode == GAIN_AUTO) appendLine("Auto gain: temperatures assume high gain (hot scenes may read wrong)")
            append(idText)
        }
    }
}
