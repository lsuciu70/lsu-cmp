// Two screens. Connect: board address (blank = search), connect button,
// status and event log. Once the board has answered, the dashboard, which
// fits on one screen:
//   - engine speed as the one hero figure, inside an arc gauge, with the
//     vehicle speed beside it;
//   - six tiles (value + meter bar) for the other watched PIDs;
//   - at the bottom: disconnect button, status line and the event log.
// Values older than 3 s are dimmed, so a stalled link is visible at a
// glance. Warning colors appear only when a value is out of its normal
// range, always with a word next to them (HIGH, HOT, LOW), never by color
// alone.
package io.github.lsuciu70.lsucmp

import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import kotlinx.coroutines.delay
import kotlin.math.cos
import kotlin.math.sin

/** The values shown; the board polls them (WATCH). */
val DASHBOARD = listOf(
    0x01 to 0x0C,  // engine speed
    0x01 to 0x0D,  // vehicle speed
    0x01 to 0x0B,  // intake manifold pressure (boost)
    0x01 to 0x05,  // coolant
    0x01 to 0x42,  // voltage
    0x01 to 0x11,  // throttle
    0x01 to 0x10,  // MAF
    0x01 to 0x04,  // load
)
private val VIN = 0x09 to 0x02
private const val STALE_MS = 3000L

// Dark chart chrome and ink (dataviz reference palette, dark mode).
private object Ink {
    val page = Color(0xFF0D0D0D)
    val surface = Color(0xFF1A1A19)
    val primary = Color(0xFFF0EFEC)
    val secondary = Color(0xFFC3C2B7)
    val muted = Color(0xFF898781)
    val accent = Color(0xFF3987E5)          // blue, dark step
    val track = Color(0xFF3987E5).copy(alpha = 0.22f)  // same ramp, dimmer
    val warning = Color(0xFFFAB219)
    val critical = Color(0xFFD03B3B)
}

/** An out-of-range state: its color always comes with its word. */
private class Alert(val word: String, val color: Color)

/** How one value is shown: meter range, decimals, and when to alert. */
private class Gauge(
    val pid: Int,
    val label: String,
    val unit: String,
    val min: Double,
    val max: Double,
    val decimals: Int,
    val alert: (Double) -> Alert? = { null },
)

private val RPM = Gauge(0x0C, "engine speed", "rpm", 0.0, 5000.0, 0, { if (it >= 4500) Alert("HIGH", Ink.warning) else null })
private val SPEED = Gauge(0x0D, "speed", "km/h", 0.0, 220.0, 0)
private val TILES = listOf(
    Gauge(0x0B, "manifold pressure", "kPa", 80.0, 250.0, 0),
    Gauge(0x05, "coolant", "°C", 0.0, 130.0, 0, { if (it >= 105) Alert("HOT", Ink.critical) else null }),
    Gauge(0x42, "voltage", "V", 10.0, 16.0, 1, {
        when {
            it < 12.0 -> Alert("LOW", Ink.warning)
            it > 15.0 -> Alert("HIGH", Ink.warning)
            else -> null
        }
    }),
    Gauge(0x11, "throttle", "%", 0.0, 100.0, 0),
    Gauge(0x10, "MAF", "g/s", 0.0, 150.0, 1),
    Gauge(0x04, "engine load", "%", 0.0, 100.0, 0),
)

class MainViewModel : ViewModel() {
    val client = CmpClient(viewModelScope)
    var host by mutableStateOf("")

    fun connect() = client.start(host, DASHBOARD, listOf(VIN))
    fun disconnect() = client.stop()
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            MaterialTheme(colorScheme = darkColorScheme(background = Ink.page, surface = Ink.surface)) {
                val vm: MainViewModel = viewModel()
                val state by vm.client.state.collectAsStateWithLifecycle()
                val active = state.phase != CmpClient.Phase.IDLE
                LaunchedEffect(active) {
                    if (active) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
                // The dashboard from the board's first answer until disconnect.
                if (active && state.status != null) Dashboard(vm, state) else ConnectScreen(vm, state, active)
            }
        }
    }
}

/** A reading of one PID: value if fresh enough to show, and whether it is. */
private class Reading(val value: Double?, val fresh: Boolean)

private fun CmpClient.State.reading(pid: Int, now: Long): Reading {
    val entry = values[0x01 shl 8 or pid] ?: return Reading(null, false)
    return Reading(entry.first.value, now - entry.second < STALE_MS)
}

private fun format(v: Double?, decimals: Int) =
    if (v == null) "–" else "%.${decimals}f".format(v)

