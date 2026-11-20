package dev.thermalcam.boson

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Rect
import android.media.MediaRecorder
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.provider.MediaStore
import android.util.Log
import java.io.File

/**
 * Records what the app shows (image + color bar + overlay) to H.264 MP4, no audio.
 * Frames come from NativeBridge.nativeSnapshot and are drawn on MediaRecorder's input
 * surface with a hardware canvas (encoder surfaces need hardware rendering).
 * Saved to Movies/BosonThermal (MediaStore) on Android 10+, app Movies dir before.
 */
class VideoRecorder(private val ctx: Context) {
    companion object {
        private const val TAG = "VideoRecorder"
        const val WIDTH = 736   // 740 rounded down to a multiple of 16 for the encoder
        const val HEIGHT = 512
        const val FPS = 30
    }

    private var recorder: MediaRecorder? = null
    private var pfd: ParcelFileDescriptor? = null
    private var uri: Uri? = null
    private var file: File? = null
    private var thread: Thread? = null
    @Volatile private var running = false
    var label: String = ""
        private set

    val isRecording get() = recorder != null

    /** Starts recording. Throws on failure (nothing is left behind). */
    fun start() {
        val name = "boson_${System.currentTimeMillis()}.mp4"
        try {
            val r = if (Build.VERSION.SDK_INT >= 31) MediaRecorder(ctx) else @Suppress("DEPRECATION") MediaRecorder()
            recorder = r
            r.setVideoSource(MediaRecorder.VideoSource.SURFACE)
            r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            r.setVideoEncoder(MediaRecorder.VideoEncoder.H264)
            r.setVideoSize(WIDTH, HEIGHT)
            r.setVideoFrameRate(FPS)
            r.setVideoEncodingBitRate(6_000_000)
            if (Build.VERSION.SDK_INT >= 29) {
                val values = ContentValues().apply {
                    put(MediaStore.Video.Media.DISPLAY_NAME, name)
                    put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                    put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/BosonThermal")
                    put(MediaStore.Video.Media.IS_PENDING, 1)
                }
                val u = ctx.contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                    ?: throw IllegalStateException("MediaStore insert failed")
                uri = u
                pfd = ctx.contentResolver.openFileDescriptor(u, "w")
                r.setOutputFile(pfd!!.fileDescriptor)
                label = "Movies/BosonThermal/$name"
            } else {
                val f = File(ctx.getExternalFilesDir(Environment.DIRECTORY_MOVIES), name)
                file = f
                r.setOutputFile(f.absolutePath)
                label = f.absolutePath
            }
            r.prepare()
            r.start()
        } catch (e: Exception) {
            cleanup(keep = false)
            throw e
        }
        running = true
        thread = Thread(::pump, "VideoRecorder").also { it.start() }
    }

    /** Copies the latest frame to the encoder surface FPS times per second. */
    private fun pump() {
        val surface = recorder?.surface ?: return
        val bmp = Bitmap.createBitmap(NativeBridge.OUT_WIDTH, NativeBridge.OUT_HEIGHT, Bitmap.Config.ARGB_8888)
        val dst = Rect(0, 0, WIDTH, HEIGHT)
        val periodMs = 1000L / FPS
        var next = SystemClock.uptimeMillis()
        while (running) {
            if (NativeBridge.nativeSnapshot(bmp)) {
                try {
                    val c = surface.lockHardwareCanvas()
                    c.drawBitmap(bmp, null, dst, null)
                    surface.unlockCanvasAndPost(c)
                } catch (e: Exception) {
                    Log.w(TAG, "frame dropped", e)
                }
            }
            next += periodMs
            val wait = next - SystemClock.uptimeMillis()
            if (wait > 0) SystemClock.sleep(wait) else next = SystemClock.uptimeMillis()
        }
        bmp.recycle()
    }

    /** Stops and finalizes the file. Returns its label, or null if nothing usable was recorded. */
    fun stop(): String? {
        if (recorder == null) return null
        running = false
        thread?.join(2000)
        thread = null
        val ok = try {
            recorder?.stop()
            true
        } catch (e: RuntimeException) { // no frame was ever encoded
            Log.w(TAG, "stop failed, discarding", e)
            false
        }
        cleanup(keep = ok)
        return if (ok) label else null
    }

    private fun cleanup(keep: Boolean) {
        recorder?.release()
        recorder = null
        pfd?.close()
        pfd = null
        uri?.let {
            if (keep) {
                ctx.contentResolver.update(it, ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }, null, null)
            } else {
                ctx.contentResolver.delete(it, null, null)
            }
        }
        uri = null
        if (!keep) file?.delete()
        file = null
    }
}
