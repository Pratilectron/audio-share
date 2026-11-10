/*
 *    Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

package io.github.mkckr0.audio_share_app.service

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * The name is calculated on the PC from its machine id. This side only
 * turns that name back into an address and the matching token.
 */
object MachineName {
    private val crockford = "0123456789abcdefghjkmnpqrstvwxyz"
    private val namePattern =
        Regex("^[a-z]{3,16}-[a-z]{3,16}-[0-9a-hjkmnp-tv-z]{8}$")

    fun isName(value: String): Boolean = namePattern.matches(value.trim().lowercase())

    fun token(name: String): String {
        val digest = sha256("audio-share/1/token/" + name.trim().lowercase())
        return crockford(digest, 10)
    }

    data class Endpoint(val host: String, val port: Int)

    suspend fun resolve(name: String): Endpoint = withContext(Dispatchers.IO) {
        val normalized = name.trim().lowercase()
        val topic = topic(normalized)
        val url = URL("https://ntfy.sh/$topic/json?poll=1&since=12h")
        val connection = (url.openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 10_000
            readTimeout = 10_000
            setRequestProperty("User-Agent", "audio-share")
        }
        try {
            val code = connection.responseCode
            val body = (if (code in 200..299) connection.inputStream else connection.errorStream)
                ?.bufferedReader()
                ?.use { it.readText() }
                .orEmpty()
            if (code !in 200..299) {
                throw IllegalStateException("Could not look up this name ($code)")
            }
            val message = Regex("\"message\"\\s*:\\s*\"([^\"]+)\"")
                .findAll(body)
                .map { it.groupValues[1] }
                .lastOrNull { it.contains(':') }
                ?: throw IllegalStateException("This name is not online. Start the PC with Internet mode on.")
            val colon = message.lastIndexOf(':')
            val host = message.substring(0, colon).trim()
            val port = message.substring(colon + 1).trim().toIntOrNull()
                ?: throw IllegalStateException("This name is not online. Start the PC with Internet mode on.")
            if (host.isEmpty() || port !in 1..65535) {
                throw IllegalStateException("This name is not online. Start the PC with Internet mode on.")
            }
            Endpoint(host, port)
        } finally {
            connection.disconnect()
        }
    }

    private fun topic(name: String): String {
        val digest = sha256("audio-share/1/topic/$name")
        return digest.take(16).joinToString("") { "%02x".format(it) }
    }

    private fun sha256(text: String): ByteArray {
        return MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.UTF_8))
    }

    private fun crockford(data: ByteArray, chars: Int): String {
        val out = StringBuilder(chars)
        var acc = 0
        var bits = 0
        for (value in data) {
            acc = (acc shl 8) or (value.toInt() and 0xff)
            bits += 8
            while (bits >= 5 && out.length < chars) {
                bits -= 5
                out.append(crockford[(acc shr bits) and 31])
            }
            if (out.length == chars) {
                break
            }
        }
        return out.toString()
    }
}