@Composable
private fun ConnectScreen(vm: MainViewModel, state: CmpClient.State, active: Boolean) {
    Column(
        Modifier.fillMaxSize().background(Ink.page).safeDrawingPadding().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("lsu-cmp", color = Ink.primary, fontSize = 24.sp, fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(top = 24.dp, bottom = 8.dp))
        OutlinedTextField(
            value = vm.host,
            onValueChange = { vm.host = it },
            label = { Text("Board IP (blank: search)") },
            singleLine = true,
            enabled = !active,
            modifier = Modifier.fillMaxWidth(),
        )
        Button(onClick = { if (active) vm.disconnect() else vm.connect() }, modifier = Modifier.fillMaxWidth()) {
            Text(if (active) "Cancel" else "Connect")
        }
        StatusAndLog(state, Modifier.weight(1f))
    }
}

/** Status line and the event log, one entry per line, newest visible. */
@Composable
private fun StatusAndLog(state: CmpClient.State, modifier: Modifier) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(statusLine(state), color = Ink.secondary, fontSize = 11.sp, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        val listState = rememberLazyListState()
        LaunchedEffect(state.log.size) {
            if (state.log.isNotEmpty()) listState.animateScrollToItem(state.log.size - 1)
        }
        LazyColumn(state = listState, modifier = Modifier.fillMaxWidth().fillMaxHeight()) {
            items(state.log) { line ->
                Text(line, color = Ink.muted, fontSize = 10.sp, lineHeight = 13.sp,
                    fontFamily = FontFamily.Monospace, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@Composable
private fun Dashboard(vm: MainViewModel, state: CmpClient.State) {
    // Re-evaluate staleness even when no new value arrives.
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            now = System.currentTimeMillis()
            delay(500)
        }
    }

    Column(
        Modifier.fillMaxSize().background(Ink.page).safeDrawingPadding().padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            HeroGauge(RPM, state.reading(RPM.pid, now), Modifier.weight(1.45f))
            SpeedPanel(state, now, Modifier.weight(1f).aspectRatio(0.69f))
        }
        for (row in TILES.chunked(2)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (g in row) Tile(g, state.reading(g.pid, now), Modifier.weight(1f))
            }
        }
        ConnectionPanel(vm, state, Modifier.weight(1f))
    }
}

/** Engine speed: the one hero figure, inside a 240° arc meter. */
@Composable
private fun HeroGauge(g: Gauge, r: Reading, modifier: Modifier) {
    val alert = r.value?.let(g.alert)
    Box(
        modifier.aspectRatio(1f).background(Ink.surface, RoundedCornerShape(12.dp)).alpha(if (r.fresh) 1f else 0.4f),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.fillMaxSize().padding(14.dp)) {
            val stroke = 10.dp.toPx()
            val inset = stroke / 2
            val arcSize = Size(size.width - stroke, size.height - stroke)
            val topLeft = Offset(inset, inset)
            val start = 150f
            val sweep = 240f
            drawArc(Ink.track, start, sweep, false, topLeft, arcSize, style = Stroke(stroke, cap = StrokeCap.Round))
            // Red zone from 4500 rpm, as a thin outer band.
            val red = ((4500 - g.min) / (g.max - g.min)).toFloat()
            drawArc(Ink.critical.copy(alpha = 0.7f), start + sweep * red, sweep * (1 - red), false,
                Offset(inset - stroke * 0.9f, inset - stroke * 0.9f),
                Size(arcSize.width + stroke * 1.8f, arcSize.height + stroke * 1.8f),
                style = Stroke(2.dp.toPx()))
            val v = r.value
            if (v != null) {
                val f = ((v - g.min) / (g.max - g.min)).coerceIn(0.0, 1.0).toFloat()
                drawArc(alert?.color ?: Ink.accent, start, sweep * f, false, topLeft, arcSize,
                    style = Stroke(stroke, cap = StrokeCap.Round))
            }
            // Ticks every 1000 rpm.
            val cx = size.width / 2
            val cy = size.height / 2
            val rOuter = size.width / 2 - stroke * 1.6f
            for (k in 0..5) {
                val a = Math.toRadians((start + sweep * k / 5).toDouble())
                val r1 = rOuter - 6.dp.toPx()
                drawLine(Ink.muted, Offset(cx + rOuter * cos(a).toFloat(), cy + rOuter * sin(a).toFloat()),
                    Offset(cx + r1 * cos(a).toFloat(), cy + r1 * sin(a).toFloat()), 2.dp.toPx())
            }
        }
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(format(r.value, g.decimals), color = Ink.primary, fontSize = 50.sp, fontWeight = FontWeight.SemiBold)
            Text(g.unit, color = Ink.muted, fontSize = 14.sp)
            Text(alert?.word ?: "×1000: 0 – 5", color = alert?.color ?: Ink.muted, fontSize = 11.sp,
                fontWeight = if (alert != null) FontWeight.Bold else FontWeight.Normal)
        }
    }
}

