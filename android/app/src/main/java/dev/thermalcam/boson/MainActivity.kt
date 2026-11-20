package dev.thermalcam.boson

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.MediaStore
import android.provider.Settings
import android.text.InputType
import android.util.Log
import android.util.TypedValue
import android.view.GestureDetector
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.accessibility.AccessibilityNodeInfo
import android.view.animation.AlphaAnimation
import android.view.animation.Animation
import android.widget.Button
import android.widget.Chronometer
import android.widget.EditText
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import dev.thermalcam.boson.cci.CciClient
import dev.thermalcam.boson.cci.CciParams
import java.io.File
import java.text.DateFormat
import java.util.Date
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import kotlin.math.min
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        /** nativeStart / nativeStop / CciClient.close block. Process-wide, one thread: a start and a stop
         *  never overlap, even across activity instances (native stop_all() works on unlocked globals). */
        private val usbExec = Executors.newSingleThreadExecutor()
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

        // message pill kinds
        const val MSG_INFO = 0
        const val MSG_SUCCESS = 1
        const val MSG_WARNING = 2
        const val MSG_ERROR = 3

        // empty states over the image (null = live image)
        const val E_NO_DEVICE = 0
        const val E_CAM_PERMISSION = 1
        const val E_CAM_DENIED = 2
        const val E_WAIT_USB = 3
        const val E_USB_DENIED = 4
        const val E_CONNECTING = 5
        const val E_START_FAILED = 6
        const val E_DETACHED = 7
        const val E_OPEN_FAILED = 8

        /** Native enum flir_lut per palette segment, in [paletteIds] order (iron, white, black, rainbow, lava, arctic, hot spot). */
        val PALETTE_LUTS = intArrayOf(4, 0, 1, 2, 5, 6, 9)

        private const val DISABLED_ALPHA = 0.38f
        private val MSG_TOKEN = Any()
    }

    private lateinit var usb: UsbManager
    private val root by lazy { findViewById<LinearLayout>(R.id.root) }
    private val thermalView by lazy { findViewById<ThermalSurfaceView>(R.id.thermal_view) }
    private val mediaColumn by lazy { findViewById<LinearLayout>(R.id.media_column) }
    private val imageFrame by lazy { findViewById<View>(R.id.image_frame) }
    private val statusStrip by lazy { findViewById<View>(R.id.status_strip) }
    private val rail by lazy { findViewById<LinearLayout>(R.id.action_rail) }
    private val controlsScroll by lazy { findViewById<ScrollView>(R.id.controls_scroll) }
    private val btnFullscreen by lazy { findViewById<Button>(R.id.btn_fullscreen) }
    private val btnFfc by lazy { findViewById<Button>(R.id.btn_ffc) }
    private val btnPanel by lazy { findViewById<Button>(R.id.btn_panel) }
    private val calibrateButton by lazy { findViewById<Button>(R.id.calibrate_button) }
    private val photoButton by lazy { findViewById<ImageButton>(R.id.photo_button) }
    private val recordButton by lazy { findViewById<ImageButton>(R.id.record_button) }
    private val railSpace1 by lazy { findViewById<View>(R.id.rail_space_1) }
    private val railSpace2 by lazy { findViewById<View>(R.id.rail_space_2) }
    private val chipState by lazy { findViewById<TextView>(R.id.chip_state) }
    private val chipRec by lazy { findViewById<View>(R.id.chip_rec) }
    private val chipWarm by lazy { findViewById<View>(R.id.chip_warm) }
    private val chipGain by lazy { findViewById<TextView>(R.id.chip_gain) }
    private val chipCal by lazy { findViewById<TextView>(R.id.chip_cal) }
    private val readoutText by lazy { findViewById<TextView>(R.id.readout_text) }
    private val recTimes by lazy { listOf(findViewById<Chronometer>(R.id.rec_time), findViewById(R.id.rec_badge_time)) }
    private val recDots by lazy { listOf(findViewById<View>(R.id.rec_dot), findViewById(R.id.rec_badge_dot)) }
    private val recBadge by lazy { findViewById<View>(R.id.rec_badge) }
    private val exitFullscreen by lazy { findViewById<View>(R.id.exit_fullscreen) }
    private val emptyState by lazy { findViewById<View>(R.id.empty_state) }
    private val emptyIcon by lazy { findViewById<ImageView>(R.id.empty_icon) }
    private val emptyProgress by lazy { findViewById<ProgressBar>(R.id.empty_progress) }
    private val emptyTitle by lazy { findViewById<TextView>(R.id.empty_title) }
    private val emptyBody by lazy { findViewById<TextView>(R.id.empty_body) }
    private val emptyAction by lazy { findViewById<Button>(R.id.empty_action) }
    private val messagePill by lazy { findViewById<View>(R.id.message_pill) }
    private val messageDot by lazy { findViewById<View>(R.id.message_dot) }
    private val messageText by lazy { findViewById<TextView>(R.id.message_text) }
    private val paletteGroup by lazy { findViewById<RadioGroup>(R.id.palette_group) }
    private val unitGroup by lazy { findViewById<RadioGroup>(R.id.unit_group) }
    private val spotSwitch by lazy { findViewById<Switch>(R.id.spot_switch) }
    private val nrSwitch by lazy { findViewById<Switch>(R.id.nr_switch) }
    private val minmaxSwitch by lazy { findViewById<Switch>(R.id.minmax_switch) }
    private val messageOpen by lazy { findViewById<Button>(R.id.message_open) }
    private val messageShare by lazy { findViewById<Button>(R.id.message_share) }
    private val gainGroup by lazy { findViewById<RadioGroup>(R.id.gain_group) }
    private val gainHelper by lazy { findViewById<TextView>(R.id.gain_helper) }
    private val controlUnavailable by lazy { findViewById<View>(R.id.control_unavailable) }
    private val ffcModeGroup by lazy { findViewById<RadioGroup>(R.id.ffc_mode_group) }
    private val tfSwitch by lazy { findViewById<Switch>(R.id.tf_switch) }
    private val scnrSwitch by lazy { findViewById<Switch>(R.id.scnr_switch) }
    private val spnrSwitch by lazy { findViewById<Switch>(R.id.spnr_switch) }
    private val cameraSections by lazy { listOf(R.id.section_camera, R.id.section_filters, R.id.section_advanced).map { findViewById<View>(it) } }
    private val infoFw by lazy { findViewById<TextView>(R.id.info_fw) }
    private val infoSn by lazy { findViewById<TextView>(R.id.info_sn) }
    private val infoPn by lazy { findViewById<TextView>(R.id.info_pn) }
    private val infoFpa by lazy { findViewById<TextView>(R.id.info_fpa) }
    private val infoMyriad by lazy { findViewById<TextView>(R.id.info_myriad) }

    private val recorder by lazy { VideoRecorder(this) { reason -> stopRecording(reason) } }
    private val handler = Handler(Looper.getMainLooper())
    private var permissionPending = false // USB permission dialog up: don't ask twice
    private var backCallback: Any? = null // OnBackInvokedCallback while full screen (API 33+)
    private var capture: Pair<Uri, String>? = null // last saved photo/video (MediaStore Uri, MIME) for Open/Share

    private var conn: UsbDeviceConnection? = null
    private var device: UsbDevice? = null
    private var cci: CciClient? = null
    private var fpaTask: ScheduledFuture<*>? = null
    private var started = false
    private var streaming = false
    private var updatingUi = false
    private var settingsDialog: CameraSettingsDialog? = null
    private var gainMode: Int? = null // camera gain mode from CCI, null = unknown
    private val prefs by lazy { getSharedPreferences("camera", MODE_PRIVATE) } // camera settings to re-apply on connect
    private val fpaHist = ArrayDeque<Pair<Long, Int>>() // (time ms, FPA cx10) during warm-up
    private var fpaSmooth: Double? = null
    private var fpaC: Double? = null
    private var myriadC: Double? = null
    private var warm = false
    private val display by lazy { getSharedPreferences("display", MODE_PRIVATE) } // palette, units, spot...
    private val paletteIds by lazy {
        listOf(R.id.palette_iron, R.id.palette_white, R.id.palette_black, R.id.palette_rainbow,
            R.id.palette_lava, R.id.palette_arctic, R.id.palette_hotspot)
    }
    private val unitIds by lazy { listOf(R.id.unit_c, R.id.unit_f, R.id.unit_k, R.id.unit_raw) }
    private var fullscreen = false
    private var empty: Int? = null

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION ->
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false).also { permissionPending = false }) {
                        if (started) connect()
                    } else {
                        showEmpty(E_USB_DENIED)
                    }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    @Suppress("DEPRECATION")
                    val d = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                    if (d != null && d.deviceName == device?.deviceName) {
                        disconnect()
                        showEmpty(E_DETACHED)
                    }
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        usb = getSystemService(UsbManager::class.java)

        thermalView.holder.addCallback(this)
        recTimes.forEach { it.format = getString(R.string.rec_format) }
        applyOrientation(resources.configuration)

        restoreDisplay()
        setupImageGestures()
        setupAccessibility()

        // display settings: native only
        paletteGroup.setOnCheckedChangeListener { _, _ -> pushSettings() }
        unitGroup.setOnCheckedChangeListener { _, _ -> pushSettings() }
        spotSwitch.setOnCheckedChangeListener { _, _ -> pushSettings() }
        nrSwitch.setOnCheckedChangeListener { _, _ -> pushSettings() }
        minmaxSwitch.setOnCheckedChangeListener { _, _ -> pushSettings() }
        messageOpen.setOnClickListener { openCapture(share = false) }
        messageShare.setOnClickListener { openCapture(share = true) }

        // camera settings: CCI
        gainGroup.setOnCheckedChangeListener { _, id ->
            val v = when (id) { R.id.gain_high -> 0.0; R.id.gain_low -> 1.0; R.id.gain_auto -> 2.0; else -> return@setOnCheckedChangeListener }
            setCameraParam("gain", v, getString(R.string.label_gain), findViewById<RadioButton>(id).text)
        }
        ffcModeGroup.setOnCheckedChangeListener { _, id ->
            val v = when (id) { R.id.ffc_manual -> 0.0; R.id.ffc_auto -> 1.0; else -> return@setOnCheckedChangeListener }
            setCameraParam("ffc-mode", v, getString(R.string.label_ffc_mode), findViewById<RadioButton>(id).text)
        }
        tfSwitch.setOnCheckedChangeListener { _, on -> setCameraParam("tf", if (on) 1.0 else 0.0, getString(R.string.switch_tf), onOff(on)) }
        scnrSwitch.setOnCheckedChangeListener { _, on -> setCameraParam("scnr", if (on) 1.0 else 0.0, getString(R.string.switch_scnr), onOff(on)) }
        spnrSwitch.setOnCheckedChangeListener { _, on -> setCameraParam("spnr", if (on) 1.0 else 0.0, getString(R.string.switch_spnr), onOff(on)) }
        // subtitles are hidden from TalkBack: read them with the switch
        listOf(spotSwitch to R.string.switch_spot_sub, nrSwitch to R.string.switch_nr_sub,
            minmaxSwitch to R.string.minmax_markers_sub, tfSwitch to R.string.switch_tf_sub,
            scnrSwitch to R.string.switch_scnr_sub, spnrSwitch to R.string.switch_spnr_sub)
            .forEach { (sw, sub) -> sw.contentDescription = "${sw.text}. ${getString(sub)}" }

        btnFfc.setOnClickListener { v ->
            haptic(v)
            val c = cci ?: return@setOnClickListener showMessage(getString(R.string.msg_cci_offline), MSG_WARNING)
            c.enqueue({ c.runFfc() to c.lastStatus }) { (ok, st) ->
                if (ok) showMessage(getString(R.string.msg_ffc_done), MSG_SUCCESS)
                else showMessage(getString(R.string.msg_ffc_failed, st), MSG_ERROR)
            }
        }
        calibrateButton.setOnClickListener { calibrateDialog() }
        chipCal.setOnClickListener { calibrateDialog() }
        btnFullscreen.setOnClickListener { setFullscreen(true) }
        exitFullscreen.setOnClickListener { setFullscreen(false) }
        btnPanel.setOnClickListener {
            display.edit().putBoolean("panel", !display.getBoolean("panel", true)).apply()
            applyOrientation(resources.configuration)
        }
        findViewById<View>(R.id.save_button).setOnClickListener { confirmSave() }
        findViewById<View>(R.id.forget_overrides_button).setOnClickListener { confirmForget() }
        findViewById<View>(R.id.all_settings_button).setOnClickListener {
            val c = cci ?: return@setOnClickListener showMessage(getString(R.string.msg_cci_offline), MSG_WARNING)
            settingsDialog = CameraSettingsDialog(this, c,
                onSet = { n, v -> if (cci === c) { onParamSet(n, v); startCci(c, poll = false) } }, // refresh main controls
                onMessage = { text, ok -> showMessage(text, if (ok) MSG_SUCCESS else MSG_ERROR) },
            ).also { it.show() }
        }
        emptyAction.setOnClickListener { onEmptyAction() }
        photoButton.setOnClickListener { haptic(it); snapshot() }
        recordButton.setOnClickListener { haptic(it); if (recorder.isRecording) stopRecording() else startRecording() }

        val filter = IntentFilter(ACTION_USB_PERMISSION).apply { addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(usbReceiver, filter)
        pushSettings()
        showEmpty(E_NO_DEVICE)
        updateChips()
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
        handler.removeCallbacksAndMessages(null)
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
            // no rationale after a denial = "don't ask again": only the settings screen can grant it
            showEmpty(if (shouldShowRequestPermissionRationale(android.Manifest.permission.CAMERA)) E_CAM_PERMISSION else E_CAM_DENIED)
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

    private fun dp(id: Int) = resources.getDimensionPixelSize(id)

    private fun haptic(v: View) = v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)

    /**
     * Landscape: [status strip / image] | vertical rail | panel (min(320dp, 40 %), can be hidden).
     * Portrait: status strip / image / horizontal rail / panel. One layout: the rail children are
     * re-added in the order of the orientation.
     */
    private fun applyOrientation(cfg: Configuration) {
        val match = ViewGroup.LayoutParams.MATCH_PARENT
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT
        val land = cfg.orientation == Configuration.ORIENTATION_LANDSCAPE
        val fill = land || fullscreen // image column takes all the space left
        root.orientation = if (land) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
        mediaColumn.layoutParams = if (land) LinearLayout.LayoutParams(0, match, 1f)
            else LinearLayout.LayoutParams(match, if (fill) 0 else wrap, if (fill) 1f else 0f)
        imageFrame.layoutParams = LinearLayout.LayoutParams(match, if (fill) 0 else wrap, if (fill) 1f else 0f)

        val railW = dp(R.dimen.rail_w)
        rail.orientation = if (land) LinearLayout.VERTICAL else LinearLayout.HORIZONTAL
        rail.layoutParams = if (land) LinearLayout.LayoutParams(railW, match) else LinearLayout.LayoutParams(match, railW)
        // resolve every lazy rail view before removeAllViews(): a detached view is not found by findViewById
        // (btnPanel is not in the portrait order, so its first lookup would return null and crash)
        val all = listOf(btnFullscreen, btnFfc, railSpace1, photoButton, recordButton, railSpace2, calibrateButton, btnPanel)
        val order = if (land) all
            else listOf(btnFfc, calibrateButton, railSpace1, photoButton, recordButton, railSpace2, btnFullscreen)
        rail.removeAllViews()
        val shutter = dp(R.dimen.shutter)
        val railButton = dp(R.dimen.rail_button)
        val gap = dp(R.dimen.space_xs)
        order.forEach { v ->
            val lp = when (v) {
                railSpace1, railSpace2 -> LinearLayout.LayoutParams(0, 0, 1f)
                photoButton, recordButton -> LinearLayout.LayoutParams(shutter, shutter).apply { setMargins(gap, gap, gap, gap) }
                else -> if (land) LinearLayout.LayoutParams(match, railButton) else LinearLayout.LayoutParams(railW, match)
            }
            rail.addView(v, lp)
        }

        val panelShown = !land || display.getBoolean("panel", true)
        btnPanel.isActivated = panelShown
        btnPanel.contentDescription = getString(if (panelShown) R.string.cd_hide_panel else R.string.cd_show_panel)
        val panelW = min(dp(R.dimen.panel_w), (cfg.screenWidthDp * resources.displayMetrics.density * 0.4f).toInt())
        controlsScroll.layoutParams = if (land) LinearLayout.LayoutParams(panelW, match) else LinearLayout.LayoutParams(match, 0, 1f)
        controlsScroll.visibility = if (panelShown && !fullscreen) View.VISIBLE else View.GONE
        rail.visibility = if (fullscreen) View.GONE else View.VISIBLE
        statusStrip.visibility = if (fullscreen) View.GONE else View.VISIBLE
    }

    private fun startRecording() {
        try {
            recorder.start()
        } catch (e: Exception) {
            return showMessage(getString(R.string.msg_rec_failed, e.message ?: e.javaClass.simpleName), MSG_ERROR)
        }
        recordButton.isActivated = true
        recordButton.contentDescription = getString(R.string.cd_record_stop)
        val base = SystemClock.elapsedRealtime()
        recTimes.forEach { it.base = base; it.start() }
        recDots.forEach {
            it.startAnimation(AlphaAnimation(1f, 0.2f).apply {
                duration = 600; repeatMode = Animation.REVERSE; repeatCount = Animation.INFINITE
            })
        }
        chipRec.visibility = View.VISIBLE
        recBadge.visibility = if (fullscreen) View.VISIBLE else View.GONE
        showMessage(getString(R.string.msg_rec_started), MSG_INFO)
    }

    /** [reason] set = the recorder stopped by itself (limit, encoder error). */
    private fun stopRecording(reason: String? = null) {
        if (!recorder.isRecording) return
        var failed: String? = null
        val saved = try { recorder.stop() } catch (e: IllegalStateException) { failed = e.message; null }
        recordButton.isActivated = false
        recordButton.contentDescription = getString(R.string.cd_record_start)
        recTimes.forEach { it.stop() }
        recDots.forEach { it.clearAnimation() }
        chipRec.visibility = View.GONE
        recBadge.visibility = View.GONE
        val video = recorder.savedUri?.let { it to "video/mp4" }
        when {
            failed != null -> showMessage(getString(R.string.msg_rec_failed, failed), MSG_ERROR)
            saved != null && reason != null -> showMessage(getString(R.string.msg_rec_stopped_error, reason) + " · " +
                getString(R.string.msg_rec_saved, saved), MSG_WARNING, video)
            saved != null -> showMessage(getString(R.string.msg_rec_saved, saved), MSG_SUCCESS, video)
            reason != null -> showMessage(getString(R.string.msg_rec_stopped_error, reason), MSG_ERROR)
            else -> showMessage(getString(R.string.msg_rec_discarded), MSG_WARNING)
        }
    }

    /** Unit index for native code: 0 raw, 1 C, 2 F, 3 K. */
    private fun displayUnit(): Int = when (unitGroup.checkedRadioButtonId) { R.id.unit_c -> 1; R.id.unit_f -> 2; R.id.unit_k -> 3; else -> 0 }

    private fun pushSettings() {
        val lut = PALETTE_LUTS.getOrElse(paletteIds.indexOf(paletteGroup.checkedRadioButtonId)) { 4 }
        // FLIR_CAL_* was measured in high gain: in low gain show raw counts instead of wrong temps.
        // Auto (power-up default) stays in high gain for normal scenes, so units stay available.
        val unit = if (gainMode == GAIN_LOW) 0 else displayUnit()
        NativeBridge.nativeSetSettings(lut, unit, spotSwitch.isChecked, nrSwitch.isChecked, minmaxSwitch.isChecked)
        display.edit()
            .putInt("palette", paletteIds.indexOf(paletteGroup.checkedRadioButtonId))
            .putInt("unit", unitIds.indexOf(unitGroup.checkedRadioButtonId))
            .putBoolean("spot", spotSwitch.isChecked)
            .putBoolean("nr", nrSwitch.isChecked)
            .putBoolean("minmax", minmaxSwitch.isChecked)
            .apply()
    }

    /** Display settings from the last run (before the listeners are attached). */
    private fun restoreDisplay() {
        paletteIds.getOrNull(display.getInt("palette", -1))?.let { paletteGroup.check(it) }
        unitIds.getOrNull(display.getInt("unit", -1))?.let { unitGroup.check(it) }
        spotSwitch.isChecked = display.getBoolean("spot", spotSwitch.isChecked)
        nrSwitch.isChecked = display.getBoolean("nr", nrSwitch.isChecked)
        minmaxSwitch.isChecked = display.getBoolean("minmax", false)
        NativeBridge.nativeSetSpot(display.getInt("spotX", NativeBridge.SENSOR_WIDTH / 2),
            display.getInt("spotY", NativeBridge.OUT_HEIGHT / 2))
        NativeBridge.nativeSetCalOffset(display.getFloat("calOffset", 0f))
    }

    /** Put the cross on something of known temperature, type it in: the offset applies to every reading. */
    private fun calibrateDialog() {
        val unit = if (gainMode == GAIN_LOW) 0 else displayUnit()
        val suffix = getString(when (unit) { 2 -> R.string.unit_f; 3 -> R.string.unit_k; else -> R.string.unit_c })
        val offset = display.getFloat("calOffset", 0f)
        val pad = dp(R.dimen.space_xl)
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
            hint = getString(R.string.dlg_cal_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
            fontFeatureSettings = "tnum"
        }
        val view = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, dp(R.dimen.space_s), pad, 0)
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(input, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                addView(TextView(this@MainActivity).apply {
                    text = suffix
                    setTextAppearance(R.style.TextAppearance_Boson_Title)
                    setPadding(dp(R.dimen.space_s), 0, 0, 0)
                })
            })
            addView(TextView(this@MainActivity).apply {
                text = getString(R.string.dlg_cal_current, offset)
                setTextAppearance(R.style.TextAppearance_Boson_Caption)
                fontFeatureSettings = "tnum"
                setPadding(dp(R.dimen.space_xs), dp(R.dimen.space_s), 0, 0)
            })
        }
        val b = AlertDialog.Builder(this)
            .setTitle(R.string.dlg_cal_title)
            .setMessage(R.string.dlg_cal_msg)
            .setView(view)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.dlg_apply, null) // overridden below: stays open on bad input
        if (offset != 0f) b.setNeutralButton(R.string.dlg_cal_reset) { _, _ -> setCalOffset(0f) }
        val dlg = b.create()
        dlg.setOnShowListener {
            dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val t = parseValue(input.text.toString())
                    ?: return@setOnClickListener run { input.error = getString(R.string.dlg_invalid) }
                val c = when (unit) { 2 -> (t - 32) * 5 / 9; 3 -> t - 273.15; else -> t }
                val off = NativeBridge.nativeCalibrate(c.toFloat())
                if (off.isNaN()) showMessage(getString(R.string.msg_cal_needs_live), MSG_WARNING)
                else setCalOffset(off)
                dlg.dismiss()
            }
            input.requestFocus()
        }
        dlg.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        dlg.show()
    }

    private fun setCalOffset(off: Float) {
        NativeBridge.nativeSetCalOffset(off)
        display.edit().putFloat("calOffset", off).apply()
        showMessage(if (off == 0f) getString(R.string.msg_cal_reset) else getString(R.string.msg_cal_set, off), MSG_SUCCESS)
        updateChips()
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
            override fun onLongPress(e: MotionEvent) = centreSpot()
            override fun onDoubleTap(e: MotionEvent): Boolean { setFullscreen(!fullscreen); return true }
        })
        thermalView.setOnTouchListener { _, e -> gd.onTouchEvent(e) }
    }

    /** TalkBack cannot tap a point on the image: offer the gestures as custom actions. */
    private fun setupAccessibility() {
        thermalView.accessibilityDelegate = object : View.AccessibilityDelegate() {
            override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
                super.onInitializeAccessibilityNodeInfo(host, info)
                info.addAction(AccessibilityNodeInfo.AccessibilityAction(R.id.action_center_spot, getString(R.string.a11y_center_spot)))
                info.addAction(AccessibilityNodeInfo.AccessibilityAction(R.id.action_toggle_fullscreen, getString(R.string.a11y_toggle_fullscreen)))
            }
            override fun performAccessibilityAction(host: View, action: Int, args: Bundle?): Boolean = when (action) {
                R.id.action_center_spot -> { centreSpot(); true }
                R.id.action_toggle_fullscreen -> { setFullscreen(!fullscreen); true }
                else -> super.performAccessibilityAction(host, action, args)
            }
        }
    }

    private fun centreSpot() = placeSpot(NativeBridge.SENSOR_WIDTH / 2, NativeBridge.OUT_HEIGHT / 2)

    private fun placeSpot(x: Int, y: Int) {
        haptic(thermalView)
        NativeBridge.nativeSetSpot(x, y)
        display.edit().putInt("spotX", x).putInt("spotY", y).apply()
        if (!spotSwitch.isChecked) spotSwitch.isChecked = true // listener pushes + saves
    }

    private fun setFullscreen(on: Boolean) {
        fullscreen = on
        applyOrientation(resources.configuration)
        exitFullscreen.visibility = if (on) View.VISIBLE else View.GONE
        recBadge.visibility = if (on && recorder.isRecording) View.VISIBLE else View.GONE
        if (Build.VERSION.SDK_INT >= 33) { // predictive back: only intercept Back while full screen
            if (on && backCallback == null) {
                val cb = OnBackInvokedCallback { setFullscreen(false) }
                onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, cb)
                backCallback = cb
            } else if (!on) {
                (backCallback as? OnBackInvokedCallback)?.let { onBackInvokedDispatcher.unregisterOnBackInvokedCallback(it) }
                backCallback = null
            }
        }
        if (Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let {
                if (on) {
                    it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                    it.hide(WindowInsets.Type.systemBars())
                } else {
                    it.show(WindowInsets.Type.systemBars())
                }
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = if (on) View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY else 0
        }
    }

    /** Below API 33 only (enableOnBackInvokedCallback routes Back to [backCallback] from 33 on). */
    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (fullscreen) setFullscreen(false) else @Suppress("DEPRECATION") super.onBackPressed()
    }

    private fun connect() {
        if (conn != null || permissionPending) return // conn is set while starting: no re-entry
        val dev = usb.deviceList.values.firstOrNull { it.vendorId == VID && it.productId == PID }
            ?: return showEmpty(if (empty == E_DETACHED) E_DETACHED else E_NO_DEVICE)
        // Android 9+: USB permission for a UVC (video class) device needs the CAMERA permission first
        if (checkSelfPermission(android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(android.Manifest.permission.CAMERA), REQ_CAMERA)
            return showEmpty(E_CAM_PERMISSION)
        }
        if (!usb.hasPermission(dev)) {
            val pi = PendingIntent.getBroadcast(this, 0,
                Intent(ACTION_USB_PERMISSION).setPackage(packageName), PendingIntent.FLAG_MUTABLE)
            permissionPending = true
            usb.requestPermission(dev, pi)
            return showEmpty(E_WAIT_USB)
        }
        val c = usb.openDevice(dev) ?: return showEmpty(E_OPEN_FAILED)
        conn = c
        device = dev
        showEmpty(E_CONNECTING)
        usbExec.execute {
            val r = NativeBridge.nativeStart(c.fileDescriptor) // ~0.5 s: double start, see README
            runOnUiThread { onStarted(c, dev, r) }
        }
    }

    private fun onStarted(c: UsbDeviceConnection, dev: UsbDevice, r: Int) {
        if (conn !== c) return // disconnected while starting: its stop is queued behind this start
        if (r != 0) {
            disconnect() // closes the connection so the next connect() retries
            return showEmpty(E_START_FAILED, r)
        }
        streaming = true
        showEmpty(null)
        cci = try { CciClient(c, dev) } catch (e: Exception) {
            showMessage(getString(R.string.msg_cci_unavailable, e.message ?: e.javaClass.simpleName), MSG_WARNING); null
        }
        cci?.let { startCci(it) }
        updateChips()
        if (!display.getBoolean("hintShown", false) && cci != null) {
            display.edit().putBoolean("hintShown", true).apply()
            showMessage(getString(R.string.msg_hint), MSG_INFO)
        }
    }

    private fun disconnect() {
        stopRecording()
        settingsDialog?.dismiss()
        settingsDialog = null
        fpaTask?.cancel(false)
        fpaTask = null
        val oldCci = cci
        val oldConn = conn
        cci = null
        conn = null
        usbExec.execute { // after any start still running
            oldCci?.close()
            NativeBridge.nativeStop()
            oldConn?.close()
        }
        device = null
        streaming = false
        NativeBridge.nativeSetFpa(0)
        gainMode = null
        fpaHist.clear()
        fpaSmooth = null
        fpaC = null
        myriadC = null
        warm = false
        listOf(infoFw, infoSn, infoPn).forEach { it.setText(R.string.dash) }
        pushSettings()
        updateChips()
    }

    private class CamState(val fw: String?, val sn: Long?, val pn: String?, val values: Map<String, Double?>)

    private fun startCci(c: CciClient, poll: Boolean = true) {
        // the camera powers up with its defaults: re-apply what the user set last time (poll = fresh connect)
        val saved = if (poll) prefs.all.mapNotNull { (n, v) -> CciParams.find(n)?.let { p -> (v as? Float)?.let { p to it.toDouble() } } } else emptyList()
        c.enqueue({
            saved.forEach { (p, v) -> if (!c.setParam(p, v)) Log.w("BosonApp", "re-apply ${p.name}=$v failed") }
            CamState(c.softwareRev(), c.serialNumber(), c.partNumber(),
                listOf("gain", "ffc-mode", "tf", "scnr", "spnr").associateWith { n -> c.getParam(CciParams.find(n)!!) })
        }) { s ->
            if (cci !== c) return@enqueue
            val dash = getString(R.string.dash)
            infoFw.text = s.fw ?: dash
            infoSn.text = s.sn?.toString() ?: dash
            infoPn.text = s.pn ?: dash
            updatingUi = true
            when (s.values["gain"]?.toInt()) { 0 -> gainGroup.check(R.id.gain_high); 1 -> gainGroup.check(R.id.gain_low); 2 -> gainGroup.check(R.id.gain_auto) }
            when (s.values["ffc-mode"]?.toInt()) { 0 -> ffcModeGroup.check(R.id.ffc_manual); 1 -> ffcModeGroup.check(R.id.ffc_auto) }
            s.values["tf"]?.let { tfSwitch.isChecked = it != 0.0 }
            s.values["scnr"]?.let { scnrSwitch.isChecked = it != 0.0 }
            s.values["spnr"]?.let { spnrSwitch.isChecked = it != 0.0 }
            updatingUi = false
            gainMode = s.values["gain"]?.toInt()
            pushSettings()
            updateChips()
        }
        if (!poll) return
        val myriad = CciParams.find("myriad-temp")!!
        fpaTask = c.every(FPA_PERIOD_MS) {
            val fpa = c.fpaTempCx10()
            val myr = c.getParam(myriad)
            runOnUiThread {
                if (cci !== c) return@runOnUiThread
                onFpa(c, fpa)
                fpaC = fpa?.let { it / 10.0 }
                myriadC = myr
                updateChips()
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
        c.enqueue({ c.runFfc() }) { ok ->
            if (ok) showMessage(getString(R.string.msg_warm_ffc), MSG_SUCCESS)
            else showMessage(getString(R.string.msg_warm_ffc_failed), MSG_WARNING)
        }
        updateChips()
    }

    private fun onOff(on: Boolean) = getString(if (on) R.string.value_on else R.string.value_off)

    private fun setCameraParam(name: String, v: Double, label: CharSequence, shown: CharSequence) {
        if (updatingUi) return
        val c = cci ?: return showMessage(getString(R.string.msg_cci_offline), MSG_WARNING)
        val p = CciParams.find(name)!!
        c.enqueue({ c.setParam(p, v) to c.lastStatus }) { (ok, st) ->
            if (cci !== c) return@enqueue
            if (ok) onParamSet(name, v) else startCci(c, poll = false) // re-sync controls
            if (ok) showMessage(getString(R.string.msg_param_set, label, shown), MSG_SUCCESS)
            else showMessage(getString(R.string.msg_param_failed, name, st), MSG_ERROR)
        }
    }

    private fun onParamSet(name: String, v: Double) {
        if (cci == null) return // late reply after disconnect
        if (name != "freeze") prefs.edit().putFloat(name, v.toFloat()).apply() // never come up frozen
        if (name != "gain") return
        gainMode = v.toInt()
        pushSettings()
        updateChips()
    }

    private fun confirmSave() {
        val c = cci ?: return showMessage(getString(R.string.msg_cci_offline), MSG_WARNING)
        val dlg = AlertDialog.Builder(this)
            .setTitle(R.string.dlg_save_title)
            .setMessage(R.string.dlg_save_msg)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.dlg_save_ok) { _, _ ->
                c.enqueue({ c.saveSettings() to c.lastStatus }) { (ok, st) ->
                    if (ok) showMessage(getString(R.string.msg_saved_settings), MSG_SUCCESS)
                    else showMessage(getString(R.string.msg_save_failed, st), MSG_ERROR)
                }
            }
            .create()
        dlg.setOnShowListener { dlg.getButton(AlertDialog.BUTTON_POSITIVE).setTextColor(getColor(R.color.warning)) }
        dlg.show()
    }

    /** Stop re-applying the camera settings changed in the app. The camera is not touched. */
    private fun confirmForget() {
        AlertDialog.Builder(this)
            .setTitle(R.string.dlg_forget_title)
            .setMessage(R.string.dlg_forget_msg)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.dlg_forget_ok) { _, _ ->
                prefs.edit().clear().apply()
                showMessage(getString(R.string.msg_overrides_forgotten), MSG_SUCCESS)
            }
            .show()
    }

    private fun snapshot() {
        val bmp = Bitmap.createBitmap(NativeBridge.OUT_WIDTH, NativeBridge.OUT_HEIGHT, Bitmap.Config.ARGB_8888)
        if (!NativeBridge.nativeSnapshot(bmp)) return showMessage(getString(R.string.msg_no_frame), MSG_WARNING)
        stamp(bmp)
        val name = "boson_${System.currentTimeMillis()}.png"
        Thread {
            var uri: Uri? = null
            val (msg, kind) = try {
                val where = if (Build.VERSION.SDK_INT >= 29) {
                    val values = ContentValues().apply {
                        put(MediaStore.Images.Media.DISPLAY_NAME, name)
                        put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                        put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/BosonThermal")
                        put(MediaStore.Images.Media.IS_PENDING, 1) // hidden from the gallery until written
                    }
                    val u = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                        ?: throw IllegalStateException(getString(R.string.err_mediastore))
                    try {
                        val ok = contentResolver.openOutputStream(u)?.use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) } ?: false
                        if (!ok) throw IllegalStateException(getString(R.string.err_compress))
                        contentResolver.update(u, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
                    } catch (e: Exception) {
                        contentResolver.delete(u, null, null) // no broken entry left in the gallery
                        throw e
                    }
                    uri = u
                    "Pictures/BosonThermal/$name"
                } else {
                    // API 26-28: app-specific Pictures dir, no storage permission needed
                    val f = File(getExternalFilesDir(Environment.DIRECTORY_PICTURES), name)
                    val ok = try { f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) } } catch (e: Exception) { f.delete(); throw e }
                    if (!ok) { f.delete(); throw IllegalStateException(getString(R.string.err_compress)) }
                    f.absolutePath
                }
                getString(R.string.msg_photo_saved, where) to MSG_SUCCESS
            } catch (e: Exception) {
                getString(R.string.msg_photo_failed, e.message ?: e.javaClass.simpleName) to MSG_ERROR
            }
            runOnUiThread { showMessage(msg, kind, uri?.let { it to "image/png" }) }
        }.start()
    }

    /** Burn the date/time and the calibration context into the bottom-left of the photo. */
    private fun stamp(bmp: Bitmap) {
        val text = getString(R.string.photo_stamp, DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.MEDIUM).format(Date()),
            display.getFloat("calOffset", 0f))
        val p = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = getColor(R.color.text_primary)
            textSize = 15f // bitmap pixels (740x512 frame)
            setShadowLayer(3f, 0f, 0f, getColor(R.color.scrim))
        }
        Canvas(bmp).drawText(text, 8f, NativeBridge.OUT_HEIGHT - 10f, p)
    }

    /** Open or share the last saved photo/video (MediaStore Uri, Android 10+). */
    private fun openCapture(share: Boolean) {
        val (uri, mime) = capture ?: return
        val i = if (share) Intent.createChooser(Intent(Intent.ACTION_SEND).setType(mime).putExtra(Intent.EXTRA_STREAM, uri)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), getString(R.string.share_title))
            else Intent(Intent.ACTION_VIEW).setDataAndType(uri, mime).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        try { startActivity(i) } catch (e: ActivityNotFoundException) { showMessage(getString(R.string.msg_no_viewer), MSG_WARNING) }
    }

    // --- feedback: status chips, empty state, message pill ---

    private fun updateChips() {
        val (state, dot) = when {
            streaming && cci != null -> R.string.state_live to R.color.success
            streaming -> R.string.state_control_offline to R.color.warning
            conn != null || empty == E_CONNECTING -> R.string.state_connecting to R.color.info
            else -> R.string.state_no_camera to R.color.text_disabled
        }
        chipState.setText(state)
        chipState.compoundDrawableTintList = ColorStateList.valueOf(getColor(dot))
        chipWarm.visibility = if (cci != null && !warm) View.VISIBLE else View.GONE
        when (gainMode) {
            GAIN_LOW -> chipGain.setText(R.string.chip_gain_low)
            GAIN_AUTO -> chipGain.setText(R.string.chip_gain_auto)
        }
        chipGain.compoundDrawableTintList = ColorStateList.valueOf(getColor(if (gainMode == GAIN_LOW) R.color.warning else R.color.info))
        chipGain.visibility = if (cci != null && (gainMode == GAIN_LOW || gainMode == GAIN_AUTO)) View.VISIBLE else View.GONE
        val off = display.getFloat("calOffset", 0f)
        chipCal.visibility = if (off != 0f) View.VISIBLE else View.GONE
        chipCal.text = getString(R.string.chip_cal, off)

        val dash = getString(R.string.dash)
        val fpa = fpaC?.let { getString(R.string.temp_c, it) }
        val myr = myriadC?.let { getString(R.string.temp_c, it) }
        readoutText.text = if (fpa != null || myr != null) getString(R.string.readout, fpa ?: dash, myr ?: dash) else ""
        infoFpa.text = fpa ?: dash
        infoMyriad.text = myr ?: dash

        val online = cci != null
        cameraSections.forEach { setEnabledDeep(it, online); it.alpha = if (online) 1f else DISABLED_ALPHA }
        val unitsOk = gainMode != GAIN_LOW
        setEnabledDeep(unitGroup, unitsOk)
        unitGroup.alpha = if (unitsOk) 1f else DISABLED_ALPHA
        controlUnavailable.visibility = if (!online && streaming) View.VISIBLE else View.GONE
        val helper = when (gainMode) {
            GAIN_LOW -> R.string.gain_helper_low to R.color.warning
            GAIN_AUTO -> R.string.gain_helper_auto to R.color.info
            else -> null
        }
        gainHelper.visibility = if (online && helper != null) View.VISIBLE else View.GONE
        helper?.let { (txt, col) ->
            gainHelper.setText(txt)
            gainHelper.compoundDrawableTintList = ColorStateList.valueOf(getColor(col))
        }
    }

    private fun setEnabledDeep(v: View, on: Boolean) {
        v.isEnabled = on
        if (v is ViewGroup) for (i in 0 until v.childCount) setEnabledDeep(v.getChildAt(i), on)
    }

    /** Covers the image with what to do next; null shows the live image. */
    private fun showEmpty(state: Int?, code: Int = 0) {
        empty = state
        if (state == null) {
            emptyState.visibility = View.GONE
            updateChips()
            return
        }
        class E(val title: Int, val body: String, val action: Int? = null, val icon: Int = R.drawable.ic_usb, val busy: Boolean = false)
        val e = when (state) {
            E_CAM_PERMISSION -> E(R.string.empty_cam_perm_title, getString(R.string.empty_cam_perm_body), R.string.action_allow, R.drawable.ic_lock)
            E_CAM_DENIED -> E(R.string.empty_cam_denied_title, getString(R.string.empty_cam_denied_body), R.string.action_open_settings, R.drawable.ic_lock)
            E_WAIT_USB -> E(R.string.empty_wait_usb_title, getString(R.string.empty_wait_usb_body), busy = true)
            E_USB_DENIED -> E(R.string.empty_usb_denied_title, getString(R.string.empty_usb_denied_body), R.string.action_retry, R.drawable.ic_lock)
            E_CONNECTING -> E(R.string.empty_connecting_title, getString(R.string.empty_connecting_body), busy = true)
            E_START_FAILED -> E(R.string.empty_start_failed_title, when (code) { // libusb error codes
                -4 -> getString(R.string.empty_start_failed_body_nodev)
                -3 -> getString(R.string.empty_start_failed_body_access)
                -6 -> getString(R.string.empty_start_failed_body_busy)
                else -> getString(R.string.empty_start_failed_body, code)
            }, R.string.action_retry, R.drawable.ic_error)
            E_DETACHED -> E(R.string.empty_detached_title, getString(R.string.empty_detached_body))
            E_OPEN_FAILED -> E(R.string.empty_open_failed_title, getString(R.string.empty_open_failed_body), R.string.action_retry, R.drawable.ic_error)
            else -> E(R.string.empty_nodev_title, getString(R.string.empty_nodev_body))
        }
        emptyIcon.setImageResource(e.icon)
        emptyIcon.visibility = if (e.busy) View.GONE else View.VISIBLE
        emptyProgress.visibility = if (e.busy) View.VISIBLE else View.GONE
        emptyTitle.setText(e.title)
        emptyBody.text = e.body
        emptyAction.visibility = if (e.action != null) View.VISIBLE else View.GONE
        e.action?.let { emptyAction.setText(it) }
        emptyState.visibility = View.VISIBLE
        updateChips()
    }

    private fun onEmptyAction() {
        when (empty) {
            E_CAM_PERMISSION -> requestPermissions(arrayOf(android.Manifest.permission.CAMERA), REQ_CAMERA)
            E_CAM_DENIED -> startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null)))
            else -> if (started) connect()
        }
    }

    /**
     * Transient feedback over the image; the newest message replaces the current one.
     * [saved] (Uri, MIME) of a new capture adds Open / Share actions and becomes the last capture.
     */
    private fun showMessage(text: String, kind: Int = MSG_INFO, saved: Pair<Uri, String>? = null) {
        handler.removeCallbacksAndMessages(MSG_TOKEN)
        messageText.text = text
        if (saved != null) capture = saved
        val actions = if (saved != null) View.VISIBLE else View.GONE
        messageOpen.visibility = actions
        messageShare.visibility = actions
        val color = when (kind) { MSG_SUCCESS -> R.color.success; MSG_WARNING -> R.color.warning; MSG_ERROR -> R.color.rec; else -> R.color.info }
        messageDot.backgroundTintList = ColorStateList.valueOf(getColor(color))
        if (messagePill.visibility != View.VISIBLE) { messagePill.alpha = 0f; messagePill.visibility = View.VISIBLE }
        messagePill.animate().alpha(1f).setDuration(150).start()
        val ms = if (kind == MSG_WARNING || kind == MSG_ERROR || saved != null) 5000L else 3000L
        handler.postAtTime({
            messagePill.animate().alpha(0f).setDuration(150)
                .withEndAction { if (messagePill.alpha == 0f) messagePill.visibility = View.GONE }.start()
        }, MSG_TOKEN, SystemClock.uptimeMillis() + ms)
    }
}
