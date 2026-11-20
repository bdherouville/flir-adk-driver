package dev.thermalcam.boson.cci

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

/** Vectors captured from the repo C driver (src/cci.c via libflir_adk.a over a socketpair). */
class FslpTest {
    private fun hex(s: String): ByteArray =
        s.split(" ").filter { it.isNotEmpty() }.map { it.toInt(16).toByte() }.toByteArray()

    private fun decodeAll(wire: ByteArray): Fslp.Response? {
        val d = Fslp.Decoder()
        var r: Fslp.Response? = null
        for (b in wire) d.feed(b)?.let { r = it }
        return r
    }

    @Test fun crcCheckValue() {
        assertEquals(0xE5CC, Fslp.crc16("123456789".toByteArray(Charsets.US_ASCII)))
    }

    @Test fun buildGetCameraSn() {
        assertArrayEquals(hex("8E 00 00 00 00 00 00 05 00 02 FF FF FF FF EA 28 AE"),
            Fslp.buildFrame(0, 0x00050002))
    }

    @Test fun buildPayloadStuffing() { // ffc-period = 0x8E9E, seq 1
        assertArrayEquals(hex("8E 00 00 00 00 01 00 05 00 0A FF FF FF FF 00 00 9E 81 9E 91 F5 5A AE"),
            Fslp.buildFrame(1, 0x0005000A, hex("00 00 8E 9E")))
    }

    @Test fun buildFloatPayload() { // agc-gamma = 0.97, seq 2
        assertArrayEquals(hex("8E 00 00 00 00 02 00 09 00 0D FF FF FF FF 3F 78 51 EC AA FF AE"),
            Fslp.buildFrame(2, 0x0009000D, hex("3F 78 51 EC")))
    }

    @Test fun buildCrcStuffing() { // ffc-temp = 24, seq 3: CRC 0x429E gets stuffed
        assertArrayEquals(hex("8E 00 00 00 00 03 00 05 00 08 FF FF FF FF 00 18 42 9E 91 AE"),
            Fslp.buildFrame(3, 0x00050008, hex("00 18")))
    }

    @Test fun buildMoreRequests() {
        assertArrayEquals(hex("8E 00 00 00 00 04 00 05 00 30 FF FF FF FF 3E AA AE"), Fslp.buildFrame(4, 0x00050030))
        assertArrayEquals(hex("8E 00 00 00 00 08 00 05 00 18 FF FF FF FF 81 23 AE"), Fslp.buildFrame(8, 0x00050018))
        assertArrayEquals(hex("8E 00 00 00 00 09 00 05 00 07 FF FF FF FF 0B A3 AE"), Fslp.buildFrame(9, 0x00050007))
    }

    @Test fun parseFpaReply() {
        val r = decodeAll(hex("8E 00 00 00 00 04 00 05 00 30 00 00 00 00 01 7B 44 27 AE"))
        assertNotNull(r)
        assertEquals(4, r!!.seq)
        assertEquals(0x00050030, r.functionId)
        assertEquals(0, r.status)
        assertArrayEquals(hex("01 7B"), r.payload) // 379 -> 37.9 degC
    }

    @Test fun parseStuffedReply() { // payload 00 00 00 AE arrives as ... 9E A1
        val r = decodeAll(hex("8E 00 00 00 00 05 00 05 00 0B 00 00 00 00 00 00 00 9E A1 2A 3E AE"))
        assertArrayEquals(hex("00 00 00 AE"), r!!.payload)
        assertEquals(174.0, CciParams.find("ffc-period")!!.decode(r.payload)!!, 0.0)
    }

    @Test fun parseErrorStatus() {
        val r = decodeAll(hex("8E 00 00 00 00 06 00 3E 00 02 00 00 01 61 74 7B AE"))
        assertEquals(0x161, r!!.status)
        assertEquals(0, r.payload.size)
    }

    @Test fun parseSoftwareRev() {
        val r = decodeAll(hex("8E 00 00 00 00 01 00 05 00 22 00 00 00 00 00 00 00 02 00 00 00 00 00 00 3B 77 37 1B AE"))
        assertArrayEquals(hex("00 00 00 02 00 00 00 00 00 00 3B 77"), r!!.payload) // 2.0.15223
    }

    @Test fun badCrcAndShortFramesRejected() {
        assertNull(decodeAll(hex("8E 00 00 00 00 04 00 05 00 30 00 00 00 00 01 7B 44 28 AE")))
        assertNull(decodeAll(hex("8E 00 01 02 AE")))
    }

    @Test fun garbageBeforeStartIgnored() {
        val r = decodeAll(hex("00 AE 13 8E 00 00 00 00 00 00 00 00 00 00 00 00 00 4E C3 AE"))
        assertEquals(0, r!!.status)
    }

    @Test fun roundTrip() {
        val payload = ByteArray(40) { (0x80 + it).toByte() } // covers 0x8E, 0x9E, 0xA..
        val wire = Fslp.buildFrame(7, 0x12345678, payload, status = 0)
        val r = decodeAll(wire)!!
        assertEquals(7, r.seq)
        assertEquals(0x12345678, r.functionId)
        assertArrayEquals(payload, r.payload)
    }

    private fun replyTo(seq: Int, fn: Int, wire: ByteArray) = Fslp.Decoder().replyTo(seq, fn, wire, wire.size)

    @Test fun replyMatchesSeqAndFunction() {
        val r = replyTo(3, 0x00050030, Fslp.buildFrame(3, 0x00050030, hex("01 7B"), status = 0))
        assertArrayEquals(hex("01 7B"), r!!.payload)
    }

    @Test fun staleRepliesSkipped() { // late reply to seq 2, wrong function, then ours
        val wire = Fslp.buildFrame(2, 0x00050030, hex("01 7B"), status = 0) +
            Fslp.buildFrame(3, 0x00050002, hex("00 00 00 01"), status = 0) +
            Fslp.buildFrame(3, 0x00050030, hex("00 2A"), status = 0x161)
        val r = replyTo(3, 0x00050030, wire)!!
        assertEquals(0x161, r.status)
        assertArrayEquals(hex("00 2A"), r.payload)
    }

    @Test fun noMatchingReply() {
        assertNull(replyTo(4, 0x00050030, Fslp.buildFrame(3, 0x00050030, status = 0)))
        assertNull(replyTo(3, 0x00050002, Fslp.buildFrame(3, 0x00050030, status = 0)))
    }

    @Test fun replySplitAcrossReads() {
        val wire = Fslp.buildFrame(5, 0x00050002, hex("00 00 8E 9E"), status = 0)
        val d = Fslp.Decoder()
        assertNull(d.replyTo(5, 0x00050002, wire, 7))
        val rest = wire.copyOfRange(7, wire.size)
        assertArrayEquals(hex("00 00 8E 9E"), d.replyTo(5, 0x00050002, rest, rest.size)!!.payload)
    }
}
