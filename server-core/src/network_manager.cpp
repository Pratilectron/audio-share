/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "network_manager.hpp"
#include "formatter.hpp"
#include "audio_manager.hpp"
#include "upnp_client.hpp"
#include "machine_name.hpp"
#include "directory.hpp"
#include "remote_control.hpp"
#include "local_dashboard.hpp"
#include "pc_remote.hpp"
#include "session_crypto.hpp"

#include <array>
#include <list>
#include <random>
#include <ranges>
#include <coroutine>

#ifdef _WINDOWS
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#endif // _WINDOWS

#ifdef linux
#include <sys/types.h>
#include <ifaddrs.h>
#endif

#include <spdlog/spdlog.h>
#include <fmt/ranges.h>

namespace ip = asio::ip;
using namespace std::chrono_literals;

namespace {

std::string generate_token(size_t length = 8)
{
    static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device random;
    std::uniform_int_distribution<size_t> distribution(0, sizeof(alphabet) - 2);

    std::string token;
    token.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        token.push_back(alphabet[distribution(random)]);
    }
    return token;
}

} // namespace

network_manager::network_manager(std::shared_ptr<audio_manager>& audio_manager)
    : _audio_manager(audio_manager)
{
}

std::vector<std::string> network_manager::get_address_list()
{
    std::vector<std::string> address_list;

#ifdef _WINDOWS
    ULONG family = AF_INET;
    ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES;

    ULONG size = 0;
    GetAdaptersAddresses(family, flags, nullptr, nullptr, &size);
    auto pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

    auto ret = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &size);
    if (ret == ERROR_SUCCESS) {
        for (auto pCurrentAddress = pAddresses; pCurrentAddress; pCurrentAddress = pCurrentAddress->Next) {
            if (pCurrentAddress->OperStatus != IfOperStatusUp || pCurrentAddress->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }

            for (auto pUnicast = pCurrentAddress->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                auto sockaddr = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char buf[50];
                if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
                    address_list.emplace_back(buf);
                }
            }
        }
    }

    free(pAddresses);
#endif

#ifdef linux
    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) == -1) {
        return address_list;
    }

    for (auto ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        auto sockaddr = (sockaddr_in*)ifa->ifa_addr;
        char buf[50];
        if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
            address_list.emplace_back(buf);
        }
    }

    freeifaddrs(ifaddrs);
#endif

    return address_list;
}

std::string network_manager::get_default_address()
{
    return select_default_address(get_address_list());
}

std::string network_manager::get_primary_address()
{
    try {
        asio::io_context ioc;
        asio::ip::udp::socket socket(ioc);
        socket.open(asio::ip::udp::v4());
        socket.connect(asio::ip::udp::endpoint(asio::ip::make_address_v4("8.8.8.8"), 53));
        return socket.local_endpoint().address().to_string();
    } catch (const std::exception&) {
        return {};
    }
}

std::string network_manager::select_default_address(const std::vector<std::string>& address_list)
{
    if (address_list.empty()) {
        return {};
    }

    auto is_private_address = [](const std::string& address) {
        constexpr uint32_t private_addr_list[] = {
            0x0a000000,
            0xac100000,
            0xc0a80000,
        };

        uint32_t addr;
        inet_pton(AF_INET, address.c_str(), &addr);
        addr = ntohl(addr);
        for (auto&& private_addr : private_addr_list) {
            if ((addr & private_addr) == private_addr) {
                return true;
            }
        }

        return false;
    };

    for (auto&& address : address_list) {
        if (is_private_address(address)) {
            return address;
        }
    }
    return address_list.front();
}

