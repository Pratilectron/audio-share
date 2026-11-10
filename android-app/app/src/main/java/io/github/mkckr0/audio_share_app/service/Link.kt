/*
 *    Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 */

package io.github.mkckr0.audio_share_app.service

import android.content.Context
import android.net.wifi.WifiManager
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import io.github.mkckr0.audio_share_app.R
import io.github.mkckr0.audio_share_app.model.NetworkConfigKeys
import io.github.mkckr0.audio_share_app.model.getInteger
import io.github.mkckr0.audio_share_app.model.networkConfigDataStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

data class FoundPc(
    val name: String,
    val host: String,
    val port: Int,
    val clients: Int,
    val needsAuth: Boolean,
    val seenAt: Long,
)

object LanPresence {
    private val found = LinkedHashMap<String, FoundPc>()
    private val _devices = MutableStateFlow<List<FoundPc>>(emptyList())
    val devices: StateFlow<List<FoundPc>> = _devices

    private var job: Job? = null
    private var lock: WifiManager.MulticastLock? = null

    fun start(context: Context) {
        if (job != null) {
            return
        }
        val app = context.applicationContext
        val wifi = app.getSystemService(WifiManager::class.java)
        lock = wifi?.createMulticastLock("audio-share")?.apply {
            setReferenceCounted(false)
            acquire()
        }
        job = CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            val socket = DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                soTimeout = 400
                bind(InetSocketAddress(0))
            }
            val query = byteArrayOf('A'.code.toByte(), 'S'.code.toByte(), 'Q'.code.toByte(), '1'.code.toByte())
            try {
                while (isActive) {
                    try {
                        socket.send(DatagramPacket(query, query.size, InetAddress.getByName("255.255.255.255"), 54530))
                    } catch (_: Exception) {
                    }
                    val deadline = System.currentTimeMillis() + 1200
                    while (System.currentTimeMillis() < deadline) {
                        val buffer = ByteArray(512)
                        val packet = DatagramPacket(buffer, buffer.size)
                        try {
                            socket.receive(packet)
                        } catch (_: SocketTimeoutException) {
                            break
                        }
                        readBeacon(packet)
                    }
                    val now = System.currentTimeMillis()
                    synchronized(found) {
                        found.entries.removeIf { now - it.value.seenAt > 8000 }
                        _devices.value = found.values.sortedBy { it.name }
                    }
                    delay(800)
                }
            } finally {
                socket.close()
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        lock?.release()
        lock = null
    }

    private fun readBeacon(packet: DatagramPacket) {
        val data = packet.data
        val size = packet.length
        if (size < 9 || data[0] != 'A'.code.toByte() || data[1] != 'S'.code.toByte() || data[2] != 'N'.code.toByte() || data[3] != '1'.code.toByte()) {
            return
        }
        val port = (data[4].toInt() and 0xff) or ((data[5].toInt() and 0xff) shl 8)
        val clients = (data[6].toInt() and 0xff) or ((data[7].toInt() and 0xff) shl 8)
        val needsAuth = data[8].toInt() and 0xff != 0
        val name = String(data, 9, size - 9, Charsets.UTF_8).trim()
        val host = packet.address?.hostAddress ?: return
        if (name.isEmpty() || port !in 1..65535) {
            return
        }
        synchronized(found) {
            found[name] = FoundPc(name, host, port, clients, needsAuth, System.currentTimeMillis())
        }
    }
}

data class PlaybackTarget(val host: String, val port: Int, val token: String, val name: String)

suspend fun Context.playbackTarget(): PlaybackTarget {
    val config = networkConfigDataStore.data.first()
    val entered = (config[stringPreferencesKey(NetworkConfigKeys.HOST)] ?: getString(R.string.default_host)).trim()
    val savedPort = config[intPreferencesKey(NetworkConfigKeys.PORT)] ?: getInteger(R.integer.default_port)
    val savedToken = config[stringPreferencesKey(NetworkConfigKeys.TOKEN)] ?: ""
    if (!MachineName.isName(entered)) {
        return PlaybackTarget(entered, savedPort, savedToken, entered)
    }
    val seen = LanPresence.devices.value.firstOrNull { it.name.equals(entered, ignoreCase = true) }
    val token = MachineName.token(entered)
    if (seen != null) {
        return PlaybackTarget(seen.host, seen.port, if (seen.needsAuth) token else "", entered)
    }
    val remote = MachineName.resolve(entered)
    return PlaybackTarget(remote.host, remote.port, token, entered)
}

object PcLink {
    private const val CMD_AUTH = 4
    private const val CMD_CONTROL = 5
    private const val CMD_LIST_APPS = 6

    suspend fun command(context: Context, action: Int, payload: String = ""): Boolean {
        return useLink(context) { socket ->
            val payloadBytes = payload.toByteArray(Charsets.UTF_8)
            socket.writeLe(CMD_CONTROL)
            socket.writeLe(action)
            socket.writeLe(payloadBytes.size)
            if (payloadBytes.isNotEmpty()) {
                socket.getOutputStream().write(payloadBytes)
            }
            socket.getOutputStream().flush()
            val cmd = socket.readLe()
            val status = socket.readLe()
            cmd == CMD_CONTROL && status == 0
        }
    }

    suspend fun shortcuts(context: Context): List<String> {
        return useLink(context) { socket ->
            socket.writeLe(CMD_LIST_APPS)
            socket.getOutputStream().flush()
            if (socket.readLe() != CMD_LIST_APPS) {
                return@useLink emptyList()
            }
            val count = socket.readLe()
            buildList {
                repeat(count.coerceIn(0, 32)) {
                    val length = socket.readLe()
                    if (length !in 0..256) {
                        return@buildList
                    }
                    add(String(socket.readBytes(length), Charsets.UTF_8))
                }
            }
        }
    }

    private suspend fun <T> useLink(context: Context, block: (Socket) -> T): T {
        val config = context.playbackTarget()
        return withContext(Dispatchers.IO) {
            Socket().use { socket ->
                socket.soTimeout = 4000
                socket.connect(InetSocketAddress(config.host, config.port), 4000)
                if (config.token.isNotEmpty()) {
                    val tokenBytes = config.token.toByteArray(Charsets.UTF_8)
                    socket.writeLe(CMD_AUTH)
                    socket.writeLe(tokenBytes.size)
                    socket.getOutputStream().write(tokenBytes)
                    socket.getOutputStream().flush()
                    if (socket.readLe() != CMD_AUTH || socket.readLe() != 0) {
                        throw IllegalStateException("The PC refused this phone")
                    }
                }
                block(socket)
            }
        }
    }

    private fun Socket.writeLe(value: Int) {
        getOutputStream().write(
            byteArrayOf(
                (value and 0xff).toByte(),
                ((value shr 8) and 0xff).toByte(),
                ((value shr 16) and 0xff).toByte(),
                ((value shr 24) and 0xff).toByte(),
            )
        )
    }

    private fun Socket.readLe(): Int {
        val bytes = readBytes(4)
        return (bytes[0].toInt() and 0xff) or
            ((bytes[1].toInt() and 0xff) shl 8) or
            ((bytes[2].toInt() and 0xff) shl 16) or
            ((bytes[3].toInt() and 0xff) shl 24)
    }

    private fun Socket.readBytes(count: Int): ByteArray {
        val out = ByteArray(count)
        var got = 0
        val input = getInputStream()
        while (got < count) {
            val n = input.read(out, got, count - got)
            if (n < 0) {
                throw IllegalStateException("The PC closed the line")
            }
            got += n
        }
        return out
    }
}