/** Vehicle speed, plus the VIN under it. */
@Composable
private fun SpeedPanel(state: CmpClient.State, now: Long, modifier: Modifier) {
    val r = state.reading(SPEED.pid, now)
    Column(
        modifier.background(Ink.surface, RoundedCornerShape(12.dp)).padding(12.dp),
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(Modifier.alpha(if (r.fresh) 1f else 0.4f)) {
            Text(SPEED.label, color = Ink.secondary, fontSize = 13.sp)
            Row(verticalAlignment = Alignment.Bottom) {
                Text(format(r.value, 0), color = Ink.primary, fontSize = 44.sp, fontWeight = FontWeight.SemiBold)
                Text(" km/h", color = Ink.muted, fontSize = 14.sp, modifier = Modifier.padding(bottom = 8.dp))
            }
            Meter(r.value, SPEED, null)
        }
        state.values[VIN.first shl 8 or VIN.second]?.let { (v, _) ->
            Column {
                Text("VIN", color = Ink.muted, fontSize = 11.sp)
                Text(v.text, color = Ink.secondary, fontSize = 9.sp, fontFamily = FontFamily.Monospace,
                    maxLines = 1, softWrap = false)
            }
        }
    }
}

/** One value: label, figure, and a meter bar under it. */
@Composable
private fun Tile(g: Gauge, r: Reading, modifier: Modifier) {
    val alert = r.value?.let(g.alert)
    Column(
        modifier.background(Ink.surface, RoundedCornerShape(12.dp)).padding(horizontal = 12.dp, vertical = 8.dp)
            .alpha(if (r.fresh) 1f else 0.4f),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(g.label, color = Ink.secondary, fontSize = 12.sp, modifier = Modifier.weight(1f))
            if (alert != null) Text(alert.word, color = alert.color, fontSize = 11.sp, fontWeight = FontWeight.Bold)
        }
        Row(verticalAlignment = Alignment.Bottom) {
            Text(format(r.value, g.decimals), color = Ink.primary, fontSize = 30.sp, fontWeight = FontWeight.SemiBold)
            Text(" ${g.unit}", color = Ink.muted, fontSize = 13.sp, modifier = Modifier.padding(bottom = 5.dp))
        }
        Meter(r.value, g, alert)
    }
}

/** Thin bar: track in a dim step of the fill's hue, rounded ends. */
@Composable
private fun Meter(v: Double?, g: Gauge, alert: Alert?) {
    Canvas(Modifier.fillMaxWidth().height(6.dp)) {
        val h = size.height
        drawLine(Ink.track, Offset(h / 2, h / 2), Offset(size.width - h / 2, h / 2), h, StrokeCap.Round)
        if (v != null) {
            val f = ((v - g.min) / (g.max - g.min)).coerceIn(0.0, 1.0).toFloat()
            if (f > 0f) drawLine(alert?.color ?: Ink.accent, Offset(h / 2, h / 2),
                Offset(h / 2 + (size.width - h) * f, h / 2), h, StrokeCap.Round)
        }
    }
}

/** Bottom: disconnect button on its own row, then status and log. */
@Composable
private fun ConnectionPanel(vm: MainViewModel, state: CmpClient.State, modifier: Modifier) {
    Column(
        modifier.fillMaxWidth().background(Ink.surface, RoundedCornerShape(12.dp)).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Button(onClick = { vm.disconnect() }, modifier = Modifier.fillMaxWidth()) { Text("Disconnect") }
        StatusAndLog(state, Modifier.weight(1f))
    }
}

private fun statusLine(s: CmpClient.State): String = when (s.phase) {
    CmpClient.Phase.IDLE -> s.error?.let { "Error: $it" } ?: "Not connected"
    CmpClient.Phase.SEARCHING -> "Searching for the board…"
    CmpClient.Phase.UNREACHABLE -> "No answer from ${s.esp ?: "the board"}"
    CmpClient.Phase.CONNECTED -> buildString {
        append("${s.esp} · ${s.frames} frames · ${s.lost} lost")
        // Only worth a word when it is not the normal state.
        s.status?.let { if (it.canStateName != "running") append(" · CAN ${it.canStateName}") }
        if (s.refused > 0) append(" · ${s.refused} refused")
    }
}
