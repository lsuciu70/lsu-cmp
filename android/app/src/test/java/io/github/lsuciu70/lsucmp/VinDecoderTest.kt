package io.github.lsuciu70.lsucmp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class VinDecoderTest {
    @Test fun decodesTheGolf() {
        val d = VinDecoder.decode("WVWZZZAUZJP001831", currentYear = 2026).toMap()
        assertEquals("Volkswagen (passenger car)", d["Manufacturer"])
        assertEquals("Germany", d["Country"])
        assertEquals("Golf VII (MQB)", d["Model"])
        assertEquals("2018", d["Model year"])
        assertEquals("Mosel / Zwickau", d["Plant"])
        assertEquals("001831", d["Serial number"])
    }

    @Test fun modelYearCycle() {
        assertEquals(2018, VinDecoder.modelYear('J', 2026))
        assertEquals(2010, VinDecoder.modelYear('A', 2026))
        assertEquals(2027, VinDecoder.modelYear('V', 2026))  // next year's models
        assertEquals(1997, VinDecoder.modelYear('V', 2025))
    }

    @Test fun leavesOutWhatItDoesNotKnow() {
        // Porsche: manufacturer and country, but no VW model or plant table.
        val d = VinDecoder.decode("WP0ZZZY1ZPSA00001", currentYear = 2026).toMap()
        assertEquals("Porsche (passenger car)", d["Manufacturer"])
        assertEquals(null, d["Model"])
        assertEquals(null, d["Plant"])
    }

    @Test fun rejectsInvalidVins() {
        assertTrue(VinDecoder.decode("WVWZZZAUZJP00183", 2026).isEmpty())   // 16 chars
        assertTrue(VinDecoder.decode("WVWZZZAUZJP0O1831", 2026).isEmpty())  // letter O
        assertTrue(VinDecoder.decode("", 2026).isEmpty())
    }
}