void network_manager::start_server(const std::string& host, uint16_t port, const audio_manager::capture_config& capture_config,
                                   const std::string& token, bool enable_upnp)
{
    _directory_stop = true;
    if (_directory_thread.joinable()) {
        _directory_thread.join();
    }
    _directory_stop = false;
    _directory_ready = false;
    _beacon_stop = true;
    if (_beacon_thread.joinable()) {
        _beacon_thread.join();
    }
    _beacon_stop = false;
    _client_count = 0;
    local_dashboard::stop();

    _token = token;
    _machine_name.clear();
    _port = port;
    _external_endpoint.clear();
    _upnp_note.clear();
    _upnp_mapped = false;
    _udp_servers.clear();

    if (enable_upnp) {
        // The name and the token are a function of this computer, so deleting
        // the app and opening it again produces the same name.
        _machine_name = machine_name::local_name();
        if (!_machine_name.empty()) {
            _token = machine_name::token_for(_machine_name);
        } else if (_token.empty()) {
            _token = generate_token();
        }
    }

    _ioc = std::make_shared<asio::io_context>();
    {
        ip::tcp::endpoint endpoint { ip::make_address(host), port };

        ip::tcp::acceptor acceptor(*_ioc, endpoint.protocol());
        acceptor.set_option(ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        _audio_manager->start_loopback_recording(shared_from_this(), capture_config);
        asio::co_spawn(*_ioc, accept_tcp_loop(std::move(acceptor)), asio::detached);

        // start tcp success
        spdlog::info("tcp listen success on {}", endpoint);
    }

    {
        auto shown = _machine_name.empty() ? machine_name::local_name() : _machine_name;
        local_dashboard::note(shown, 0);
        local_dashboard::start();
    }

    {
        // In internet mode also listen on a few ports below the server port.
        // Some routers keep a single rule per external port, so the UDP rule
        // for the TCP port is refused; the client registers on all of them and
        // the audio is sent back from the socket that received it, which keeps
        // the source port identical to the port the client used.
        std::vector<uint16_t> udp_ports { port };
        if (enable_upnp) {
            for (uint16_t offset = 1; offset < 4 && port > offset; ++offset) {
                udp_ports.push_back((uint16_t)(port - offset));
            }
        }

        for (size_t i = 0; i < udp_ports.size(); ++i) {
            auto udp_port = udp_ports[i];
            try {
                ip::udp::endpoint endpoint { ip::make_address(host), udp_port };
                auto socket = std::make_shared<udp_socket>(*_ioc, endpoint.protocol());
                socket->bind(endpoint);
                _udp_servers.push_back(socket);
                asio::co_spawn(*_ioc, accept_udp_loop(socket), asio::detached);
                spdlog::info("udp listen success on {}", endpoint);
            } catch (const std::exception& e) {
                if (i == 0) {
                    // The server port itself must be available.
                    throw;
                }
                spdlog::warn("udp listen on {} failed: {}", udp_port, e.what());
            }
        }
    }

    if (enable_upnp) {
        auto internal_ip = host;
        if (internal_ip == "0.0.0.0" || internal_ip == "::") {
            internal_ip = get_primary_address();
            if (internal_ip.empty()) {
                internal_ip = get_default_address();
            }
        }

        auto result = upnp_client::map_port(port, internal_ip, "Audio Share");
        if (result.success) {
            _upnp_mapped = true;
            spdlog::info("upnp forwarded tcp {} and udp {}", port, fmt::join(result.udp_ports, ", "));
            if (!result.udp_ports.empty()) {
                _upnp_note = fmt::format("UDP {}", fmt::join(result.udp_ports, ", "));
            }
            if (!result.external_ip.empty()) {
                _external_endpoint = fmt::format("{}:{}", result.external_ip, port);
                spdlog::info("upnp port forwarding success, public endpoint {}", _external_endpoint);
            }
            if (!result.error.empty()) {
                spdlog::warn("upnp: {}", result.error);
                if (!_upnp_note.empty()) {
                    _upnp_note += ". ";
                }
                _upnp_note += result.error;
            }
        } else {
            spdlog::warn("upnp port forwarding failed: {}", result.error);
        }
    }

    if (!_machine_name.empty()) {
        spdlog::info("machine name: {}", _machine_name);
    }
    if (!_token.empty() && _machine_name.empty()) {
        spdlog::info("authentication token: {}", _token);
    }

    if (enable_upnp && !_machine_name.empty() && !_external_endpoint.empty()) {
        _directory_ready = directory::publish(_machine_name, _external_endpoint);
        if (_directory_ready) {
            spdlog::info("name {} is online", _machine_name);
        } else {
            spdlog::warn("could not register name {}", _machine_name);
        }
        _directory_thread = std::thread([self = shared_from_this()] {
            while (!self->_directory_stop) {
                for (int second = 0; second < 15 * 60 && !self->_directory_stop; ++second) {
                    std::this_thread::sleep_for(1s);
                }
                if (self->_directory_stop) {
                    break;
                }
                self->_directory_ready = directory::publish(self->_machine_name, self->_external_endpoint);
            }
        });
    }

    _beacon_thread = std::thread([this] {
        try {
            asio::io_context ioc;
            asio::ip::udp::socket socket(ioc);
            socket.open(asio::ip::udp::v4());
            socket.set_option(asio::ip::udp::socket::reuse_address(true));
            socket.set_option(asio::socket_base::broadcast(true));
            socket.bind({ asio::ip::address_v4::any(), 54530 });
            socket.non_blocking(true);
            while (!_beacon_stop) {
                std::array<char, 8> query {};
                asio::ip::udp::endpoint from;
                asio::error_code ec;
                while (!ec) {
                    auto n = socket.receive_from(asio::buffer(query), from, 0, ec);
                    if (ec || n < 4) {
                        break;
                    }
                    if (std::string(query.data(), 4) == "ASQ1") {
                        auto packet = announcement_packet();
                        socket.send_to(asio::buffer(packet), from, 0, ec);
                    }
                }
                auto packet = announcement_packet();
                socket.send_to(asio::buffer(packet), { asio::ip::address_v4::broadcast(), 54530 }, 0, ec);
                for (int i = 0; i < 20 && !_beacon_stop; ++i) {
                    std::this_thread::sleep_for(100ms);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("lan beacon stopped: {}", e.what());
        }
    });

    _net_thread = std::thread([self = shared_from_this()] {
        self->_ioc->run();
    });

    spdlog::info("server started");
}

void network_manager::stop_server()
{
    _directory_stop = true;
    if (_directory_thread.joinable()) {
        _directory_thread.join();
    }
    _directory_ready = false;
    _beacon_stop = true;
    if (_beacon_thread.joinable()) {
        _beacon_thread.join();
    }
    local_dashboard::stop();
    if (_ioc) {
        _ioc->stop();
    }
    _net_thread.join();
    _audio_manager->stop();
    _playing_peer_list.clear();
    _udp_servers.clear();
    _ioc = nullptr;
    if (_upnp_mapped) {
        upnp_client::unmap_port(_port);
        _upnp_mapped = false;
    }
    _external_endpoint.clear();
    _upnp_note.clear();
    spdlog::info("server stopped");
}

std::string network_manager::get_external_endpoint() const
{
    return _external_endpoint;
}

std::string network_manager::get_upnp_note() const
{
    return _upnp_note;
}

std::string network_manager::get_token() const
{
    return _token;
}

std::string network_manager::get_machine_name() const
{
    return _machine_name;
}

bool network_manager::directory_ready() const
{
    return _directory_ready;
}

void network_manager::wait_server()
{
    _net_thread.join();
}

bool network_manager::is_running() const
{
    return _ioc != nullptr;
}

asio::awaitable<void> network_manager::read_loop(std::shared_ptr<tcp_socket> peer)
{
    // No token means the original protocol: every command is allowed.
    // With a token, the first command must be cmd_auth and it has to match.
    bool authenticated = _token.empty();

    while (true) {
        cmd_t cmd = cmd_t::cmd_none;
        auto [ec, _] = co_await asio::async_read(*peer, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            close_session(peer);
            spdlog::trace("{} {}", __func__, ec);
            break;
        }

        spdlog::trace("cmd {}", (uint32_t)cmd);

        if (cmd == cmd_t::cmd_auth) {
            uint32_t token_size = 0;
            {
                auto [ec, n] = co_await asio::async_read(*peer, asio::buffer(&token_size, sizeof(token_size)));
                if (ec) {
                    close_session(peer);
                    break;
                }
            }
            if (token_size > 4096) {
                spdlog::warn("auth token too large from {}", peer->remote_endpoint());
                close_session(peer);
                break;
            }

            std::string client_token(token_size, '\0');
            if (token_size > 0) {
                auto [ec, n] = co_await asio::async_read(*peer, asio::buffer(client_token.data(), client_token.size()));
                if (ec) {
                    close_session(peer);
                    break;
                }
            }

            {
                int32_t status = (_token.empty() || client_token == _token) ? 0 : 1;
                std::array<asio::const_buffer, 2> buffers = {
                    asio::buffer(&cmd, sizeof(cmd)),
                    asio::buffer(&status, sizeof(status)),
                };
                auto [write_ec, n] = co_await asio::async_write(*peer, buffers);
                if (write_ec) {
                    close_session(peer);
                    break;
                }
                if (status != 0) {
                    spdlog::warn("auth failed from {}", peer->remote_endpoint());
                    close_session(peer);
                    break;
                }
            }
            authenticated = true;
            spdlog::info("auth success from {}", peer->remote_endpoint());
            continue;
        }

        if (!authenticated) {
            spdlog::warn("unauthenticated command {} from {}", (uint32_t)cmd, peer->remote_endpoint());
            close_session(peer);
            break;
        }

        if (cmd == cmd_t::cmd_get_format) {
            auto format = _audio_manager->get_format_binary();
            auto size = (uint32_t)format.size();
            std::array<asio::const_buffer, 3> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&size, sizeof(size)),
                asio::buffer(format),
            };
            auto [ec, _] = co_await asio::async_write(*peer, buffers);
            if (ec) {
                close_session(peer);
                spdlog::trace("{} {}", __func__, ec);
                break;
            }
        } else if (cmd == cmd_t::cmd_start_play) {
            int id = add_playing_peer(peer);
            if (id <= 0) {
                spdlog::error("{} id error", __func__);
                close_session(peer);
                spdlog::trace("{} {}", __func__, ec);
                break;
            }
            std::array<asio::const_buffer, 2> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&id, sizeof(id)),
            };
            auto [ec, _] = co_await asio::async_write(*peer, buffers);
            if (ec) {
                spdlog::trace("{} {}", __func__, ec);
                close_session(peer);
                break;
            }
            asio::co_spawn(*_ioc, heartbeat_loop(peer), asio::detached);
        } else if (cmd == cmd_t::cmd_heartbeat) {
            auto it = _playing_peer_list.find(peer);
            if (it != _playing_peer_list.end()) {
                it->second->last_tick = std::chrono::steady_clock::now();
            }
        } else if (cmd == cmd_t::cmd_control) {
            uint32_t action = 0;
            uint32_t size = 0;
            auto [action_ec, action_n] = co_await asio::async_read(*peer, asio::buffer(&action, sizeof(action)));
            if (action_ec) {
                close_session(peer);
                break;
            }
            auto [size_ec, size_n] = co_await asio::async_read(*peer, asio::buffer(&size, sizeof(size)));
            if (size_ec || size > 256 * 1024) {
                close_session(peer);
                break;
            }
            std::vector<uint8_t> body(size);
            if (size > 0) {
                auto [payload_ec, payload_n] = co_await asio::async_read(*peer, asio::buffer(body.data(), body.size()));
                if (payload_ec) {
                    close_session(peer);
                    break;
                }
            }
            std::string payload;
            if (!_token.empty() && !body.empty()) {
                auto key = session_crypto::derive_key(_token);
                std::string crypto_error;
                if (!session_crypto::open(key, body, payload, crypto_error)) {
                    spdlog::warn("control decrypt failed from {}: {}", peer->remote_endpoint(), crypto_error);
                    int32_t status = 2;
                    std::array<asio::const_buffer, 2> buffers = {
                        asio::buffer(&cmd, sizeof(cmd)),
                        asio::buffer(&status, sizeof(status)),
                    };
                    co_await asio::async_write(*peer, buffers);
                    close_session(peer);
                    break;
                }
            } else {
                payload.assign(reinterpret_cast<const char*>(body.data()), body.size());
            }
            int32_t status = remote_control::run(action, payload) ? 0 : 1;
            std::array<asio::const_buffer, 2> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&status, sizeof(status)),
            };
            auto [write_ec, write_n] = co_await asio::async_write(*peer, buffers);
            if (write_ec) {
                close_session(peer);
                break;
            }
        } else if (cmd == cmd_t::cmd_list_apps) {
            auto labels = remote_control::shortcut_labels();
            uint32_t count = (uint32_t)labels.size();
            std::array<asio::const_buffer, 2> head = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&count, sizeof(count)),
            };
            auto [write_ec, write_n] = co_await asio::async_write(*peer, head);
            if (write_ec) {
                close_session(peer);
                break;
            }
            bool failed = false;
            for (const auto& label : labels) {
                uint32_t length = (uint32_t)label.size();
                std::array<asio::const_buffer, 2> item = {
                    asio::buffer(&length, sizeof(length)),
                    asio::buffer(label),
                };
                auto [item_ec, item_n] = co_await asio::async_write(*peer, item);
                if (item_ec) {
                    close_session(peer);
                    failed = true;
                    break;
                }
            }
            if (failed) {
                break;
            }
        } else if (cmd == cmd_t::cmd_fs_list || cmd == cmd_t::cmd_fs_get || cmd == cmd_t::cmd_fs_put
                   || cmd == cmd_t::cmd_stats || cmd == cmd_t::cmd_end_process || cmd == cmd_t::cmd_camera) {
            uint32_t size = 0;
            auto [size_ec, size_n] = co_await asio::async_read(*peer, asio::buffer(&size, sizeof(size)));
            if (size_ec || size > 8 * 1024 * 1024) {
                close_session(peer);
                break;
            }
            std::vector<uint8_t> body(size);
            if (size > 0) {
                auto [body_ec, body_n] = co_await asio::async_read(*peer, asio::buffer(body.data(), body.size()));
                if (body_ec) {
                    close_session(peer);
                    break;
                }
            }
            std::string request;
            if (!_token.empty() && !body.empty()) {
                auto key = session_crypto::derive_key(_token);
                std::string crypto_error;
                if (!session_crypto::open(key, body, request, crypto_error)) {
                    int32_t status = 2;
                    std::array<asio::const_buffer, 2> buffers = {
                        asio::buffer(&cmd, sizeof(cmd)),
                        asio::buffer(&status, sizeof(status)),
                    };
                    co_await asio::async_write(*peer, buffers);
                    close_session(peer);
                    break;
                }
            } else {
                request.assign(reinterpret_cast<const char*>(body.data()), body.size());
            }

            int32_t status = 0;
            std::vector<uint8_t> reply;
            std::string remote_error;
            if (cmd == cmd_t::cmd_fs_list) {
                std::vector<pc_remote::dir_entry> entries;
                if (!pc_remote::list_dir(request, entries, remote_error)) {
                    status = 1;
                } else {
                    reply.reserve(entries.size() * 64);
                    auto push_u32 = [&](uint32_t v) {
                        reply.push_back((uint8_t)(v & 0xff));
                        reply.push_back((uint8_t)((v >> 8) & 0xff));
                        reply.push_back((uint8_t)((v >> 16) & 0xff));
                        reply.push_back((uint8_t)((v >> 24) & 0xff));
                    };
                    auto push_u64 = [&](uint64_t v) {
                        for (int i = 0; i < 8; ++i) {
                            reply.push_back((uint8_t)((v >> (8 * i)) & 0xff));
                        }
                    };
                    push_u32((uint32_t)entries.size());
                    for (const auto& entry : entries) {
                        push_u32((uint32_t)entry.name.size());
                        reply.insert(reply.end(), entry.name.begin(), entry.name.end());
                        reply.push_back(entry.directory ? 1 : 0);
                        push_u64(entry.bytes);
                    }
                }
            } else if (cmd == cmd_t::cmd_fs_get) {
                if (!pc_remote::read_file(request, reply, remote_error)) {
                    status = 1;
                    reply.clear();
                }
            } else if (cmd == cmd_t::cmd_fs_put) {
                if (request.size() < 4) {
                    status = 1;
                } else {
                    uint32_t path_len = (uint8_t)request[0] | ((uint8_t)request[1] << 8)
                        | ((uint8_t)request[2] << 16) | ((uint8_t)request[3] << 24);
                    if (path_len > request.size() - 4) {
                        status = 1;
                    } else {
                        std::string path = request.substr(4, path_len);
                        const uint8_t* data = reinterpret_cast<const uint8_t*>(request.data() + 4 + path_len);
                        size_t data_len = request.size() - 4 - path_len;
                        if (!pc_remote::write_file(path, data, data_len, remote_error)) {
                            status = 1;
                        }
                    }
                }
            } else if (cmd == cmd_t::cmd_stats) {
                auto stats = pc_remote::sample_stats();
                auto push_u32 = [&](uint32_t v) {
                    reply.push_back((uint8_t)(v & 0xff));
                    reply.push_back((uint8_t)((v >> 8) & 0xff));
                    reply.push_back((uint8_t)((v >> 16) & 0xff));
                    reply.push_back((uint8_t)((v >> 24) & 0xff));
                };
                push_u32(stats.cpu_percent);
                push_u32(stats.memory_used_mb);
                push_u32(stats.memory_total_mb);
                push_u32((uint32_t)stats.processes.size());
                for (const auto& row : stats.processes) {
                    push_u32(row.pid);
                    push_u32(row.memory_mb);
                    push_u32((uint32_t)row.name.size());
                    reply.insert(reply.end(), row.name.begin(), row.name.end());
                }
            } else if (cmd == cmd_t::cmd_end_process) {
                if (request.size() < 4) {
                    status = 1;
                } else {
                    uint32_t pid = (uint8_t)request[0] | ((uint8_t)request[1] << 8)
                        | ((uint8_t)request[2] << 16) | ((uint8_t)request[3] << 24);
                    if (!pc_remote::end_process(pid, remote_error)) {
                        status = 1;
                    }
                }
            } else if (cmd == cmd_t::cmd_camera) {
                if (!pc_remote::capture_jpeg(reply, remote_error)) {
                    status = 1;
                    reply.clear();
                }
            }

            std::vector<uint8_t> out = reply;
            if (!_token.empty() && !reply.empty()) {
                auto key = session_crypto::derive_key(_token);
                std::string crypto_error;
                if (!session_crypto::seal(key, reply.data(), reply.size(), out, crypto_error)) {
                    status = 2;
                    out.clear();
                }
            }
            uint32_t out_size = (uint32_t)out.size();
            std::array<asio::const_buffer, 4> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&status, sizeof(status)),
                asio::buffer(&out_size, sizeof(out_size)),
                asio::buffer(out),
            };
            auto [write_ec, write_n] = co_await asio::async_write(*peer, buffers);
            if (write_ec) {
                close_session(peer);
                break;
            }
        } else {
            spdlog::error("{} error cmd", __func__);
            close_session(peer);
            break;
        }
    }
    spdlog::trace("stop {}", __func__);
}

