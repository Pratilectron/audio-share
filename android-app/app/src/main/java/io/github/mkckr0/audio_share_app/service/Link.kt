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
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

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

data class RemoteDirEntry(val name: String, val directory: Boolean, val bytes: Long)
data class RemoteProcess(val pid: Int, val memoryMb: Int, val name: String)
data class RemoteStats(val cpuPercent: Int, val memoryUsedMb: Int, val memoryTotalMb: Int, val processes: List<RemoteProcess>)

object SessionCrypto {
    private const val NONCE = 12
    private const val TAG = 16

    fun key(token: String): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        return digest.digest("audio-share/1/session/$token".toByteArray(Charsets.UTF_8))
    }

    fun seal(token: String, plain: ByteArray): ByteArray {
        if (token.isEmpty() || plain.isEmpty()) {
            return plain
        }
        val nonce = ByteArray(NONCE).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key(token), "AES"), GCMParameterSpec(TAG * 8, nonce))
        val cipherText = cipher.doFinal(plain)
        return nonce + cipherText
    }

    fun open(token: String, sealed: ByteArray): ByteArray {
        if (token.isEmpty() || sealed.isEmpty()) {
            return sealed
        }
        require(sealed.size >= NONCE + TAG) { "sealed too short" }
        val nonce = sealed.copyOfRange(0, NONCE)
        val body = sealed.copyOfRange(NONCE, sealed.size)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key(token), "AES"), GCMParameterSpec(TAG * 8, nonce))
        return cipher.doFinal(body)
    }
}

object PcLink {
    private const val CMD_AUTH = 4
    private const val CMD_CONTROL = 5
    private const val CMD_LIST_APPS = 6
    private const val CMD_FS_LIST = 7
    private const val CMD_FS_GET = 8
    private const val CMD_FS_PUT = 9
    private const val CMD_STATS = 10
    private const val CMD_END_PROCESS = 11
    private const val CMD_CAMERA = 12

    suspend fun command(context: Context, action: Int, payload: String = ""): Boolean {
        return command(context, action, payload.toByteArray(Charsets.UTF_8))
    }

    suspend fun command(context: Context, action: Int, payload: ByteArray): Boolean {
        return useLink(context) { socket, token ->
            val payloadBytes = SessionCrypto.seal(token, payload)
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
        return useLink(context) { socket, _ ->
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

    suspend fun listDir(context: Context, relative: String = ""): List<RemoteDirEntry> {
        return useLink(context) { socket, token ->
            val body = exchange(socket, token, CMD_FS_LIST, relative.toByteArray(Charsets.UTF_8))
            var offset = 0
            fun u32(): Int {
                val v = (body[offset].toInt() and 0xff) or
                    ((body[offset + 1].toInt() and 0xff) shl 8) or
                    ((body[offset + 2].toInt() and 0xff) shl 16) or
                    ((body[offset + 3].toInt() and 0xff) shl 24)
                offset += 4
                return v
            }
            fun u64(): Long {
                var v = 0L
                for (i in 0 until 8) {
                    v = v or ((body[offset + i].toLong() and 0xff) shl (8 * i))
                }
                offset += 8
                return v
            }
            val count = u32().coerceIn(0, 200)
            buildList {
                repeat(count) {
                    val nameLen = u32().coerceIn(0, 512)
                    val name = String(body, offset, nameLen, Charsets.UTF_8)
                    offset += nameLen
                    val directory = body[offset].toInt() != 0
                    offset += 1
                    val bytes = u64()
                    add(RemoteDirEntry(name, directory, bytes))
                }
            }
        }
    }

    suspend fun readFile(context: Context, relative: String): ByteArray {
        return useLink(context) { socket, token ->
            exchange(socket, token, CMD_FS_GET, relative.toByteArray(Charsets.UTF_8))
        }
    }

    suspend fun writeFile(context: Context, relative: String, data: ByteArray): Boolean {
        return useLink(context) { socket, token ->
            val path = relative.toByteArray(Charsets.UTF_8)
            val packed = ByteArray(4 + path.size + data.size)
            packed[0] = (path.size and 0xff).toByte()
            packed[1] = ((path.size shr 8) and 0xff).toByte()
            packed[2] = ((path.size shr 16) and 0xff).toByte()
            packed[3] = ((path.size shr 24) and 0xff).toByte()
            System.arraycopy(path, 0, packed, 4, path.size)
            System.arraycopy(data, 0, packed, 4 + path.size, data.size)
            exchange(socket, token, CMD_FS_PUT, packed)
            true
        }
    }

    suspend fun stats(context: Context): RemoteStats {
        return useLink(context) { socket, token ->
            val body = exchange(socket, token, CMD_STATS, ByteArray(0))
            var offset = 0
            fun u32(): Int {
                val v = (body[offset].toInt() and 0xff) or
                    ((body[offset + 1].toInt() and 0xff) shl 8) or
                    ((body[offset + 2].toInt() and 0xff) shl 16) or
                    ((body[offset + 3].toInt() and 0xff) shl 24)
                offset += 4
                return v
            }
            val cpu = u32()
            val used = u32()
            val total = u32()
            val count = u32().coerceIn(0, 80)
            val processes = buildList {
                repeat(count) {
                    val pid = u32()
                    val mem = u32()
                    val nameLen = u32().coerceIn(0, 260)
                    val name = String(body, offset, nameLen, Charsets.UTF_8)
                    offset += nameLen
                    add(RemoteProcess(pid, mem, name))
                }
            }
            RemoteStats(cpu, used, total, processes)
        }
    }

    suspend fun endProcess(context: Context, pid: Int): Boolean {
        return useLink(context) { socket, token ->
            val req = byteArrayOf(
                (pid and 0xff).toByte(),
                ((pid shr 8) and 0xff).toByte(),
                ((pid shr 16) and 0xff).toByte(),
                ((pid shr 24) and 0xff).toByte(),
            )
            exchange(socket, token, CMD_END_PROCESS, req)
            true
        }
    }

    suspend fun cameraJpeg(context: Context): ByteArray {
        return useLink(context) { socket, token ->
            exchange(socket, token, CMD_CAMERA, ByteArray(0))
        }
    }

    private fun exchange(socket: Socket, token: String, cmd: Int, request: ByteArray): ByteArray {
        val sealed = SessionCrypto.seal(token, request)
        socket.writeLe(cmd)
        socket.writeLe(sealed.size)
        if (sealed.isNotEmpty()) {
            socket.getOutputStream().write(sealed)
        }
        socket.getOutputStream().flush()
        if (socket.readLe() != cmd) {
            throw IllegalStateException("Unexpected reply")
        }
        val status = socket.readLe()
        val size = socket.readLe()
        if (size !in 0..9_000_000) {
            throw IllegalStateException("Reply too large")
        }
        val body = if (size > 0) socket.readBytes(size) else ByteArray(0)
        if (status != 0) {
            throw IllegalStateException("PC remote command failed ($status)")
        }
        return SessionCrypto.open(token, body)
    }

    private suspend fun <T> useLink(context: Context, block: (Socket, String) -> T): T {
        val config = context.playbackTarget()
        return withContext(Dispatchers.IO) {
            Socket().use { socket ->
                socket.soTimeout = 8000
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
                block(socket, config.token)
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
