package dev.thermalcam.boson

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Typeface
import android.text.InputType
import android.widget.EditText
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
 * onSet(name, value) runs on the main thread after a successful write.
 */
class CameraSettingsDialog(
    private val act: Activity,
    private val cci: CciClient,
    private val onSet: (String, Double) -> Unit,
) {
    private val list = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL; setPadding(32, 16, 32, 16) }
    private var title = "All camera settings"
    private lateinit var dialog: AlertDialog

    fun show() {
        dialog = AlertDialog.Builder(act)
            .setTitle(title)
            .setView(ScrollView(act).apply { addView(list) })
            .setNegativeButton("Close", null)
            .setNeutralButton("Refresh", null)
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
        dialog.setTitle("Reading camera…")
        cci.enqueue({ CciParams.all.map { it to cci.getParam(it) } }) { values ->
            if (!dialog.isShowing) return@enqueue
            dialog.setTitle(title)
            list.removeAllViews()
            values.forEach { (p, v) -> list.addView(row(p, v)) }
        }
    }

    private fun fmt(p: CciParam, v: Double?): String = formatValue(p.type, v)

    private fun row(p: CciParam, v: Double?): TextView = TextView(act).apply {
        val range = if (p.readOnly) "read-only" else "${fmt(p, p.lo)}..${fmt(p, p.hi)}"
        text = "${p.name} = ${fmt(p, v)}\n${p.help} ($range)"
        setPadding(0, 12, 0, 12)
        if (p.readOnly) {
            alpha = 0.6f
        } else {
            setTypeface(typeface, Typeface.BOLD)
            setOnClickListener { edit(p, v) }
        }
    }

    private fun edit(p: CciParam, cur: Double?) {
        val input = EditText(act).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED or
                (if (p.type == 'f') InputType.TYPE_NUMBER_FLAG_DECIMAL else 0)
            if (cur != null) setText(fmt(p, cur))
        }
        AlertDialog.Builder(act)
            .setTitle(p.name)
            .setMessage("${p.help}\nRange ${fmt(p, p.lo)}..${fmt(p, p.hi)}")
            .setView(input)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton("Set") { _, _ ->
                if (!dialog.isShowing) return@setPositiveButton // camera gone meanwhile
                val v = parseValue(input.text.toString())
                if (v == null || p.encode(v) == null) {
                    title = "${p.name}: invalid value"
                    dialog.setTitle(title)
                    return@setPositiveButton
                }
                cci.enqueue({ cci.setParam(p, v) to cci.lastStatus }) { (ok, st) ->
                    title = if (ok) "${p.name} = ${fmt(p, v)}" else "${p.name} failed (status 0x%08X)".format(st)
                    if (ok) onSet(p.name, v)
                    if (dialog.isShowing) refresh()
                }
            }
            .show()
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