asio::awaitable<void> network_manager::heartbeat_loop(std::shared_ptr<tcp_socket> peer)
{
    std::error_code ec;
    size_t _;

    steady_timer timer(*_ioc);
    while (true) {
        timer.expires_after(3s);
        std::tie(ec) = co_await timer.async_wait();
        if (ec) {
            break;
        }

        if (!peer->is_open()) {
            break;
        }

        auto it = _playing_peer_list.find(peer);
        if (it == _playing_peer_list.end()) {
            spdlog::trace("{} it == _playing_peer_list.end()", __func__);
            close_session(peer);
            break;
        }
        if (std::chrono::steady_clock::now() - it->second->last_tick > _heartbeat_timeout) {
            spdlog::info("{} timeout", it->first->remote_endpoint());
            close_session(peer);
            break;
        }

        auto cmd = cmd_t::cmd_heartbeat;
        std::tie(ec, _) = co_await asio::async_write(*peer, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            spdlog::trace("{} {}", __func__, ec);
            close_session(peer);
            break;
        }
    }
    spdlog::trace("stop {}", __func__);
}

asio::awaitable<void> network_manager::accept_tcp_loop(tcp_acceptor acceptor)
{
    while (true) {
        auto peer = std::make_shared<tcp_socket>(acceptor.get_executor());
        auto [ec] = co_await acceptor.async_accept(*peer);
        if (ec) {
            spdlog::error("{} {}", __func__, ec);
            co_return;
        }

        spdlog::info("accept {}", peer->remote_endpoint());

        // No-Delay
        peer->set_option(ip::tcp::no_delay(true), ec);
        if (ec) {
            spdlog::info("{} {}", __func__, ec);
        }

        asio::co_spawn(acceptor.get_executor(), read_loop(peer), asio::detached);
    }
}

