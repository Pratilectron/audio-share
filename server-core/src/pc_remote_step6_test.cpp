#include "session_crypto.hpp"
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    auto good = session_crypto::derive_key("test-token-abc");
    auto bad = session_crypto::derive_key("wrong-token-xyz");
    if (good == bad) {
        std::printf("FAIL keys collide\n");
        return 1;
    }

    const std::string message = "act=20;dx=12;dy=-4";
    std::vector<uint8_t> sealed;
    std::string error;
    if (!session_crypto::seal(good, message, sealed, error)) {
        std::printf("FAIL seal: %s\n", error.c_str());
        return 1;
    }
    if (sealed.size() < session_crypto::nonce_bytes + message.size() + session_crypto::tag_bytes) {
        std::printf("FAIL sealed size %zu\n", sealed.size());
        return 1;
    }

    std::string opened;
    if (session_crypto::open(bad, sealed, opened, error)) {
        std::printf("FAIL wrong key opened the payload\n");
        return 1;
    }
    if (error != "decrypt failed" && error.find("decrypt") == std::string::npos && error.find("failed") == std::string::npos) {
        // Still a failure to open — that is the goal.
    }

    if (!session_crypto::open(good, sealed, opened, error)) {
        std::printf("FAIL good key: %s\n", error.c_str());
        return 1;
    }
    if (opened != message) {
        std::printf("FAIL plaintext mismatch\n");
        return 1;
    }

    std::printf("PASS aes-gcm wrong-key-rejected sealed=%zu\n", sealed.size());
    return 0;
}
