package dev.thermalcam.boson

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Rect
import android.media.MediaRecorder
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.provider.MediaStore
import android.util.Log
import java.io.File

/** MediaRecorder size cap: 3.5 GB (MP4 on FAT/MediaStore), and 200 MB of storage left free. <= 0 = no room. */
fun recordLimitBytes(freeBytes: Long): Long = minOf(3_500_000_000L, freeBytes - 200_000_000L)

/**
 * Records what the app shows (image + color bar + overlay) to H.264 MP4, no audio.
 * Frames come from NativeBridge.nativeSnapshot and are drawn on MediaRecorder's input
 * surface with a hardware canvas (encoder surfaces need hardware rendering).
 * Saved to Movies/BosonThermal (MediaStore) on Android 10+, app Movies dir before.
 * [onStopped] runs on the main thread when recording ends by itself (size/duration limit,
 * encoder error, lost surface) with the reason; the caller should then call [stop].
 */
class VideoRecorder(private val ctx: Context, private val onStopped: (String) -> Unit) {
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
    private var limitHit = false // the recorder finalized the file itself (max size / duration)
    private val main = Handler(Looper.getMainLooper())
    var label: String = ""
        private set
    /** MediaStore Uri of the last saved video (Android 10+), null before or when discarded. */
    var savedUri: Uri? = null
        private set

    val isRecording get() = recorder != null

    /** Starts recording. Throws on failure (nothing is left behind). */
    fun start() {
        val name = "boson_${System.currentTimeMillis()}.mp4"
        val limit = recordLimitBytes(ctx.getExternalFilesDir(null)?.usableSpace ?: 0L) // same volume as Movies
        if (limit <= 0) throw IllegalStateException(ctx.getString(R.string.err_storage_full))
        limitHit = false
        try {
            val r = if (Build.VERSION.SDK_INT >= 31) MediaRecorder(ctx) else @Suppress("DEPRECATION") MediaRecorder()
            recorder = r
            r.setVideoSource(MediaRecorder.VideoSource.SURFACE)
            r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            r.setVideoEncoder(MediaRecorder.VideoEncoder.H264)
            r.setVideoSize(WIDTH, HEIGHT)
            r.setVideoFrameRate(FPS)
            r.setVideoEncodingBitRate(6_000_000)
            r.setMaxFileSize(limit)
            r.setOnErrorListener { _, what, _ -> report(ctx.getString(R.string.rec_reason_error, what)) }
            r.setOnInfoListener { _, what, _ ->
                when (what) {
                    MediaRecorder.MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED -> { limitHit = true; report(ctx.getString(R.string.rec_reason_max_size)) }
                    MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED -> { limitHit = true; report(ctx.getString(R.string.rec_reason_max_duration)) }
                }
            }
            if (Build.VERSION.SDK_INT >= 29) {
                val values = ContentValues().apply {
                    put(MediaStore.Video.Media.DISPLAY_NAME, name)
                    put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                    put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/BosonThermal")
                    put(MediaStore.Video.Media.IS_PENDING, 1)
                }
                val u = ctx.contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                    ?: throw IllegalStateException(ctx.getString(R.string.err_mediastore))
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
                } catch (e: Throwable) { // surface abandoned (encoder died): no point retrying
                    Log.w(TAG, "encoder surface lost", e)
                    report(ctx.getString(R.string.rec_reason_surface))
                    break
                }
            }
            next += periodMs
            val wait = next - SystemClock.uptimeMillis()
            if (wait > 0) SystemClock.sleep(wait) else next = SystemClock.uptimeMillis()
        }
        bmp.recycle()
    }

    private fun report(reason: String) = main.post { if (recorder != null) onStopped(reason) }

    /**
     * Stops and finalizes the file. Returns its label, or null if nothing usable was recorded.
     * Throws IllegalStateException when the frame thread hung (file deleted).
     */
    fun stop(): String? {
        if (recorder == null) return null
        running = false
        val t = thread
        t?.join(2000)
        thread = null
        if (t?.isAlive == true) { // stuck in the encoder: recorder.stop() would hang too
            Log.e(TAG, "frame thread did not stop, deleting the file")
            cleanup(keep = false)
            throw IllegalStateException(ctx.getString(R.string.err_rec_stuck))
        }
        val ok = try {
            recorder?.stop()
            true
        } catch (e: RuntimeException) { // no frame was ever encoded, or already stopped at a limit
            Log.w(TAG, "stop failed" + if (limitHit) " after a limit, keeping" else ", discarding", e)
            limitHit
        }
        savedUri = if (ok) uri else null
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
