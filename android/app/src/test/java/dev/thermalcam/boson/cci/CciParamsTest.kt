package dev.thermalcam.boson.cci

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CciParamsTest {
    private fun p(n: String) = CciParams.find(n)!!

    @Test fun tableSanity() { // mirrors tools/test_cci.c step 10
        assertEquals(56, CciParams.all.size)
        assertEquals(CciParams.all.size, CciParams.all.map { it.name }.toSet().size)
        for (q in CciParams.all) {
            assertTrue(q.name, q.getId != 0)
            assertTrue(q.name, q.type in "euhwf")
            assertTrue(q.name, q.lo <= q.hi)
            assertTrue(q.name, q.help.isNotEmpty())
        }
        assertNull(CciParams.find("nope"))
    }

    @Test fun idsMatchC() {
        val g = p("agc-gamma")
        assertEquals(0x0009000E, g.getId); assertEquals(0x0009000D, g.setId); assertEquals('f', g.type)
        assertEquals(0x00050015, p("gain").getId); assertEquals(0x00050014, p("gain").setId)
        assertEquals(0x00050013, p("ffc-mode").getId); assertEquals(0x00050012, p("ffc-mode").setId)
        assertEquals(0x000A0001, p("tf").setId); assertEquals(0x00080001, p("scnr").setId); assertEquals(0x000C0001, p("spnr").setId)
        assertEquals(0x0006000C, p("dvo-format").postId)
        assertEquals(0x00050068, p("myriad-temp").getId)
    }

    @Test fun encodeMatchesC() {
        assertArrayEquals(byteArrayOf(0x3F, 0x78, 0x51, 0xEC.toByte()), p("agc-gamma").encode(0.97))
        assertArrayEquals(byteArrayOf(0, 10), p("ffc-temp").encode(10.0))
        assertArrayEquals(byteArrayOf(0, 0, 0x8E.toByte(), 0x9E.toByte()), p("ffc-period").encode(0x8E9E.toDouble()))
        assertArrayEquals(byteArrayOf(0, 0, 0, 2), p("gain").encode(2.0))
    }

    @Test fun encodeRejectsLikeC() { // tools/test_cci.c step 4
        assertNull(p("agc-gamma").encode(5.0))      // out of range
        assertNull(p("agc-gamma").encode(Double.NaN))
        assertNull(p("ffc-mode").encode(1.5))       // not whole
        assertNull(p("ffc-mode").encode(3.0))       // above hi
        assertNull(p("fps").encode(30.0))           // read-only
        assertTrue(p("fps").readOnly)
    }

    @Test fun rangeEdgesAccepted() {
        for (q in CciParams.all.filter { !it.readOnly }) {
            assertTrue(q.name, q.encode(q.lo) != null)
            assertTrue(q.name, q.encode(q.hi) != null)
            assertNull(q.name, q.encode(q.hi + 1))
        }
    }

    @Test fun decodeTypes() {
        assertEquals(0.97, p("agc-gamma").decode(byteArrayOf(0x3F, 0x78, 0x51, 0xEC.toByte()))!!, 1e-6)
        assertEquals(4294967295.0, p("fps").decode(byteArrayOf(-1, -1, -1, -1))!!, 0.0)       // 'u'
        assertEquals(-1.0, p("ffc-in-progress").decode(byteArrayOf(-1, -1))!!, 0.0)           // 'h'
        assertEquals(65535.0, p("ffc-last-temp-kx10").decode(byteArrayOf(-1, -1))!!, 0.0)     // 'w'
        assertEquals(-1.0, p("gain").decode(byteArrayOf(-1, -1, -1, -1))!!, 0.0)              // 'e'
        assertNull(p("gain").decode(byteArrayOf(0, 0)))                                         // too short
    }
}
