/*
 *    Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 */

package io.github.mkckr0.audio_share_app.ui.screen

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PauseCircle
import androidx.compose.material.icons.filled.PlayCircle
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleStartEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.media3.common.Player
import io.github.mkckr0.audio_share_app.service.AudioPlayer
import io.github.mkckr0.audio_share_app.service.FoundPc
import io.github.mkckr0.audio_share_app.service.LanPresence
import io.github.mkckr0.audio_share_app.service.MachineName
import io.github.mkckr0.audio_share_app.service.PcLink
import io.github.mkckr0.audio_share_app.ui.MainActivity
import kotlinx.coroutines.launch

private val roomActions = listOf(
    1 to "Volume up",
    2 to "Volume down",
    3 to "Mute",
    4 to "Play",
    5 to "Next",
    6 to "Previous",
    7 to "Settings",
    8 to "Sound",
    9 to "Display",
    10 to "Wi-Fi",
    11 to "Bluetooth",
    17 to "Night light",
    13 to "Files",
    14 to "Tasks",
    18 to "Notepad",
    19 to "Calculator",
    15 to "Lock PC",
)

@Composable
fun HomeScreen(viewModel: HomeScreenViewModel = viewModel()) {
    val context = LocalContext.current
    val activity = context as MainActivity
    val scope = rememberCoroutineScope()
    val devices by LanPresence.devices.collectAsStateWithLifecycle()
    val rememberOn by viewModel.rememberOn.collectAsStateWithLifecycle()
    val remembered by viewModel.remembered.collectAsStateWithLifecycle()
    val uiState = viewModel.uiState.collectAsStateWithLifecycle().value

    DisposableEffect(Unit) {
        LanPresence.start(context)
        onDispose { }
    }

    when (uiState) {
        HomeScreenViewModel.UiState.Loading -> {}
        is HomeScreenViewModel.UiState.Success -> {
            var name by remember(uiState.host) { mutableStateOf(uiState.host) }
            var started by remember { mutableStateOf(false) }
            var holdAuto by remember { mutableStateOf(false) }
            var shortcuts by remember { mutableStateOf(listOf<String>()) }
            var browser by remember { mutableStateOf("") }
            var note by remember { mutableStateOf("") }
            val tried = remember { mutableStateListOf<String>() }
            val breath by rememberInfiniteTransition(label = "room").animateFloat(
                initialValue = 0.35f,
                targetValue = 1f,
                animationSpec = infiniteRepeatable(
                    animation = tween(if (started) 1200 else 2600),
                    repeatMode = RepeatMode.Reverse,
                ),
                label = "breath",
            )

            suspend fun connect(target: String) {
                viewModel.saveNetWorkSettings(target, uiState.port, "").join()
                if (rememberOn && MachineName.isName(target)) {
                    viewModel.remember(target, true).join()
                }
                activity.awaitMediaController().play()
            }

            LaunchedEffect(devices, remembered, rememberOn, started, holdAuto) {
                if (started || holdAuto || !rememberOn) {
                    return@LaunchedEffect
                }
                val hit = devices.firstOrNull { it.name in remembered && it.name !in tried } ?: return@LaunchedEffect
                tried.add(hit.name)
                name = hit.name
                connect(hit.name)
            }

            LaunchedEffect(started) {
                shortcuts = if (started) {
                    try {
                        PcLink.shortcuts(context)
                    } catch (_: Exception) {
                        emptyList()
                    }
                } else {
                    emptyList()
                }
            }

            LifecycleStartEffect(true) {
                val listener = object : Player.Listener {
                    override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) {
                        started = playWhenReady
                    }
                }
                scope.launch {
                    activity.awaitMediaController().run {
                        started = playWhenReady
                        addListener(listener)
                    }
                }
                onStopOrDispose {
                    scope.launch { activity.awaitMediaController().removeListener(listener) }
                }
            }

            Box(Modifier.fillMaxSize()) {
                Box(
                    Modifier
                        .size(460.dp)
                        .align(Alignment.TopCenter)
                        .offset(y = (-120).dp)
                        .graphicsLayer { alpha = breath * if (started) 0.45f else 0.16f }
                        .background(
                            if (started) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                            CircleShape,
                        )
                )
                Column(
                    Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(20.dp),
                    verticalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    Text(
                        text = if (started) "On the line" else "Waiting",
                        style = MaterialTheme.typography.headlineMedium,
                    )
                    Text(
                        text = if (started) {
                            "This phone is linked. The room is awake."
                        } else {
                            "Quiet. The line is down, listening for the next name."
                        },
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = if (started) 0.8f else 0.55f),
                    )
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
                        Text("Remember accepted PCs", modifier = Modifier.weight(1f))
                        Switch(checked = rememberOn, onCheckedChange = { viewModel.setRememberOn(it) })
                    }
                    Text("On this Wi-Fi", style = MaterialTheme.typography.titleMedium)
                    if (devices.isEmpty()) {
                        Text(
                            "No PC is announcing itself yet.",
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                        )
                    }
                    devices.forEach { pc ->
                        PcRow(
                            pc = pc,
                            live = started && name.equals(pc.name, ignoreCase = true),
                            remembered = pc.name in remembered,
                            onAccept = {
                                holdAuto = false
                                name = pc.name
                                scope.launch { connect(pc.name) }
                            },
                        )
                    }
                    Text("Commands", style = MaterialTheme.typography.titleMedium, modifier = Modifier.alpha(if (started) 1f else 0.4f))
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        roomActions.chunked(3).forEach { row ->
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                                row.forEach { (action, label) ->
                                    Button(
                                        onClick = {
                                            if (!started) {
                                                note = "Accept a PC first."
                                                return@Button
                                            }
                                            scope.launch {
                                                note = if (PcLink.command(context, action)) label else "The PC did not do that"
                                            }
                                        },
                                        modifier = Modifier.weight(1f),
                                        enabled = started,
                                    ) { Text(label) }
                                }
                                repeat(3 - row.size) { Box(Modifier.weight(1f)) }
                            }
                        }
                    }
                    if (shortcuts.isNotEmpty()) {
                        Text("Pinned on the PC", style = MaterialTheme.typography.titleMedium)
                        shortcuts.chunked(2).forEach { row ->
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                                row.forEach { label ->
                                    Button(
                                        onClick = {
                                            scope.launch {
                                                note = if (PcLink.command(context, 16, label)) label else "Could not open $label"
                                            }
                                        },
                                        modifier = Modifier.weight(1f),
                                    ) { Text(label) }
                                }
                                if (row.size == 1) Box(Modifier.weight(1f))
                            }
                        }
                    }
                    OutlinedTextField(
                        value = browser,
                        onValueChange = { browser = it },
                        label = { Text("Open on the PC browser") },
                        enabled = started,
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true,
                        trailingIcon = {
                            Button(
                                onClick = {
                                    scope.launch {
                                        note = if (PcLink.command(context, 12, browser)) "Browser" else "Could not open the browser"
                                    }
                                },
                                enabled = started,
                            ) { Text("Go") }
                        },
                    )
                    OutlinedTextField(
                        value = name,
                        onValueChange = { if (!it.contains(' ')) name = it },
                        label = { Text("Name, if it is not on this Wi-Fi") },
                        enabled = !started,
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true,
                    )
                    IconButton(
                        onClick = {
                            scope.launch {
                                if (started) {
                                    holdAuto = true
                                    activity.awaitMediaController().stop()
                                } else if (name.isNotBlank()) {
                                    holdAuto = false
                                    connect(name.trim())
                                }
                            }
                        },
                        modifier = Modifier.size(84.dp).align(Alignment.CenterHorizontally),
                    ) {
                        Icon(
                            imageVector = if (started) Icons.Default.PauseCircle else Icons.Default.PlayCircle,
                            contentDescription = null,
                            tint = if (started) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                    if (note.isNotEmpty()) {
                        Text(note, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f))
                    }
                    OutlinedCard(Modifier.fillMaxWidth()) {
                        SelectionContainer {
                            Text(
                                text = AudioPlayer.message,
                                modifier = Modifier.padding(16.dp),
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PcRow(pc: FoundPc, live: Boolean, remembered: Boolean, onAccept: () -> Unit) {
    OutlinedCard(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Column(Modifier.weight(1f)) {
                Text(pc.name, style = MaterialTheme.typography.titleMedium)
                val badge = when {
                    live -> "Live"
                    pc.clients > 0 -> "${pc.clients} linked"
                    remembered -> "Remembered"
                    else -> "New"
                }
                Text(
                    "$badge  ·  ${pc.host}",
                    color = if (live) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
                )
            }
            Button(onClick = onAccept, enabled = !live) {
                Text(if (live) "On" else "Accept")
            }
        }
    }
}
