package dev.thermalcam.boson

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.Locale
import dev.thermalcam.boson.cci.CciClient
import dev.thermalcam.boson.cci.CciParam
import dev.thermalcam.boson.cci.CciParams

/**
 * "All camera settings": one row per CciParams entry with its current value and help text.
 * Tap a writable row to edit it (range-checked by CciParam.encode). Read-only rows only display.
 * onSet(name, value) runs on the main thread after a successful write; onMessage(text, ok)
 * reports every write result to the activity.
 */
class CameraSettingsDialog(
    private val act: Activity,
    private val cci: CciClient,
    private val onSet: (String, Double) -> Unit,
    private val onMessage: (String, Boolean) -> Unit,
) {
    private fun dp(v: Int) = (v * act.resources.displayMetrics.density).toInt()
    private val list = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL; setPadding(0, dp(4), 0, dp(8)) }
    private val open = mutableSetOf<String>() // expanded sections, kept across refresh
    private lateinit var dialog: AlertDialog

    fun show() {
        dialog = AlertDialog.Builder(act)
            .setTitle(R.string.dlg_settings_title)
            .setView(ScrollView(act).apply { addView(list) })
            .setNegativeButton(R.string.dlg_close, null)
            .setNeutralButton(R.string.dlg_refresh, null)
            .create()
        dialog.setOnShowListener {
            // neutral button would dismiss the dialog by default
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener { refresh() }
        }
        dialog.show()
        refresh()
    }

    /** Call when the CciClient closes: its executor rejects new work. */
    fun dismiss() = dialog.dismiss()

    private fun refresh() {
        dialog.setTitle(R.string.dlg_settings_reading)
        cci.enqueue({ CciParams.all.map { it to cci.getParam(it) } }) { values ->
            if (!dialog.isShowing) return@enqueue
            dialog.setTitle(R.string.dlg_settings_title)
            list.removeAllViews()
            values.groupBy { (p, _) -> group(p.name) }.toSortedMap(compareBy { GROUPS.indexOf(it) })
                .entries.forEachIndexed { i, (g, rows) ->
                    if (i > 0) list.addView(View(act).apply { setBackgroundColor(act.getColor(R.color.outline)) },
                        LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1)))
                    list.addView(section(g, rows))
                }
        }
    }

    private fun ripple() = TypedValue().let {
        act.theme.resolveAttribute(android.R.attr.selectableItemBackground, it, true)
        act.getDrawable(it.resourceId)
    }

    /** Collapsible section: tap the header to show / hide its rows. */
    private fun section(name: String, rows: List<Pair<CciParam, Double?>>): LinearLayout {
        val body = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            visibility = if (name in open) View.VISIBLE else View.GONE
            rows.forEach { (p, v) -> addView(row(p, v)) }
        }
        val arrow = ImageView(act).apply {
            setImageResource(R.drawable.ic_expand_more)
            imageTintList = ColorStateList.valueOf(act.getColor(R.color.text_secondary))
            rotation = if (body.visibility == View.VISIBLE) 0f else -90f
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        val header = LinearLayout(act).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(48)
            setPadding(dp(16), dp(4), dp(24), dp(4))
            background = ripple()
            addView(arrow, LinearLayout.LayoutParams(dp(24), dp(24)).apply { marginEnd = dp(12) })
            addView(TextView(act).apply {
                text = name
                setTextAppearance(R.style.TextAppearance_Boson_Body)
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(TextView(act).apply {
                text = rows.size.toString()
                setTextAppearance(R.style.TextAppearance_Boson_Caption)
            })
            isClickable = true
            setOnClickListener {
                val show = body.visibility != View.VISIBLE
                body.visibility = if (show) View.VISIBLE else View.GONE
                if (show) open += name else open -= name
                arrow.animate().rotation(if (show) 0f else -90f).setDuration(150).start()
            }
        }
        return LinearLayout(act).apply { orientation = LinearLayout.VERTICAL; addView(header); addView(body) }
    }

    private fun fmt(p: CciParam, v: Double?): String = if (v == null) act.getString(R.string.dlg_na) else formatValue(p.type, v)

    private fun row(p: CciParam, v: Double?): View = LinearLayout(act).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(56)
        setPadding(dp(52), dp(12), dp(24), dp(12))
        val range = if (p.readOnly) "" else " · " + act.getString(R.string.dlg_range, fmt(p, p.lo), fmt(p, p.hi))
        addView(LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(act).apply {
                text = p.name
                setTextAppearance(R.style.TextAppearance_Boson_Readout)
                textSize = 15f
            })
            addView(TextView(act).apply {
                text = p.help + range
                setTextAppearance(R.style.TextAppearance_Boson_Caption)
                setPadding(0, dp(2), 0, 0)
            })
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        addView(TextView(act).apply {
            text = fmt(p, v)
            setTextAppearance(R.style.TextAppearance_Boson_Readout)
            setTextColor(act.getColor(if (p.readOnly) R.color.text_secondary else R.color.accent))
            gravity = Gravity.END
            setPadding(dp(12), 0, 0, 0)
        })
        if (p.readOnly) {
            addView(TextView(act).apply {
                setText(R.string.dlg_read_only)
                setTextAppearance(R.style.TextAppearance_Boson_Caption)
                setPadding(dp(12), 0, 0, 0)
            })
        } else {
            addView(ImageView(act).apply {
                setImageResource(R.drawable.ic_chevron_right)
                imageTintList = ColorStateList.valueOf(act.getColor(R.color.text_secondary))
                importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            }, LinearLayout.LayoutParams(dp(24), dp(24)).apply { marginStart = dp(8) })
            background = ripple()
            setOnClickListener { edit(p, v) }
        }
    }

    private fun edit(p: CciParam, cur: Double?) {
        val pad = dp(24)
        val input = EditText(act).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED or
                (if (p.type == 'f') InputType.TYPE_NUMBER_FLAG_DECIMAL else 0)
            if (cur != null) setText(formatValue(p.type, cur))
            fontFeatureSettings = "tnum"
            selectAll()
        }
        val box = FrameLayout(act).apply { setPadding(pad, dp(8), pad, 0); addView(input) }
        val ed = AlertDialog.Builder(act)
            .setTitle(p.name)
            .setMessage(p.help + "\n" + act.getString(R.string.dlg_range, fmt(p, p.lo), fmt(p, p.hi)))
            .setView(box)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.dlg_set, null) // overridden below: stays open on bad input
            .create()
        ed.setOnShowListener {
            ed.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                if (!dialog.isShowing) return@setOnClickListener ed.dismiss() // camera gone meanwhile
                val v = parseValue(input.text.toString())
                if (v == null || p.encode(v) == null) {
                    input.error = act.getString(R.string.dlg_invalid)
                    return@setOnClickListener
                }
                ed.dismiss()
                cci.enqueue({ cci.setParam(p, v) to cci.lastStatus }) { (ok, st) ->
                    if (ok) {
                        onMessage(act.getString(R.string.msg_param_set, p.name, formatValue(p.type, v)), true)
                        onSet(p.name, v)
                    } else {
                        onMessage(act.getString(R.string.msg_param_failed, p.name, st), false)
                    }
                    if (dialog.isShowing) refresh()
                }
            }
            input.requestFocus()
        }
        ed.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        ed.show()
    }
}

