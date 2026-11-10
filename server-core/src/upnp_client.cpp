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

#include "upnp_client.hpp"
#include "pre_asio.hpp"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <sstream>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace upnp_client {

namespace {

using namespace std::chrono_literals;

struct service_info {
    std::string control_url;
    std::string service_type;
    std::string host;
    std::string port;
    std::string path;
};

// The gateway found by the last discovery, reused for removing the mapping.
service_info g_service;
bool g_has_service = false;

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trim(const std::string& s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string find_between(const std::string& text, const std::string& open, const std::string& close, size_t from = 0)
{
    auto begin = text.find(open, from);
    if (begin == std::string::npos) {
        return {};
    }
    begin += open.size();
    auto end = text.find(close, begin);
    if (end == std::string::npos) {
        return {};
    }
    return text.substr(begin, end - begin);
}

std::string header_value(const std::string& response, const std::string& name)
{
    std::istringstream stream(response);
    std::string line;
    auto prefix = to_lower(name) + ":";
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (to_lower(line).rfind(prefix, 0) == 0) {
            return trim(line.substr(prefix.size()));
        }
    }
    return {};
}

bool parse_http_url(const std::string& url, std::string& host, std::string& port, std::string& path)
{
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos || to_lower(url.substr(0, scheme_end)) != "http") {
        return false;
    }
    auto authority_begin = scheme_end + 3;
    auto path_begin = url.find('/', authority_begin);
    auto authority = url.substr(authority_begin, path_begin == std::string::npos ? std::string::npos : path_begin - authority_begin);
    path = path_begin == std::string::npos ? "/" : url.substr(path_begin);
    if (authority.empty()) {
        return false;
    }
    auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
        host = authority;
        port = "80";
    } else {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    return !host.empty() && !port.empty();
}

// scheme://authority of a URL, used to resolve relative control URLs.
std::string url_base(const std::string& url)
{
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {};
    }
    auto path_begin = url.find('/', scheme_end + 3);
    return path_begin == std::string::npos ? url : url.substr(0, path_begin);
}

std::string resolve_url(const std::string& base, const std::string& relative)
{
    if (to_lower(relative).rfind("http://", 0) == 0) {
        return relative;
    }
    if (!relative.empty() && relative.front() == '/') {
        return base + relative;
    }
    return base + "/" + relative;
}

bool http_request(const std::string& host, const std::string& port, const std::string& request, std::string& response)
{
    try {
        asio::ip::tcp::iostream stream;
        stream.expires_after(3s);
        stream.connect(host, port);
        if (!stream) {
            return false;
        }
        stream << request;
        stream.flush();
        std::ostringstream body;
        body << stream.rdbuf();
        response = body.str();
        return !response.empty();
    } catch (const std::exception& e) {
        spdlog::debug("upnp http request failed: {}", e.what());
        return false;
    }
}

bool http_get(const std::string& url, std::string& body)
{
    std::string host, port, path;
    if (!parse_http_url(url, host, port, path)) {
        return false;
    }
    std::string request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + port + "\r\n";
    request += "Connection: close\r\n";
    request += "User-Agent: audio-share\r\n\r\n";

    std::string response;
    if (!http_request(host, port, request, response)) {
        return false;
    }
    auto split = response.find("\r\n\r\n");
    body = split == std::string::npos ? response : response.substr(split + 4);
    return true;
}

