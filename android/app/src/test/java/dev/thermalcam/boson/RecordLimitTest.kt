package dev.thermalcam.boson

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RecordLimitTest {
    @Test fun capsAt3point5GB() = assertEquals(3_500_000_000L, recordLimitBytes(100_000_000_000L))

    @Test fun keeps200MBFree() = assertEquals(800_000_000L, recordLimitBytes(1_000_000_000L))

    @Test fun noRoom() = assertTrue(recordLimitBytes(150_000_000L) <= 0)
}