/** Display a CCI value; always '.' decimals so parseValue reads it back on any locale. */
internal fun formatValue(type: Char, v: Double?): String = when {
    v == null -> "n/a"
    type == 'f' -> String.format(Locale.US, "%.3f", v)
    else -> v.toLong().toString()
}

/** Parse user input; accepts ',' as the decimal mark (French, German, ... keyboards). */
internal fun parseValue(text: String): Double? = text.trim().replace(',', '.').toDoubleOrNull()

/** Section order in the dialog; group() maps every CciParams name to one of these. */
internal val GROUPS = listOf("Sensor and flat-field correction (FFC)", "Noise filters", "Camera AGC (8-bit stream)",
    "Video output", "Image statistics", "Status", "Radiometry (radiometric cameras only)")

internal fun group(n: String): String = when {
    n.startsWith("ffc") || n == "gain" -> GROUPS[0]
    n.startsWith("tf") || n.startsWith("scnr") || n.startsWith("spnr") || n == "bpr" -> GROUPS[1]
    n.startsWith("agc") -> GROUPS[2]
    n.startsWith("dvo") || n in setOf("sym", "telemetry", "lut", "freeze") -> GROUPS[3]
    n.startsWith("stats") -> GROUPS[4]
    n in setOf("tlinear", "radiometry", "spotmeter") -> GROUPS[6]
    else -> GROUPS[5]
}