asio::awaitable<void> network_manager::accept_udp_loop(std::shared_ptr<udp_socket> socket)
{
    while (true) {
        int id = 0;
        ip::udp::endpoint udp_peer;
        auto [ec, n] = co_await socket->async_receive_from(asio::buffer(&id, sizeof(id)), udp_peer);
        if (ec) {
            // Stopping the context aborts the receive. Anything else (a probe
            // larger than 4 bytes, or Windows reporting ICMP unreachable as
            // connection_reset after a send) must not take the listener down,
            // or one bad datagram ends audio for every client on this port.
            if (ec == asio::error::operation_aborted) {
                spdlog::info("{} {}", __func__, ec);
                co_return;
            }
            spdlog::warn("udp receive failed: {}", ec.message());
            continue;
        }
        if (n != sizeof(id)) {
            continue;
        }

        fill_udp_peer(id, udp_peer, socket);
    }
}

auto network_manager::close_session(std::shared_ptr<tcp_socket>& peer) -> playing_peer_list_t::iterator
{
    spdlog::info("close {}", peer->remote_endpoint());
    auto it = remove_playing_peer(peer);
    peer->shutdown(ip::tcp::socket::shutdown_both);
    peer->close();
    return it;
}

int network_manager::add_playing_peer(std::shared_ptr<tcp_socket>& peer)
{
    if (_playing_peer_list.contains(peer)) {
        spdlog::error("{} repeat add tcp://{}", __func__, peer->remote_endpoint());
        return 0;
    }

    auto info = _playing_peer_list[peer] = std::make_shared<peer_info_t>();
    static int g_id = 0;
    info->id = ++g_id;
    info->last_tick = std::chrono::steady_clock::now();
    _client_count++;
    note_presence();

    spdlog::trace("{} add id:{} tcp://{}", __func__, info->id, peer->remote_endpoint());
    return info->id;
}

