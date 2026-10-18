// OBD2 answers from the CAN frames of the CMP stream: single frames, and
// multi-frame (ISO-TP) answers such as the VIN, reassembled per ECU. Port
// of tools/cmp_client/obd_decoder.cpp; IDs from lib/LsuObd/src/lsu_obd.h,
// PID formulas from SAE J1979.
package io.github.lsuciu70.lsucmp

/** One decoded OBD2 answer. [value] is null for text answers (VIN). */
data class ObdValue(
    val ecu: Int,
    val mode: Int,
    val pid: Int,
    val name: String,
    val value: Double?,
    val unit: String,
    val text: String,
)

object Obd {
    const val MODE_CURRENT_DATA = 0x01
    const val MODE_VEHICLE_INFO = 0x09
    private const val FIRST_RESPONSE_ID = 0x7E8
    private const val LAST_RESPONSE_ID = 0x7EF

    class Formula(
        val name: String,
        val bytes: Int,
        val scale: Double,
        val offset: Double,
        val unit: String,
    )

    /** Mode 01 PIDs with a formula: value = raw * scale + offset. */
    val MODE01 = mapOf(
        0x04 to Formula("engine load", 1, 100.0 / 255, 0.0, "%"),
        0x05 to Formula("coolant temperature", 1, 1.0, -40.0, "°C"),
        0x0B to Formula("intake manifold pressure", 1, 1.0, 0.0, "kPa"),
        0x0C to Formula("engine speed", 2, 0.25, 0.0, "rpm"),
        0x0D to Formula("vehicle speed", 1, 1.0, 0.0, "km/h"),
        0x0F to Formula("intake air temperature", 1, 1.0, -40.0, "°C"),
        0x10 to Formula("MAF air flow", 2, 0.01, 0.0, "g/s"),
        0x11 to Formula("throttle position", 1, 100.0 / 255, 0.0, "%"),
        0x1F to Formula("run time since start", 2, 1.0, 0.0, "s"),
        0x2F to Formula("fuel level", 1, 100.0 / 255, 0.0, "%"),
        0x33 to Formula("barometric pressure", 1, 1.0, 0.0, "kPa"),
        0x42 to Formula("control module voltage", 2, 0.001, 0.0, "V"),
        0x46 to Formula("ambient air temperature", 1, 1.0, -40.0, "°C"),
        0x49 to Formula("accelerator pedal D", 1, 100.0 / 255, 0.0, "%"),
        0x4A to Formula("accelerator pedal E", 1, 100.0 / 255, 0.0, "%"),
        0x5C to Formula("engine oil temperature", 1, 1.0, -40.0, "°C"),
    )

    fun isResponseId(id: Int, extended: Boolean) =
        !extended && id in FIRST_RESPONSE_ID..LAST_RESPONSE_ID
}

class ObdDecoder {
    private class Pending(val length: Int) {
        var nextSeq = 1
        val data = ArrayList<Byte>()
    }

    private val pending = HashMap<Int, Pending>()  // by response ID

    /** Feeds one CAN frame; returns the answer it completes, if any. */
    fun feed(f: CanFrame): ObdValue? {
        if (!Obd.isResponseId(f.id, f.extended) || f.rtr || f.dataLength < 1) return null
        val d = f.data
        val n = f.dataLength
        when (u8(d, 0) shr 4) {
            0 -> {  // single frame
                val len = u8(d, 0) and 0x0F
                if (len == 0 || len + 1 > n) return null
                pending.remove(f.id)
                return describe(f.id, d.copyOfRange(1, 1 + len))
            }
            1 -> {  // first frame
                if (n < 2) return null
                val p = Pending(((u8(d, 0) and 0x0F) shl 8) or u8(d, 1))
                for (i in 2 until n) p.data.add(d[i])
                pending[f.id] = p
                return null
            }
            2 -> {  // consecutive frame
                val p = pending[f.id] ?: return null
                if ((u8(d, 0) and 0x0F) != p.nextSeq) {  // lost a frame: give up
                    pending.remove(f.id)
                    return null
                }
                p.nextSeq = (p.nextSeq + 1) and 0x0F
                for (i in 1 until n) p.data.add(d[i])
                if (p.data.size < p.length) return null
                pending.remove(f.id)
                return describe(f.id, p.data.subList(0, p.length).toByteArray())
            }
            else -> return null
        }
    }

    private fun describe(ecu: Int, r: ByteArray): ObdValue? {
        if (r.size < 2 || u8(r, 0) < 0x40 || u8(r, 0) == 0x7F) return null  // negative: skip
        val mode = u8(r, 0) - 0x40
        val pid = u8(r, 1)
        val v = r.copyOfRange(2, r.size)
        if (mode == Obd.MODE_CURRENT_DATA) {
            val f = Obd.MODE01[pid]
            if (f != null && v.size >= f.bytes) {
                val raw = if (f.bytes == 1) u8(v, 0).toDouble() else u16(v, 0).toDouble()
                val value = raw * f.scale + f.offset
                return ObdValue(ecu, mode, pid, f.name, value, f.unit, format(value))
            }
        }
        if (mode == Obd.MODE_VEHICLE_INFO && pid == 0x02 && v.isNotEmpty()) {
            // First value byte: number of data items.
            val vin = v.drop(1).filter { it in 0x20..0x7E }.map { it.toInt().toChar() }.joinToString("")
            return ObdValue(ecu, mode, pid, "VIN", null, "", vin)
        }
        return ObdValue(ecu, mode, pid, "PID %02X %02X".format(mode, pid), null, "",
            v.joinToString(" ") { "%02X".format(it) })
    }

    private fun format(v: Double) =
        if (v == Math.floor(v) && Math.abs(v) < 1e9) "%.0f".format(v) else "%.1f".format(v)
}
