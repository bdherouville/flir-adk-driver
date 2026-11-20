package dev.thermalcam.boson

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.Locale

class ValueFormatTest {
    @Test fun floatsUseDotOnCommaLocales() {
        val saved = Locale.getDefault()
        try {
            Locale.setDefault(Locale.FRANCE)
            assertEquals("0.970", formatValue('f', 0.97))
            assertEquals(0.97, parseValue(formatValue('f', 0.97))!!, 1e-9)
        } finally {
            Locale.setDefault(saved)
        }
    }

    @Test fun parseAcceptsCommaAndSpaces() {
        assertEquals(1.05, parseValue(" 1,05 ")!!, 1e-9)
        assertEquals(300.0, parseValue("300")!!, 0.0)
        assertNull(parseValue("abc"))
        assertNull(parseValue(""))
    }

    @Test fun integersAndMissing() {
        assertEquals("300", formatValue('u', 300.0))
        assertEquals("n/a", formatValue('f', null))
    }
}
