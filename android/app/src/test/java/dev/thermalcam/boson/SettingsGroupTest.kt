package dev.thermalcam.boson

import dev.thermalcam.boson.cci.CciParams
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SettingsGroupTest {
    @Test fun everySectionHasParamsAndKnownOnesLandRight() {
        val byGroup = CciParams.all.groupBy { group(it.name) }
        GROUPS.forEach { assertTrue("empty section $it", byGroup[it].orEmpty().isNotEmpty()) }
        assertEquals(GROUPS[0], group("gain"))
        assertEquals(GROUPS[1], group("spnr-sf-min"))
        assertEquals(GROUPS[2], group("agc-gamma"))
        assertEquals(GROUPS[6], group("tlinear"))
    }
}