std::vector<std::string> discover_gateways()
{
    std::vector<std::string> locations;
    try {
        asio::io_context ioc;
        asio::ip::udp::socket socket(ioc);
        std::error_code ec;
        socket.open(asio::ip::udp::v4(), ec);
        if (ec) {
            return locations;
        }
        socket.bind(asio::ip::udp::endpoint(asio::ip::address_v4::any(), 0), ec);
        if (ec) {
            return locations;
        }
        socket.non_blocking(true);

        const std::array<const char*, 2> search_targets = {
            "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
            "upnp:rootdevice",
        };
        const auto gateway = asio::ip::udp::endpoint(asio::ip::make_address_v4("239.255.255.250"), 1900);
        for (auto search_target : search_targets) {
            std::string message = "M-SEARCH * HTTP/1.1\r\n";
            message += "HOST: 239.255.255.250:1900\r\n";
            message += "MAN: \"ssdp:discover\"\r\n";
            message += "MX: 1\r\n";
            message += "ST: ";
            message += search_target;
            message += "\r\n\r\n";
            socket.send_to(asio::buffer(message), gateway, 0, ec);
        }

        auto deadline = std::chrono::steady_clock::now() + 1500ms;
        while (std::chrono::steady_clock::now() < deadline && locations.size() < 8) {
            std::array<char, 4096> buffer {};
            asio::ip::udp::endpoint sender;
            auto size = socket.receive_from(asio::buffer(buffer), sender, 0, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(30ms);
                continue;
            }
            if (ec) {
                break;
            }
            std::string response(buffer.data(), size);
            auto location = header_value(response, "location");
            if (!location.empty() && std::find(locations.begin(), locations.end(), location) == locations.end()) {
                locations.push_back(location);
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("upnp discovery failed: {}", e.what());
    }
    return locations;
}

bool parse_description(const std::string& location, service_info& info)
{
    std::string description;
    if (!http_get(location, description)) {
        return false;
    }

    auto base = find_between(description, "<URLBase>", "</URLBase>");
    if (base.empty()) {
        base = url_base(location);
    }

    // Walk over the services and pick the first WAN connection service.
    size_t position = 0;
    while (true) {
        auto service_begin = description.find("<service>", position);
        if (service_begin == std::string::npos) {
            break;
        }
        auto service_end = description.find("</service>", service_begin);
        if (service_end == std::string::npos) {
            break;
        }
        auto block = description.substr(service_begin, service_end - service_begin);
        position = service_end + std::string("</service>").size();

        auto service_type = trim(find_between(block, "<serviceType>", "</serviceType>"));
        if (service_type.find("WANIPConnection") == std::string::npos &&
            service_type.find("WANPPPConnection") == std::string::npos) {
            continue;
        }

        auto control_url = trim(find_between(block, "<controlURL>", "</controlURL>"));
        if (control_url.empty()) {
            continue;
        }

        info.service_type = service_type;
        info.control_url = resolve_url(base, control_url);
        if (!parse_http_url(info.control_url, info.host, info.port, info.path)) {
            continue;
        }
        spdlog::debug("upnp gateway service: {} at {}", info.service_type, info.control_url);
        return true;
    }

    return false;
}

std::string soap_body(const std::string& service_type, const std::string& action, const std::string& arguments)
{
    std::string body = "<?xml version=\"1.0\"?>";
    body += "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ";
    body += "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>";
    body += "<u:" + action + " xmlns:u=\"" + service_type + "\">";
    body += arguments;
    body += "</u:" + action + "></s:Body></s:Envelope>";
    return body;
}

bool soap_request(const service_info& info, const std::string& action, const std::string& arguments, std::string& body)
{
    auto payload = soap_body(info.service_type, action, arguments);
    std::string request = "POST " + info.path + " HTTP/1.1\r\n";
    request += "Host: " + info.host + ":" + info.port + "\r\n";
    request += "Content-Type: text/xml; charset=\"utf-8\"\r\n";
    request += "SOAPAction: \"" + info.service_type + "#" + action + "\"\r\n";
    request += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
    request += "Connection: close\r\n";
    request += "User-Agent: audio-share\r\n\r\n";
    request += payload;

    std::string response;
    if (!http_request(info.host, info.port, request, response)) {
        return false;
    }
    auto split = response.find("\r\n\r\n");
    body = split == std::string::npos ? response : response.substr(split + 4);
    return true;
}

bool delete_mapping(const service_info& info, uint16_t port, const std::string& protocol);

bool add_mapping(const service_info& info, uint16_t port, const std::string& internal_ip, const std::string& description, const std::string& protocol)
{
    std::string arguments;
    arguments += "<NewRemoteHost></NewRemoteHost>";
    arguments += "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>";
    arguments += "<NewProtocol>" + protocol + "</NewProtocol>";
    arguments += "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>";
    arguments += "<NewInternalClient>" + internal_ip + "</NewInternalClient>";
    arguments += "<NewEnabled>1</NewEnabled>";
    arguments += "<NewPortMappingDescription>" + description + "</NewPortMappingDescription>";
    arguments += "<NewLeaseDuration>0</NewLeaseDuration>";

    std::string body;
    if (!soap_request(info, "AddPortMapping", arguments, body)) {
        return false;
    }

    // 718: ConflictInMappingEntry, a rule for this port already exists
    // (for example from a previous run). Replace it and try once more.
    if (body.find("<errorCode>718</errorCode>") != std::string::npos) {
        delete_mapping(info, port, protocol);
        body.clear();
        if (!soap_request(info, "AddPortMapping", arguments, body)) {
            return false;
        }
    }

    if (body.find("<errorCode>") != std::string::npos) {
        spdlog::debug("upnp AddPortMapping {} failed: {}", protocol, trim(find_between(body, "<errorDescription>", "</errorDescription>")));
        return false;
    }
    return true;
}

bool delete_mapping(const service_info& info, uint16_t port, const std::string& protocol)
{
    std::string arguments;
    arguments += "<NewRemoteHost></NewRemoteHost>";
    arguments += "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>";
    arguments += "<NewProtocol>" + protocol + "</NewProtocol>";

    std::string body;
    return soap_request(info, "DeletePortMapping", arguments, body) && body.find("<errorCode>") == std::string::npos;
}

std::string get_external_ip(const service_info& info)
{
    std::string body;
    if (!soap_request(info, "GetExternalIPAddress", "", body)) {
        return {};
    }
    return trim(find_between(body, "<NewExternalIPAddress>", "</NewExternalIPAddress>"));
}

bool is_private_ipv4(const std::string& address)
{
    std::error_code ec;
    auto value = asio::ip::make_address_v4(address, ec).to_uint();
    if (ec) {
        return false;
    }
    return (value & 0xff000000) == 0x0a000000       // 10.0.0.0/8
        || (value & 0xfff00000) == 0xac100000       // 172.16.0.0/12
        || (value & 0xffff0000) == 0xc0a80000       // 192.168.0.0/16
        || (value & 0xffc00000) == 0x64400000;      // 100.64.0.0/10 (CGNAT)
}

} // namespace

map_result map_port(uint16_t port, const std::string& internal_ip, const std::string& description)
{
    map_result result;

    auto locations = discover_gateways();
    if (locations.empty()) {
        result.error = "no UPnP gateway found (SSDP)";
        return result;
    }

    for (const auto& location : locations) {
        service_info info;
        if (!parse_description(location, info)) {
            continue;
        }

        auto tcp_ok = add_mapping(info, port, internal_ip, description, "TCP");
        auto udp_ok = add_mapping(info, port, internal_ip, description, "UDP");
        if (!tcp_ok || !udp_ok) {
            if (tcp_ok) {
                delete_mapping(info, port, "TCP");
            }
            if (udp_ok) {
                delete_mapping(info, port, "UDP");
            }
            continue;
        }

        g_service = info;
        g_has_service = true;
        result.external_ip = get_external_ip(info);
        result.success = true;
        if (result.external_ip.empty()) {
            result.error = "port forwarded, but the public address is unknown";
        } else if (is_private_ipv4(result.external_ip)) {
            result.error = "the gateway reports a private address (" + result.external_ip + "), there may be a second NAT";
        }
        return result;
    }

    result.error = "the UPnP gateway did not accept the port mapping";
    return result;
}

void unmap_port(uint16_t port)
{
    if (!g_has_service) {
        return;
    }
    delete_mapping(g_service, port, "TCP");
    delete_mapping(g_service, port, "UDP");
    g_has_service = false;
}

} // namespace upnp_client
