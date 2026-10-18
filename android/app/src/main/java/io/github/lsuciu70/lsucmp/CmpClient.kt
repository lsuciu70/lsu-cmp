// Talks to the lsu-cmp ESP32 the way tools/cmp_client does: resends
// SUBSCRIBE every second and WATCH (the PIDs the board polls for us) every
// 4 s, asks for one-off values (VIN) until they arrive, and decodes the CMP
// stream into OBD2 values. Publishes everything as one StateFlow.
//
// Finding the board: with no host given, SUBSCRIBE goes out as a broadcast
// on every local network (e.g. the phone's hotspot, where the board's IP is
// whatever the phone handed out). The board answers a SUBSCRIBE with STATUS
// from its control port; that answer tells the app the board's address.
package io.github.lsuciu70.lsucmp

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

class CmpClient(private val scope: CoroutineScope) {

    enum class Phase { IDLE, SEARCHING, CONNECTED, UNREACHABLE }

    data class State(
        val phase: Phase = Phase.IDLE,
        val esp: String? = null,
        val status: Lsu.Status? = null,
        /** Latest answer per (mode shl 8 or pid), with its arrival time. */
        val values: Map<Int, Pair<ObdValue, Long>> = emptyMap(),
        val frames: Long = 0,
        val lost: Long = 0,
        val refused: Long = 0,
        val lastRefusal: String? = null,
        val error: String? = null,
        /** Connection events, oldest first, "HH:mm:ss  text". */
        val log: List<String> = emptyList(),
        /** An ECU answered within the last CAR_SILENT_MS (ignition on). */
        val carAnswering: Boolean = false,
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state
    private var job: Job? = null
    private val clock = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.ROOT)

    private fun log(text: String) = _state.update {
        it.copy(log = (it.log + "${clock.format(java.util.Date())}  $text").takeLast(LOG_LINES))
    }

    /**
     * Starts talking to [host] (an IP or name), or searches the local
     * networks when it is blank. [watch]: mode 01 (mode, pid) pairs the
     * board polls as fast as the bus allows. [oneOff]: requested every 2 s
     * until answered (e.g. the VIN).
     */
    fun start(host: String, watch: List<Pair<Int, Int>>, oneOff: List<Pair<Int, Int>>) {
        stop()
        _state.update { State(phase = Phase.SEARCHING, log = it.log) }
        log(if (host.isBlank()) "searching for the board" else "connecting to ${host.trim()}")
        job = scope.launch(Dispatchers.IO) {
            try {
                run(host.trim(), watch, oneOff)
            } catch (e: Exception) {
                _state.update { it.copy(phase = Phase.IDLE, error = e.message ?: e.toString()) }
                log("error: ${e.message ?: e}")
            }
        }
    }

    fun stop() {
        if (job == null) return
        job?.cancel()
        job = null
        _state.update { it.copy(phase = Phase.IDLE) }
        log("disconnected")
    }

