package com.example.gnssble.data.orbit

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TleParserTest {
    @Test
    fun parsesValidGpsTle() {
        val records = TleParser.parse(
            """
            GPS BIIR-2  (PRN 13)
            1 24876U 97035A   24123.50000000 -.00000012  00000-0  00000+0 0  9999
            2 24876  55.0000 120.0000 0100000  40.0000  15.0000  2.00560000    01
            """.trimIndent(),
        )

        assertEquals(1, records.size)
        assertEquals(24876, records.single().noradId)
        assertEquals(2.0056, records.single().meanMotionRevsPerDay, 0.000001)
        assertEquals(0.01, records.single().eccentricity, 0.000001)
    }

    @Test
    fun skipsMalformedRecords() {
        val records = TleParser.parse("GPS\nnot-a-line\nnot-a-line")

        assertTrue(records.isEmpty())
    }
}
