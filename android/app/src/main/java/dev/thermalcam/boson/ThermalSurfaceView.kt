package dev.thermalcam.boson

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceView

/** SurfaceView that keeps the 740:512 aspect of the native output inside the space it is given. */
class ThermalSurfaceView(context: Context, attrs: AttributeSet?) : SurfaceView(context, attrs) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val maxW = MeasureSpec.getSize(widthMeasureSpec)
        val maxH = MeasureSpec.getSize(heightMeasureSpec)
        var w = maxW
        var h = w * NativeBridge.OUT_HEIGHT / NativeBridge.OUT_WIDTH
        if (MeasureSpec.getMode(heightMeasureSpec) != MeasureSpec.UNSPECIFIED && h > maxH) {
            h = maxH
            w = h * NativeBridge.OUT_WIDTH / NativeBridge.OUT_HEIGHT
        }
        setMeasuredDimension(w, h)
    }
}