    private suspend fun run(host: String, watch: List<Pair<Int, Int>>, oneOff: List<Pair<Int, Int>>) {
        val fixed: InetAddress? = if (host.isEmpty()) null else InetAddress.getByName(host)
        var esp: InetAddress? = fixed
        val socket = DatagramSocket(null).apply {
            reuseAddress = true
            broadcast = true
            soTimeout = 20
            bind(InetSocketAddress(0))
        }
        val decoder = ObdDecoder()
        val pending = oneOff.toMutableList()
        val buf = ByteArray(2048)
        val packet = DatagramPacket(buf, buf.size)
        var nextSubscribe = 0L
        var nextWatch = 0L
        var nextRequest = 0L
        var lastStatus = System.currentTimeMillis()
        var lastAnswer = 0L
        var expectedSeq = -1

        fun send(data: ByteArray, to: InetAddress) =
            socket.send(DatagramPacket(data, data.size, to, Lsu.CONTROL_PORT))

        try {
            while (currentCoroutineContext().isActive) {
                val now = System.currentTimeMillis()
                if (now >= nextSubscribe) {
                    val msg = Lsu.subscribe(now * 1_000_000)
                    val target = esp
                    if (target != null) send(msg, target) else broadcastAddresses().forEach { send(msg, it) }
                    nextSubscribe = now + 1000
                }
                val reachable = esp != null && now - lastStatus < 3000
                if (reachable && watch.isNotEmpty() && now >= nextWatch) {
                    send(Lsu.watch(watch), esp!!)
                    nextWatch = now + Lsu.WATCH_RENEW_MS
                }
                if (reachable && pending.isNotEmpty() && now >= nextRequest) {
                    pending.forEach { (mode, pid) -> send(Lsu.request(mode, pid), esp!!) }
                    nextRequest = now + 2000
                }
                if (esp != null && now - lastStatus >= 3000) {
                    // Searching again lets a board that got a new IP be found.
                    if (fixed == null && now - lastStatus >= 6000) esp = null
                    val phase = if (esp == null) Phase.SEARCHING else Phase.UNREACHABLE
                    if (phase != _state.value.phase)
                        log(if (esp == null) "searching for the board again" else "no answer from the board")
                    _state.update { it.copy(phase = phase) }
                }
                if (_state.value.carAnswering && now - lastAnswer >= CAR_SILENT_MS) {
                    _state.update { it.copy(carAnswering = false) }
                    log("car silent (ignition off?)")
                }

                try {
                    socket.receive(packet)
                } catch (_: SocketTimeoutException) {
                    continue
                }
                val from = packet.address
                val n = packet.length
                if (esp == null) {
                    // Only a STATUS identifies the board.
                    if (packet.port != Lsu.CONTROL_PORT || Lsu.decodeStatus(buf, n) == null) continue
                    esp = from
                    log("board found at ${from.hostAddress}")
                } else if (from != esp) {
                    continue
                }

                Lsu.decodeStatus(buf, n)?.let { st ->
                    lastStatus = System.currentTimeMillis()
                    // Uptime going back: the board restarted. Start the
                    // counters over, so its restart does not show as losses.
                    val old = _state.value
                    val restarted = old.status?.let { st.uptimeS < it.uptimeS } == true
                    if (restarted) {
                        expectedSeq = -1
                        nextWatch = 0  // the restarted board knows nothing of our list
                        log("board restarted")
                    }
                    if (old.phase != Phase.CONNECTED)
                        log("connected: CAN ${st.canStateName}, ${st.bitrateKbps} kbit/s")
                    else if (old.status != null && st.canState != old.status.canState)
                        log("CAN ${old.status.canStateName} -> ${st.canStateName}")
                    if (old.status != null && !restarted && st.busErrors > old.status.busErrors)
                        log("CAN bus errors: ${st.busErrors}")
                    _state.update {
                        val base = if (restarted) it.copy(frames = 0, lost = 0, refused = 0, lastRefusal = null) else it
                        base.copy(phase = Phase.CONNECTED, esp = from.hostAddress, status = st, error = null)
                    }
                }
                Lsu.decodeRequestResult(buf, n)?.let { r ->
                    // The board forgot us (restart, or no SUBSCRIBE for 5 s):
                    // subscribe again now instead of at the next second.
                    if (r.result == Lsu.REQUEST_NOT_SUBSCRIBED) {
                        nextSubscribe = 0
                        nextWatch = 0
                    }
                    if (r.result != Lsu.REQUEST_SENT) {
                        val text = "%02X %02X: %s".format(r.mode, r.pid, r.resultName)
                        if (text != _state.value.lastRefusal) log("request refused, $text")
                        _state.update { it.copy(refused = it.refused + 1, lastRefusal = text) }
                    }
                }
                Cmp.decode(buf, n)?.let { pkt ->
                    var lost = if (expectedSeq >= 0) (pkt.sequenceCounter - expectedSeq) and 0xFFFF else 0
                    // A jump of more than half the counter is the sequence
                    // starting over (new subscription), not 30000+ losses.
                    if (lost >= 0x8000) lost = 0
                    expectedSeq = (pkt.sequenceCounter + 1) and 0xFFFF
                    val answer = decoder.feed(pkt.frame)
                    if (answer != null) {
                        lastAnswer = System.currentTimeMillis()
                        if (!_state.value.carAnswering) {
                            _state.update { it.copy(carAnswering = true) }
                            log("car answering (ignition on)")
                        }
                    }
                    if (lost > 0) log("$lost packet(s) lost")
                    if (answer != null && answer.mode == Obd.MODE_VEHICLE_INFO &&
                        pending.remove(answer.mode to answer.pid)
                    ) log("${answer.name}: ${answer.text}")
                    _state.update {
                        it.copy(
                            frames = it.frames + 1,
                            lost = it.lost + lost,
                            values = if (answer == null) it.values
                            else it.values + ((answer.mode shl 8 or answer.pid) to
                                (answer to System.currentTimeMillis())),
                        )
                    }
                }
            }
        } finally {
            esp?.let { runCatching { send(Lsu.unsubscribe(), it) } }
            socket.close()
            _state.update { it.copy(phase = Phase.IDLE) }
        }
    }

    private companion object {
        const val LOG_LINES = 200
        const val CAR_SILENT_MS = 3000L
    }

    /** Broadcast address of every IPv4 network the phone is on. */
    private fun broadcastAddresses(): List<InetAddress> =
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.interfaceAddresses }
            .filter { it.address is Inet4Address }
            .mapNotNull { it.broadcast }
            .distinct()
}