auto network_manager::remove_playing_peer(std::shared_ptr<tcp_socket>& peer) -> playing_peer_list_t::iterator
{
    auto it = _playing_peer_list.find(peer);
    if (it == _playing_peer_list.end()) {
        spdlog::error("{} repeat remove tcp://{}", __func__, peer->remote_endpoint());
        return it;
    }

    it = _playing_peer_list.erase(it);
    if (_client_count > 0) {
        _client_count--;
    }
    note_presence();
    spdlog::trace("{} remove tcp://{}", __func__, peer->remote_endpoint());
    return it;
}

std::string network_manager::announcement_packet() const
{
    auto name = _machine_name.empty() ? machine_name::local_name() : _machine_name;
    std::string packet = "ASN1";
    uint16_t port = _port;
    uint16_t clients = (uint16_t)_client_count.load();
    uint8_t flags = _token.empty() ? 0 : 1;
    packet.append(reinterpret_cast<const char*>(&port), sizeof(port));
    packet.append(reinterpret_cast<const char*>(&clients), sizeof(clients));
    packet.push_back((char)flags);
    packet += name;
    return packet;
}

void network_manager::note_presence() const
{
    auto name = _machine_name.empty() ? machine_name::local_name() : _machine_name;
    local_dashboard::note(name, _client_count.load());
}

