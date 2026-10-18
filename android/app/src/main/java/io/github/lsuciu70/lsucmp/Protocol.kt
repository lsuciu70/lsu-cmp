// The wire formats the app speaks with the ESP32, ported from the C++
// sources shared by the firmware and cmp_client:
//   lib/LsuProtocol/src/lsu_protocol.h  own control protocol (port 12671)
//   lib/CmpCodec/src/cmp_decoder.cpp    ASAM CMP CAN packets (port 12670)
// All multi-byte fields are big-endian.
package io.github.lsuciu70.lsucmp

object Lsu {
    const val DATA_PORT = 12670     // CMP packets, ESP32 -> app
    const val CONTROL_PORT = 12671  // own protocol, both ways

    private val MAGIC_SUBSCRIBE = "LSUS".toByteArray()
    private val MAGIC_UNSUBSCRIBE = "LSUU".toByteArray()
    private val MAGIC_STATUS = "LSUA".toByteArray()
    private val MAGIC_REQUEST = "LSUR".toByteArray()
    private val MAGIC_REQUEST_RESULT = "LSUK".toByteArray()
    private const val STATUS_SIZE = 40
    private const val STATUS_VERSION = 1
    private const val REQUEST_RESULT_SIZE = 7

    val CAN_STATE_NAMES = listOf("stopped", "running", "bus-off", "recovering")
    val REQUEST_RESULT_NAMES = listOf(
        "sent", "not allowed", "not subscribed", "busy", "transmit disabled", "transmit failed",
    )
    const val REQUEST_SENT = 0
    const val REQUEST_NOT_SUBSCRIBED = 2

    data class Status(
        val canState: Int,
        val bitrateKbps: Int,
        val canRx: Long,
        val sent: Long,
        val queueFull: Long,
        val sendErrors: Long,
        val busErrors: Long,
        val rxMissed: Long,
        val rxOverrun: Long,
        val uptimeS: Long,
    ) {
        val canStateName get() = CAN_STATE_NAMES.getOrElse(canState) { "?" }
    }

    data class RequestResult(val mode: Int, val pid: Int, val result: Int) {
        val resultName get() = REQUEST_RESULT_NAMES.getOrElse(result) { "?" }
    }

    fun subscribe(laptopTimeNs: Long): ByteArray =
        MAGIC_SUBSCRIBE + ByteArray(8).also { putLong(it, 0, laptopTimeNs) }

    fun unsubscribe(): ByteArray = MAGIC_UNSUBSCRIBE.copyOf()

    fun request(mode: Int, pid: Int): ByteArray =
        MAGIC_REQUEST + byteArrayOf(mode.toByte(), pid.toByte())

    fun decodeStatus(p: ByteArray, n: Int): Status? {
        if (n != STATUS_SIZE || !startsWith(p, n, MAGIC_STATUS) || u8(p, 4) != STATUS_VERSION)
            return null
        return Status(
            canState = u8(p, 5),
            bitrateKbps = u16(p, 6),
            canRx = u32(p, 8),
            sent = u32(p, 12),
            queueFull = u32(p, 16),
            sendErrors = u32(p, 20),
            busErrors = u32(p, 24),
            rxMissed = u32(p, 28),
            rxOverrun = u32(p, 32),
            uptimeS = u32(p, 36),
        )
    }

    fun decodeRequestResult(p: ByteArray, n: Int): RequestResult? {
        if (n != REQUEST_RESULT_SIZE || !startsWith(p, n, MAGIC_REQUEST_RESULT)) return null
        return RequestResult(u8(p, 4), u8(p, 5), u8(p, 6))
    }

    private fun startsWith(p: ByteArray, n: Int, magic: ByteArray) =
        n >= magic.size && magic.indices.all { p[it] == magic[it] }
}

/** One classic CAN frame; [data] holds [dataLength] valid bytes. */
class CanFrame(
    val id: Int,
    val extended: Boolean,
    val rtr: Boolean,
    val dlc: Int,
    val data: ByteArray,
    val dataLength: Int,
)

class CmpPacket(
    val sequenceCounter: Int,
    val timestampNs: Long,
    val commonFlags: Int,
    val frame: CanFrame,
)

object Cmp {
    private const val HEADER = 8
    private const val MESSAGE_HEADER = 16
    private const val CAN_HEADER = 16
    private const val MAX_DATA = 8
    private const val VERSION = 1
    private const val MESSAGE_TYPE_DATA = 1
    private const val PAYLOAD_TYPE_CAN = 1

    private const val ID_MASK = 0x1FFFFFFF
    private const val ID_RTR = 1 shl 30
    private const val ID_IDE = 1 shl 31
    // 11-bit IDs: bits 18..28 (Wireshark default, this firmware since
    // 2026-10-17) or bits 0..10 (openDAQ, older recordings). See
    // lib/CmpCodec/src/cmp_encoder.h.
    private const val ID11_SHIFT = 18
    private const val ID11_MASK = 0x7FF shl ID11_SHIFT
    private const val ID11_MASK_OLD = 0x7FF

    /** Decodes one CMP CAN data packet; null if it is not one. */
    fun decode(p: ByteArray, n: Int): CmpPacket? {
        val min = HEADER + MESSAGE_HEADER + CAN_HEADER
        if (n < min) return null
        if (u8(p, 0) != VERSION || u8(p, 4) != MESSAGE_TYPE_DATA) return null
        val m = HEADER
        if (u8(p, m + 13) != PAYLOAD_TYPE_CAN) return null
        val payloadLength = u16(p, m + 14)
        val c = m + MESSAGE_HEADER
        val dataLength = u8(p, c + 15)
        if (dataLength > MAX_DATA || payloadLength != CAN_HEADER + dataLength ||
            n < min + dataLength
        ) return null

        val rawId = u32(p, c + 4).toInt()
        val extended = rawId and ID_IDE != 0
        val id = when {
            extended -> rawId and ID_MASK
            rawId and ID11_MASK != 0 -> (rawId and ID11_MASK) ushr ID11_SHIFT
            else -> rawId and ID11_MASK_OLD
        }
        val data = ByteArray(MAX_DATA)
        System.arraycopy(p, c + CAN_HEADER, data, 0, dataLength)
        return CmpPacket(
            sequenceCounter = u16(p, 6),
            timestampNs = (u32(p, m) shl 32) or u32(p, m + 4),
            commonFlags = u8(p, m + 12),
            frame = CanFrame(id, extended, rawId and ID_RTR != 0, u8(p, c + 14), data, dataLength),
        )
    }
}

internal fun u8(p: ByteArray, i: Int) = p[i].toInt() and 0xFF
internal fun u16(p: ByteArray, i: Int) = (u8(p, i) shl 8) or u8(p, i + 1)
internal fun u32(p: ByteArray, i: Int) = (u16(p, i).toLong() shl 16) or u16(p, i + 2).toLong()

internal fun putLong(p: ByteArray, i: Int, v: Long) {
    for (k in 0 until 8) p[i + k] = (v ushr (56 - 8 * k)).toByte()
}
