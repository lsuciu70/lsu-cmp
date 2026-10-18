// Records what the board sends to the phone as a pcap file in
// Download/lsu-cmp/, in the same format as cmp_client --record
// (tools/cmp_client/pcap_file.cpp): classic pcap, nanosecond timestamps,
// LINKTYPE_RAW (bare IPv4 + UDP headers built here). The files play back
// with cmp_replay and open in Wireshark.
package io.github.lsuciu70.lsucmp

import android.content.ContentResolver
import android.content.ContentValues
import android.net.Uri
import android.provider.MediaStore
import java.io.BufferedOutputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

class PcapRecorder(private val resolver: ContentResolver) {
    private var out: OutputStream? = null
    private var ipId = 0

    var uri: Uri? = null
        private set
    var name: String? = null
        private set
    var packets = 0L
        private set

    val isRecording get() = out != null

    /** Creates Download/lsu-cmp/[fileName] and writes the pcap header. */
    @Synchronized
    fun start(fileName: String) {
        stop()
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, fileName)
            put(MediaStore.Downloads.MIME_TYPE, "application/vnd.tcpdump.pcap")
            put(MediaStore.Downloads.RELATIVE_PATH, "Download/lsu-cmp")
            put(MediaStore.Downloads.IS_PENDING, 1)  // hidden until complete
        }
        val u = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: error("cannot create $fileName")
        out = BufferedOutputStream(resolver.openOutputStream(u) ?: error("cannot open $fileName"))
        uri = u
        name = fileName
        packets = 0
        val h = le(24)
        h.putInt(MAGIC_NANOS).putShort(2).putShort(4).putInt(0).putInt(0).putInt(SNAP_LEN).putInt(LINK_RAW)
        out!!.write(h.array())
    }

    /** One datagram from the board ([srcIp]:[srcPort]) to the phone. */
    @Synchronized
    fun write(timeNs: Long, srcIp: ByteArray, srcPort: Int, dstIp: ByteArray, dstPort: Int,
              data: ByteArray, length: Int) {
        val o = out ?: return
        val total = 20 + 8 + length
        val rec = le(16)
        rec.putInt((timeNs / 1_000_000_000).toInt()).putInt((timeNs % 1_000_000_000).toInt())
            .putInt(total).putInt(total)
        o.write(rec.array())

        val hdr = ByteBuffer.allocate(28).order(ByteOrder.BIG_ENDIAN)
        hdr.put(0x45).put(0).putShort(total.toShort()).putShort((ipId++).toShort()).putShort(0)
            .put(64).put(17).putShort(0).put(srcIp).put(dstIp)
        val ip = hdr.array()
        val sum = checksum(ip, 20)
        ip[10] = (sum shr 8).toByte()
        ip[11] = sum.toByte()
        hdr.position(20)
        hdr.putShort(srcPort.toShort()).putShort(dstPort.toShort()).putShort((8 + length).toShort()).putShort(0)
        o.write(ip)
        o.write(data, 0, length)
        packets++
    }

    @Synchronized
    fun flush() {
        out?.flush()
    }

    /** Closes the file and makes it visible; returns its name, or null. */
    @Synchronized
    fun stop(): String? {
        val o = out ?: return null
        runCatching { o.close() }
        out = null
        uri?.let {
            resolver.update(it, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
        }
        return name
    }

    private fun le(n: Int) = ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN)

    private fun checksum(p: ByteArray, n: Int): Int {
        var s = 0
        for (i in 0 until n step 2) s += ((p[i].toInt() and 0xFF) shl 8) or (p[i + 1].toInt() and 0xFF)
        while (s shr 16 != 0) s = (s and 0xFFFF) + (s shr 16)
        return s.inv() and 0xFFFF
    }

    private companion object {
        const val MAGIC_NANOS = 0xA1B23C4D.toInt()
        const val SNAP_LEN = 65535
        const val LINK_RAW = 101
    }
}