void network_manager::fill_udp_peer(int id, asio::ip::udp::endpoint udp_peer, std::shared_ptr<udp_socket> socket)
{
    auto it = std::find_if(_playing_peer_list.begin(), _playing_peer_list.end(), [id](const playing_peer_list_t::value_type& e) {
        return e.second->id == id;
    });

    if (it == _playing_peer_list.cend()) {
        spdlog::error("{} no tcp peer id:{} udp://{}", __func__, id, udp_peer);
        return;
    }

    it->second->udp_peer = udp_peer;
    it->second->udp_socket = socket;
    spdlog::info("{} fill udp peer id:{} tcp://{} udp://{}", __func__, id, it->first->remote_endpoint(), udp_peer);
}

void network_manager::broadcast_audio_data(const char* data, size_t count, int block_align)
{
    if (count <= 0) {
        return;
    }
    // spdlog::trace("broadcast_audio_data count: {}", count);

    // divide udp frame
    constexpr int mtu = 1492;
    int max_seg_size = mtu - 20 - 8;
    max_seg_size -= max_seg_size % block_align; // one single sample can't be divided

    std::list<std::shared_ptr<std::vector<uint8_t>>> seg_list;

    for (int begin_pos = 0; begin_pos < count;) {
        const int real_seg_size = std::min((int)count - begin_pos, max_seg_size);
        auto seg = std::make_shared<std::vector<uint8_t>>(real_seg_size);
        std::copy((const uint8_t*)data + begin_pos, (const uint8_t*)data + begin_pos + real_seg_size, seg->begin());
        seg_list.push_back(seg);
        begin_pos += real_seg_size;
    }

    // asio::io_context::post() was removed in newer Asio. The free function
    // is the supported way to queue work on the network thread.
    asio::post(*_ioc, [seg_list = std::move(seg_list), self = shared_from_this()] {
        for (const auto& seg : seg_list) {
            for (auto& [peer, info] : self->_playing_peer_list) {
                auto socket = info->udp_socket;
                if (!socket) {
                    // Not registered yet.
                    continue;
                }
                socket->async_send_to(asio::buffer(*seg), info->udp_peer, [seg](const asio::error_code& ec, std::size_t bytes_transferred) { });
            }
        }
    });
}
