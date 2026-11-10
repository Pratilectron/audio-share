/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
*/

#ifndef SESSION_CRYPTO_HPP
#define SESSION_CRYPTO_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// AES-256-GCM session sealing for authenticated remote control.
// Key = SHA-256("audio-share/1/session/" + token).
// Sealed blob = 12-byte nonce || ciphertext || 16-byte tag.
namespace session_crypto {

constexpr size_t key_bytes = 32;
constexpr size_t nonce_bytes = 12;
constexpr size_t tag_bytes = 16;

using key_t = std::array<uint8_t, key_bytes>;

key_t derive_key(const std::string& token);

bool seal(const key_t& key, const uint8_t* plain, size_t plain_len, std::vector<uint8_t>& sealed, std::string& error);
bool seal(const key_t& key, const std::string& plain, std::vector<uint8_t>& sealed, std::string& error);

bool open(const key_t& key, const uint8_t* sealed, size_t sealed_len, std::vector<uint8_t>& plain, std::string& error);
bool open(const key_t& key, const std::vector<uint8_t>& sealed, std::string& plain, std::string& error);

} // namespace session_crypto

#endif
