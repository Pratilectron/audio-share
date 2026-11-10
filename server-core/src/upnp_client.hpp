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

#ifndef UPNP_CLIENT_HPP
#define UPNP_CLIENT_HPP

#include <cstdint>
#include <string>

// Minimal UPnP IGD (Internet Gateway Device) client.
//
// It discovers the router with SSDP, asks it to forward a TCP and a UDP port
// to this machine and reads back the public (external) address. Only the
// WANIPConnection / WANPPPConnection services are used, which is what home
// routers expose for IPv4 port forwarding.
namespace upnp_client {

struct map_result {
    bool success = false;
    std::string external_ip;
    std::string error;
};

// Forward `port` (both TCP and UDP) on the gateway to `internal_ip:port`.
// Blocks for at most a few seconds.
map_result map_port(uint16_t port, const std::string& internal_ip, const std::string& description);

// Remove the forwarding rules created by map_port(). Best effort.
void unmap_port(uint16_t port);

} // namespace upnp_client

#endif // !UPNP_CLIENT_HPP
