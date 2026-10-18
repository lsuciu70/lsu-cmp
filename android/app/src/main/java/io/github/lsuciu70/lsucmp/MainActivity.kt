// Phone screen: board address (blank = search), connect button, link
// status, and one large tile per OBD2 value. Values older than 3 s are
// dimmed, so a stalled link is visible at a glance.
package io.github.lsuciu70.lsucmp

import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import kotlinx.coroutines.delay

/** The values shown, in this order; the board polls them (WATCH). */
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
            MaterialTheme(colorScheme = darkColorScheme()) {
                val vm: MainViewModel = viewModel()
                val state by vm.client.state.collectAsStateWithLifecycle()
                val active = state.phase != CmpClient.Phase.IDLE
                LaunchedEffect(active) {
                    if (active) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
                Scaffold { padding ->
                    Dashboard(vm, state, active, Modifier.padding(padding))
                }
            }
        }
    }
}

@Composable
fun Dashboard(vm: MainViewModel, state: CmpClient.State, active: Boolean, modifier: Modifier) {
    // Re-evaluate staleness even when no new value arrives.
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            now = System.currentTimeMillis()
            delay(500)
        }
    }

    Column(modifier.fillMaxSize().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = vm.host,
                onValueChange = { vm.host = it },
                label = { Text("Board IP (blank: search)") },
                singleLine = true,
                enabled = !active,
                modifier = Modifier.weight(1f),
            )
            Button(
                onClick = { if (active) vm.disconnect() else vm.connect() },
                modifier = Modifier.padding(start = 8.dp),
            ) { Text(if (active) "Disconnect" else "Connect") }
        }
        Text(statusLine(state), style = MaterialTheme.typography.bodyMedium)
        state.values[VIN.first shl 8 or VIN.second]?.let { (v, _) ->
            Text("VIN ${v.text}", style = MaterialTheme.typography.bodySmall)
        }

        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 160.dp),
            contentPadding = PaddingValues(vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(DASHBOARD) { (mode, pid) ->
                val formula = Obd.MODE01[pid]
                val entry = state.values[mode shl 8 or pid]
                val fresh = entry != null && now - entry.second < STALE_MS
                Tile(
                    name = formula?.name ?: "PID %02X".format(pid),
                    value = entry?.first?.text ?: "–",
                    unit = formula?.unit ?: "",
                    fresh = fresh,
                )
            }
        }
    }
}

@Composable
fun Tile(name: String, value: String, unit: String, fresh: Boolean) {
    Card(Modifier.fillMaxWidth().alpha(if (fresh) 1f else 0.4f)) {
        Column(Modifier.padding(12.dp)) {
            Text(name, style = MaterialTheme.typography.labelMedium)
            Row(verticalAlignment = Alignment.Bottom) {
                Text(value, fontSize = 40.sp, fontWeight = FontWeight.Bold)
                Text(" $unit", style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(bottom = 6.dp))
            }
        }
    }
}

fun statusLine(s: CmpClient.State): String = when (s.phase) {
    CmpClient.Phase.IDLE -> s.error?.let { "Error: $it" } ?: "Not connected"
    CmpClient.Phase.SEARCHING -> "Searching for the board…"
    CmpClient.Phase.UNREACHABLE -> "No answer from ${s.esp ?: "the board"}"
    CmpClient.Phase.CONNECTED -> buildString {
        append("${s.esp}  ·  CAN ${s.status?.canStateName}")
        s.status?.let { append(" ${it.bitrateKbps} kbit/s, bus errors ${it.busErrors}") }
        append("  ·  ${s.frames} frames, ${s.lost} lost")
        if (s.refused > 0) append("  ·  refused ${s.refused} (${s.lastRefusal})")
    }
}
