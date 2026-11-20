package dev.thermalcam.boson.cci

import java.io.ByteArrayOutputStream

/**
 * FSLP framing for the Boson CCI (mirror of src/cci.c).
 * Unstuffed body: channel(1)=0, seq(4 BE), function id(4 BE), status(4 BE), payload, CRC-16(2 BE).
 * On the wire: 0x8E, stuffed body, 0xAE. Stuffing: 0x8E/0x9E/0xAE -> 0x9E, byte-0x0D.
 */
object Fslp {
    const val START = 0x8E
    const val END = 0xAE
    const val ESC = 0x9E
    const val MAX_BODY = 1024

    /** CRC-16/AUG-CCITT: init 0x1D0F, poly 0x1021, no reflection, no xor-out. */
    fun crc16(data: ByteArray, len: Int = data.size): Int {
        var crc = 0x1D0F
        for (i in 0 until len) {
            crc = crc xor ((data[i].toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF else (crc shl 1) and 0xFFFF
            }
        }
        return crc
    }

    /** Complete stuffed frame for a request. status is 0xFFFFFFFF (-1) for TX. */
    fun buildFrame(seq: Int, functionId: Int, payload: ByteArray = ByteArray(0), status: Int = -1): ByteArray {
        val body = ByteArrayOutputStream()
        body.write(0)
        putInt(body, seq)
        putInt(body, functionId)
        putInt(body, status)
        body.write(payload)
        val crc = crc16(body.toByteArray())
        body.write(crc shr 8)
        body.write(crc)
        val out = ByteArrayOutputStream()
        out.write(START)
        for (b in body.toByteArray()) {
            val c = b.toInt() and 0xFF
            if (c == START || c == ESC || c == END) {
                out.write(ESC)
                out.write(c - 0x0D)
            } else {
                out.write(c)
            }
        }
        out.write(END)
        return out.toByteArray()
    }

    private fun putInt(o: ByteArrayOutputStream, v: Int) {
        o.write(v ushr 24); o.write(v ushr 16); o.write(v ushr 8); o.write(v)
    }

    private fun getInt(b: ByteArray, off: Int): Int =
        ((b[off].toInt() and 0xFF) shl 24) or ((b[off + 1].toInt() and 0xFF) shl 16) or
            ((b[off + 2].toInt() and 0xFF) shl 8) or (b[off + 3].toInt() and 0xFF)

    class Response(val seq: Int, val functionId: Int, val status: Int, val payload: ByteArray)

    /** Parse an unstuffed body (bytes between the flags). null if shorter than 15 bytes or CRC mismatch. */
    fun parseBody(body: ByteArray): Response? {
        if (body.size < 15) return null
        val n = body.size
        val got = ((body[n - 2].toInt() and 0xFF) shl 8) or (body[n - 1].toInt() and 0xFF)
        if (got != crc16(body, n - 2)) return null
        return Response(getInt(body, 1), getInt(body, 5), getInt(body, 9), body.copyOfRange(13, n - 2))
    }

    /** Byte-stream decoder: feed every received byte; returns a Response when a valid frame ends. */
    class Decoder {
        private val buf = ByteArrayOutputStream()
        private var inFrame = false
        private var escaped = false

        fun feed(b: Byte): Response? {
            val c = b.toInt() and 0xFF
            when {
                c == START -> { inFrame = true; escaped = false; buf.reset() }
                !inFrame -> {}
                c == END -> { inFrame = false; return parseBody(buf.toByteArray()) }
                c == ESC -> escaped = true
                else -> {
                    val v = if (escaped) c + 0x0D else c
                    escaped = false
                    if (buf.size() < MAX_BODY) buf.write(v)
                }
            }
            return null
        }

        /**
         * Feed buf[0 until n]; return the first valid reply to request (seq, functionId). Frames for
         * any other request (stale or late replies) are dropped, as FLIR's dispatcher rejects
         * sequence / function-ID mismatches.
         */
        fun replyTo(seq: Int, functionId: Int, buf: ByteArray, n: Int): Response? {
            for (i in 0 until n) {
                val r = feed(buf[i]) ?: continue
                if (r.seq == seq && r.functionId == functionId) return r
            }
            return null
        }
    }
}
