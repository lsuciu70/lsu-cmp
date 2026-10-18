// The same golden bytes as test/test_codec and test/test_protocol, so the
// Kotlin port reads exactly what the firmware writes.
package io.github.lsuciu70.lsucmp

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ProtocolTest {
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    // test_codec: test_standard_frame (0x7E8, 11-bit ID in bits 18..28).
    private val standardFrame = bytes(
        0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x12, 0x34,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0x01, 0x00, 0x18,
        0x00, 0x00, 0x00, 0x00,
        0x1F, 0xA0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x08,
        0x10, 0x14, 0x49, 0x02, 0x01, 0x57, 0x56, 0x57,
    )

    @Test fun decodesStandardFrame() {
        val p = Cmp.decode(standardFrame, standardFrame.size)
        assertNotNull(p)
        p!!
        assertEquals(0x1234, p.sequenceCounter)
        assertEquals(0x0102030405060708L, p.timestampNs)
        assertEquals(0x02, p.commonFlags)
        assertEquals(0x7E8, p.frame.id)
        assertFalse(p.frame.extended)
        assertFalse(p.frame.rtr)
        assertEquals(8, p.frame.dlc)
        assertEquals(8, p.frame.dataLength)
        assertArrayEquals(bytes(0x10, 0x14, 0x49, 0x02, 0x01, 0x57, 0x56, 0x57), p.frame.data)
    }

    @Test fun decodesOld11BitEncoding() {
        val old = standardFrame.copyOf()
        bytes(0x00, 0x00, 0x07, 0xE8).copyInto(old, 28)
        assertEquals(0x7E8, Cmp.decode(old, old.size)!!.frame.id)
    }

    @Test fun decodesExtendedRtrFrame() {
        val p = standardFrame.copyOf(40)
        bytes(0xD8, 0xDA, 0xF1, 0x10).copyInto(p, 28)  // IDE | RTR | 0x18DAF110
        p[23] = 16  // payload length: no data
        p[39] = 0
        val d = Cmp.decode(p, p.size)!!
        assertEquals(0x18DAF110, d.frame.id)
        assertTrue(d.frame.extended)
        assertTrue(d.frame.rtr)
        assertEquals(0, d.frame.dataLength)
    }

    @Test fun rejectsInvalidPackets() {
        assertNull(Cmp.decode(standardFrame, standardFrame.size - 1))
        assertNull(Cmp.decode(standardFrame, 39))
        val v2 = standardFrame.copyOf().also { it[0] = 2 }
        assertNull(Cmp.decode(v2, v2.size))
        assertNull(Cmp.decode("LSUA".toByteArray() + ByteArray(36), 40))
    }

    @Test fun encodesControlMessages() {
        assertArrayEquals(
            bytes('L'.code, 'S'.code, 'U'.code, 'S'.code, 1, 2, 3, 4, 5, 6, 7, 8),
            Lsu.subscribe(0x0102030405060708L),
        )
        assertArrayEquals("LSUU".toByteArray(), Lsu.unsubscribe())
        assertArrayEquals(bytes('L'.code, 'S'.code, 'U'.code, 'R'.code, 0x01, 0x0C), Lsu.request(1, 0x0C))
        // test_protocol: test_watch_round_trip
        assertArrayEquals(
            bytes('L'.code, 'S'.code, 'U'.code, 'W'.code, 3, 0x01, 0x0C, 0x01, 0x0D, 0x01, 0x42),
            Lsu.watch(listOf(1 to 0x0C, 1 to 0x0D, 1 to 0x42)),
        )
        assertArrayEquals(bytes('L'.code, 'S'.code, 'U'.code, 'W'.code, 0), Lsu.watch(emptyList()))
    }

    // test_protocol: test_status_round_trip values.
    @Test fun decodesStatus() {
        val p = "LSUA".toByteArray() + bytes(
            1, 2, 0x01, 0xF4,
            0x00, 0x01, 0x86, 0xA0,  // 100000
            0x00, 0x01, 0x82, 0xB8,  // 99000
            0, 0, 0, 7, 0, 0, 0, 3, 0, 0, 0, 12, 0, 0, 0, 5, 0, 0, 0, 1,
            0x00, 0x00, 0x0E, 0x10,  // 3600
        )
        val s = Lsu.decodeStatus(p, p.size)!!
        assertEquals("bus-off", s.canStateName)
        assertEquals(500, s.bitrateKbps)
        assertEquals(100000L, s.canRx)
        assertEquals(99000L, s.sent)
        assertEquals(12L, s.busErrors)
        assertEquals(3600L, s.uptimeS)
        assertNull(Lsu.decodeStatus(p, p.size - 1))
    }

    @Test fun decodesRequestResult() {
        val p = bytes('L'.code, 'S'.code, 'U'.code, 'K'.code, 0x09, 0x02, 3)
        val r = Lsu.decodeRequestResult(p, p.size)!!
        assertEquals(0x09, r.mode)
        assertEquals(0x02, r.pid)
        assertEquals("busy", r.resultName)
    }
}

class ObdDecoderTest {
    private fun frame(id: Int, vararg d: Int) =
        CanFrame(id, false, false, 8, ByteArray(8) { d.getOrElse(it) { 0xAA }.toByte() }, 8)

    @Test fun decodesSingleFrameValues() {
        val dec = ObdDecoder()
        val rpm = dec.feed(frame(0x7E8, 0x04, 0x41, 0x0C, 0x0C, 0x82))!!
        assertEquals("engine speed", rpm.name)
        assertEquals(800.5, rpm.value!!, 1e-9)
        assertEquals(83.0, dec.feed(frame(0x7E8, 0x03, 0x41, 0x05, 0x7B))!!.value!!, 1e-9)
        assertEquals(14.17, dec.feed(frame(0x7E8, 0x04, 0x41, 0x42, 0x37, 0x5A))!!.value!!, 1e-9)
    }

    @Test fun ignoresRequestsAndNegativeAnswers() {
        val dec = ObdDecoder()
        assertNull(dec.feed(frame(0x7DF, 0x02, 0x01, 0x0C)))
        assertNull(dec.feed(frame(0x7E8, 0x03, 0x7F, 0x09, 0x12)))
    }

    @Test fun reassemblesVin() {
        val dec = ObdDecoder()
        assertNull(dec.feed(frame(0x7E8, 0x10, 0x14, 0x49, 0x02, 0x01, 'W'.code, 'V'.code, 'W'.code)))
        assertNull(dec.feed(frame(0x7E8, 0x21, 'Z'.code, 'Z'.code, 'Z'.code, 'A'.code, 'U'.code, 'Z'.code, 'H'.code)))
        val vin = dec.feed(frame(0x7E8, 0x22, 'W'.code, '1'.code, '2'.code, '3'.code, '4'.code, '5'.code, '6'.code))!!
        assertEquals("VIN", vin.name)
        assertEquals("WVWZZZAUZHW123456", vin.text)
    }

    @Test fun dropsMultiFrameWithLostFrame() {
        val dec = ObdDecoder()
        dec.feed(frame(0x7E8, 0x10, 0x14, 0x49, 0x02, 0x01, 'W'.code, 'V'.code, 'W'.code))
        assertNull(dec.feed(frame(0x7E8, 0x22, 'W'.code, '1'.code)))  // 0x21 missing
        assertNull(dec.feed(frame(0x7E8, 0x21, 'Z'.code)))             // nothing pending any more
    }
}
