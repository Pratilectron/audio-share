/*
 *    Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
 */

package io.github.mkckr0.audio_share_app.ui.screen

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import io.github.mkckr0.audio_share_app.service.PcLink
import io.github.mkckr0.audio_share_app.service.RemoteDirEntry
import io.github.mkckr0.audio_share_app.service.RemoteStats
import kotlinx.coroutines.launch
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt

@Composable
fun RemoteScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var note by remember { mutableStateOf("Touchpad, type, files, tasks, camera.") }
    var typed by remember { mutableStateOf("") }
    var path by remember { mutableStateOf("") }
    var entries by remember { mutableStateOf<List<RemoteDirEntry>>(emptyList()) }
    var stats by remember { mutableStateOf<RemoteStats?>(null) }
    var cameraBytes by remember { mutableStateOf<ByteArray?>(null) }
    var pendingDx by remember { mutableFloatStateOf(0f) }
    var pendingDy by remember { mutableFloatStateOf(0f) }

    fun sendMove(dx: Int, dy: Int) {
        if (dx == 0 && dy == 0) return
        val payload = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            .putShort(dx.coerceIn(-32768, 32767).toShort())
            .putShort(dy.coerceIn(-32768, 32767).toShort())
            .array()
        scope.launch {
            val ok = PcLink.command(context, 20, payload)
            if (!ok) note = "Move failed"
        }
    }

    fun click(button: Int) {
        scope.launch {
            val down = byteArrayOf(button.toByte(), 1)
            val up = byteArrayOf(button.toByte(), 0)
            PcLink.command(context, 21, down)
            PcLink.command(context, 21, up)
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Remote", style = MaterialTheme.typography.headlineMedium)
        Text(note, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f))

        Text("Touchpad", style = MaterialTheme.typography.titleMedium)
        Box(
            Modifier
                .fillMaxWidth()
                .height(180.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .pointerInput(Unit) {
                    detectDragGestures(
                        onDragEnd = {
                            sendMove(pendingDx.roundToInt(), pendingDy.roundToInt())
                            pendingDx = 0f
                            pendingDy = 0f
                        },
                        onDragCancel = {
                            pendingDx = 0f
                            pendingDy = 0f
                        },
                        onDrag = { change, drag ->
                            change.consume()
                            pendingDx += drag.x
                            pendingDy += drag.y
                            if (kotlin.math.abs(pendingDx) >= 2f || kotlin.math.abs(pendingDy) >= 2f) {
                                sendMove(pendingDx.roundToInt(), pendingDy.roundToInt())
                                pendingDx = 0f
                                pendingDy = 0f
                            }
                        },
                    )
                },
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            Button(onClick = { click(0) }, modifier = Modifier.weight(1f)) { Text("Left") }
            Button(onClick = { click(2) }, modifier = Modifier.weight(1f)) { Text("Middle") }
            Button(onClick = { click(1) }, modifier = Modifier.weight(1f)) { Text("Right") }
        }

        Text("Keyboard", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = typed,
            onValueChange = { typed = it },
            label = { Text("Type on the PC") },
            modifier = Modifier.fillMaxWidth(),
        )
        Button(onClick = {
            scope.launch {
                note = if (PcLink.command(context, 23, typed)) "Typed" else "Type failed"
            }
        }) { Text("Send text") }

        Text("Files", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = path,
            onValueChange = { path = it },
            label = { Text("Folder under the user profile") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                scope.launch {
                    try {
                        entries = PcLink.listDir(context, path)
                        note = "${entries.size} items"
                    } catch (e: Exception) {
                        note = e.message ?: "List failed"
                    }
                }
            }) { Text("List") }
            Button(onClick = {
                scope.launch {
                    try {
                        val data = "audio-share remote\n".toByteArray()
                        val target = if (path.isBlank()) "audio-share-remote.txt" else "$path/audio-share-remote.txt"
                        PcLink.writeFile(context, target, data)
                        note = "Wrote $target"
                    } catch (e: Exception) {
                        note = e.message ?: "Write failed"
                    }
                }
            }) { Text("Write test") }
        }
        entries.take(12).forEach { entry ->
            Text(
                "${if (entry.directory) "[dir]" else "[file]"} ${entry.name}" +
                    if (entry.directory) "" else " (${entry.bytes} B)",
            )
        }

        Text("Tasks", style = MaterialTheme.typography.titleMedium)
        Button(onClick = {
            scope.launch {
                try {
                    stats = PcLink.stats(context)
                    note = "CPU ${stats?.cpuPercent}% · RAM ${stats?.memoryUsedMb}/${stats?.memoryTotalMb} MB"
                } catch (e: Exception) {
                    note = e.message ?: "Stats failed"
                }
            }
        }) { Text("Refresh stats") }
        stats?.processes?.take(8)?.forEach { row ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("${row.name} (${row.pid})", modifier = Modifier.weight(1f))
                Button(onClick = {
                    scope.launch {
                        try {
                            PcLink.endProcess(context, row.pid)
                            note = "Ended ${row.name}"
                            stats = PcLink.stats(context)
                        } catch (e: Exception) {
                            note = e.message ?: "End failed"
                        }
                    }
                }) { Text("End") }
            }
        }

        Text("Camera", style = MaterialTheme.typography.titleMedium)
        Button(onClick = {
            scope.launch {
                try {
                    cameraBytes = PcLink.cameraJpeg(context)
                    note = "Camera ${cameraBytes?.size ?: 0} bytes"
                } catch (e: Exception) {
                    note = e.message ?: "Camera failed"
                }
            }
        }) { Text("Snap") }
        cameraBytes?.let { bytes ->
            val bitmap = remember(bytes) { BitmapFactory.decodeByteArray(bytes, 0, bytes.size) }
            if (bitmap != null) {
                Image(
                    bitmap = bitmap.asImageBitmap(),
                    contentDescription = null,
                    modifier = Modifier.fillMaxWidth().height(200.dp),
                )
            }
        }
    }
}
