/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
*/

#include "session_crypto.hpp"

#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace session_crypto {

namespace {

std::array<uint8_t, 32> sha256_bytes(const uint8_t* data, size_t len)
{
#ifdef _WIN32
    std::array<uint8_t, 32> out {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hash_len = 0, data_size = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return out;
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_size, 0) != 0
        || hash_len != 32) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return out;
    }
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return out;
    }
    BCryptHashData(hash, const_cast<PUCHAR>(data), (ULONG)len, 0);
    BCryptFinishHash(hash, out.data(), 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
#else
    (void)data;
    (void)len;
    return {};
#endif
}

bool random_bytes(uint8_t* out, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::random_device rd;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)rd();
    }
    return true;
#endif
}

} // namespace

key_t derive_key(const std::string& token)
{
    const std::string material = "audio-share/1/session/" + token;
    return sha256_bytes(reinterpret_cast<const uint8_t*>(material.data()), material.size());
}

bool seal(const key_t& key, const uint8_t* plain, size_t plain_len, std::vector<uint8_t>& sealed, std::string& error)
{
    sealed.clear();
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        error = "aes unavailable";
        return false;
    }
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = "gcm unavailable";
        return false;
    }
    if (BCryptGenerateSymmetricKey(alg, &key_handle, nullptr, 0, const_cast<PUCHAR>(key.data()), (ULONG)key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = "bad key";
        return false;
    }

    uint8_t nonce[nonce_bytes] = {};
    if (!random_bytes(nonce, nonce_bytes)) {
        BCryptDestroyKey(key_handle);
        BCryptCloseAlgorithmProvider(alg, 0);
        error = "no entropy";
        return false;
    }

    sealed.resize(nonce_bytes + plain_len + tag_bytes);
    std::memcpy(sealed.data(), nonce, nonce_bytes);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = nonce_bytes;
    info.pbTag = sealed.data() + nonce_bytes + plain_len;
    info.cbTag = tag_bytes;

    ULONG written = 0;
    NTSTATUS status = BCryptEncrypt(
        key_handle,
        const_cast<PUCHAR>(plain),
        (ULONG)plain_len,
        &info,
        nullptr,
        0,
        sealed.data() + nonce_bytes,
        (ULONG)plain_len,
        &written,
        0);
    BCryptDestroyKey(key_handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status != 0 || written != plain_len) {
        sealed.clear();
        error = "encrypt failed";
        return false;
    }
    return true;
#else
    (void)key;
    (void)plain;
    (void)plain_len;
    error = "unsupported";
    return false;
#endif
}

bool seal(const key_t& key, const std::string& plain, std::vector<uint8_t>& sealed, std::string& error)
{
    return seal(key, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), sealed, error);
}

bool open(const key_t& key, const uint8_t* sealed, size_t sealed_len, std::vector<uint8_t>& plain, std::string& error)
{
    plain.clear();
    if (sealed_len < nonce_bytes + tag_bytes) {
        error = "sealed too short";
        return false;
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        error = "aes unavailable";
        return false;
    }
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = "gcm unavailable";
        return false;
    }
    if (BCryptGenerateSymmetricKey(alg, &key_handle, nullptr, 0, const_cast<PUCHAR>(key.data()), (ULONG)key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        error = "bad key";
        return false;
    }

    const size_t cipher_len = sealed_len - nonce_bytes - tag_bytes;
    plain.resize(cipher_len);

    uint8_t nonce[nonce_bytes];
    std::memcpy(nonce, sealed, nonce_bytes);
    uint8_t tag[tag_bytes];
    std::memcpy(tag, sealed + nonce_bytes + cipher_len, tag_bytes);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = nonce_bytes;
    info.pbTag = tag;
    info.cbTag = tag_bytes;

    ULONG written = 0;
    NTSTATUS status = BCryptDecrypt(
        key_handle,
        const_cast<PUCHAR>(sealed + nonce_bytes),
        (ULONG)cipher_len,
        &info,
        nullptr,
        0,
        plain.empty() ? nullptr : plain.data(),
        (ULONG)cipher_len,
        &written,
        0);
    BCryptDestroyKey(key_handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status != 0 || written != cipher_len) {
        plain.clear();
        error = "decrypt failed";
        return false;
    }
    return true;
#else
    (void)key;
    (void)sealed;
    error = "unsupported";
    return false;
#endif
}

bool open(const key_t& key, const std::vector<uint8_t>& sealed, std::string& plain, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!open(key, sealed.data(), sealed.size(), bytes, error)) {
        return false;
    }
    plain.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

} // namespace session_crypto
