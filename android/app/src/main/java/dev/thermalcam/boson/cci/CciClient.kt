package dev.thermalcam.boson.cci

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.io.Closeable
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * Boson CCI over the CDC-ACM data interface (bulk OUT/IN) of an already opened UsbDeviceConnection.
 * Single serial owner: every transaction runs on one thread (the executor below), in order.
 * Blocking methods (getParam, setParam, runFfc, ...) must only be called from work passed to
 * enqueue() / every().
 */
class CciClient(private val conn: UsbDeviceConnection, device: UsbDevice) : Closeable {
    private val dataIf: UsbInterface
    private val epOut: UsbEndpoint
    private val epIn: UsbEndpoint
    private val exec = Executors.newSingleThreadScheduledExecutor()
    private val main = Handler(Looper.getMainLooper())
    private val rx = ByteArray(512)
    private var seq = 0
    @Volatile private var closed = false

    /** Status word of the last transaction: 0 ok, -1 (0xFFFFFFFF) no valid reply, else FLR_RESULT. */
    @Volatile var lastStatus: Int = -1
        private set

    init {
        val ifs = (0 until device.interfaceCount).map { device.getInterface(it) }
        dataIf = ifs.firstOrNull { it.interfaceClass == UsbConstants.USB_CLASS_CDC_DATA }
            ?: throw IllegalStateException("no CDC data interface")
        val eps = (0 until dataIf.endpointCount).map { dataIf.getEndpoint(it) }
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        epOut = eps.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT } ?: throw IllegalStateException("no bulk OUT")
        epIn = eps.firstOrNull { it.direction == UsbConstants.USB_DIR_IN } ?: throw IllegalStateException("no bulk IN")
        if (!conn.claimInterface(dataIf, true)) throw IllegalStateException("cannot claim CDC data interface")
        // SET_LINE_CODING 921600 8N1 on the CDC comm interface. Harmless over USB; failures ignored.
        ifs.firstOrNull { it.interfaceClass == UsbConstants.USB_CLASS_COMM }?.let {
            val lc = byteArrayOf(0x00, 0x10, 0x0E, 0x00, 0, 0, 8)
            conn.controlTransfer(0x21, 0x20, 0, it.id, lc, lc.size, 500)
        }
    }

    /** One request/response. Returns the reply payload, or null (lastStatus says why). */
    private fun transact(functionId: Int, payload: ByteArray = ByteArray(0), timeoutMs: Long = 2000): ByteArray? {
        lastStatus = -1
        if (closed) return null
        while (conn.bulkTransfer(epIn, rx, rx.size, 5) > 0) { /* drop stale bytes */ }
        val mySeq = seq
        val frame = Fslp.buildFrame(mySeq, functionId, payload)
        seq = (seq + 1) % 10
        if (closed) return null
        if (conn.bulkTransfer(epOut, frame, frame.size, 200) != frame.size) return null
        val dec = Fslp.Decoder()
        val deadline = SystemClock.uptimeMillis() + timeoutMs
        while (!closed) {
            val left = deadline - SystemClock.uptimeMillis()
            if (left <= 0) return null
            // Short slices so close() never waits long for an in-flight transfer.
            val n = conn.bulkTransfer(epIn, rx, rx.size, minOf(left, 100L).toInt())
            if (n < 0) { SystemClock.sleep(10); continue } // timeout or error: never spin
            val r = dec.replyTo(mySeq, functionId, rx, n) ?: continue
            lastStatus = r.status
            return if (r.status == 0) r.payload else null
        }
        return null
    }

    fun getParam(p: CciParam): Double? = transact(p.getId)?.let { p.decode(it) }

    fun setParam(p: CciParam, v: Double): Boolean {
        val payload = p.encode(v) ?: return false
        if (transact(p.setId, payload) == null) return false
        return p.postId == 0 || transact(p.postId) != null
    }

    fun runFfc(): Boolean = transact(RUN_FFC) != null

    /** FPA temperature, degC x10 (INT_16). */
    fun fpaTempCx10(): Int? = transact(FPA_TEMP_CX10)?.takeIf { it.size >= 2 }
        ?.let { ((it[0].toInt() shl 8) or (it[1].toInt() and 0xFF)).toShort().toInt() }

    fun serialNumber(): Long? = transact(GET_CAMERA_SN)?.takeIf { it.size >= 4 }?.let { be32(it, 0) }

    fun partNumber(): String? = transact(GET_CAMERA_PN)?.let { ascii(it) }

    fun softwareRev(): String? = transact(GET_SW_REV)?.takeIf { it.size >= 12 }
        ?.let { "${be32(it, 0)}.${be32(it, 4)}.${be32(it, 8)}" }

    /** bosonWriteDynamicHeaderToFlash: current settings become power-up defaults. Confirm first. */
    fun saveSettings(): Boolean = transact(SAVE_SETTINGS) != null

    /** Run work on the CCI thread, deliver its result on the main thread. */
    fun <T> enqueue(work: () -> T, done: (T) -> Unit) {
        if (closed) return
        exec.execute {
            val r = try { work() } catch (e: Exception) { Log.w(TAG, "CCI work failed", e); return@execute }
            if (!closed) main.post { if (!closed) done(r) } // closed is set on main by close()
        }
    }

    /** Run work on the CCI thread now and then every periodMs; exceptions are logged, not fatal. */
    fun every(periodMs: Long, work: () -> Unit): ScheduledFuture<*> =
        exec.scheduleWithFixedDelay({
            try { work() } catch (e: Exception) { Log.w(TAG, "periodic CCI work failed", e) }
        }, 0, periodMs, TimeUnit.MILLISECONDS)

    /**
     * Non-blocking stop: queued work returns at once (transact sees closed), periodic work is
     * cancelled by shutdown(), and the interface is released on the CCI thread after any in-flight
     * transfer. The short wait lets that finish before the caller closes the connection.
     */
    override fun close() {
        if (closed) return
        closed = true
        exec.execute { conn.releaseInterface(dataIf) }
        exec.shutdown()
        if (!exec.awaitTermination(1, TimeUnit.SECONDS)) Log.w(TAG, "CCI thread still busy at close")
    }

    companion object {
        private const val TAG = "CciClient"
        const val GET_CAMERA_SN = 0x00050002
        const val GET_CAMERA_PN = 0x00050004
        const val RUN_FFC = 0x00050007
        const val SAVE_SETTINGS = 0x00050018
        const val GET_SW_REV = 0x00050022
        const val FPA_TEMP_CX10 = 0x00050030

        private fun be32(b: ByteArray, o: Int): Long =
            (((b[o].toInt() and 0xFF) shl 24) or ((b[o + 1].toInt() and 0xFF) shl 16) or
                ((b[o + 2].toInt() and 0xFF) shl 8) or (b[o + 3].toInt() and 0xFF)).toLong() and 0xFFFFFFFFL

        private fun ascii(b: ByteArray): String =
            String(b, Charsets.US_ASCII).substringBefore(Char(0)).trim()
    }
}
