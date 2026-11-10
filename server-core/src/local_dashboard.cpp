/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#include "local_dashboard.hpp"
#include "remote_control.hpp"

#include "pre_asio.hpp"
#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>

namespace local_dashboard {

namespace {

std::thread g_thread;
std::atomic<bool> g_stop { false };
std::mutex g_status_mutex;
std::string g_name;
int g_clients = 0;

std::string json_escape(const std::string& text)
{
    std::string out;
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back((char)c);
        } else if (c >= 32) {
            out.push_back((char)c);
        }
    }
    return out;
}

std::string status_json()
{
    std::lock_guard<std::mutex> lock(g_status_mutex);
    std::ostringstream out;
    out << "{\"name\":\"" << json_escape(g_name) << "\",\"clients\":" << g_clients << ",\"shortcuts\":[";
    auto labels = remote_control::shortcut_labels();
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << '"' << json_escape(labels[i]) << '"';
    }
    out << "]}";
    return out.str();
}

const char* page()
{
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Audio Share</title>
<style>
  :root { color-scheme: dark; --ink:#e8f6ea; --dim:#8ea398; --line:#243028; --bg:#070b09; --live:#7dff6b; --wait:#c45b4a; }
  * { box-sizing: border-box; }
  body { margin:0; min-height:100vh; font-family: "Segoe UI", sans-serif; color:var(--ink); background:
    radial-gradient(900px 420px at 50% -10%, rgba(125,255,107,.16), transparent 60%), var(--bg); }
  body.waiting { background: radial-gradient(700px 320px at 50% -10%, rgba(196,91,74,.14), transparent 60%), var(--bg); }
  main { max-width: 880px; margin: 0 auto; padding: 36px 20px 64px; }
  h1 { font-weight: 560; letter-spacing: .04em; font-size: 28px; margin: 0 0 6px; }
  .sub { color: var(--dim); min-height: 1.4em; }
  .pulse { width: 14px; height: 14px; border-radius: 50%; display:inline-block; margin-right: 8px; background: var(--wait); box-shadow: 0 0 0 0 rgba(196,91,74,.5); animation: beat 2.6s infinite; vertical-align: -1px; }
  body.live .pulse { background: var(--live); box-shadow: 0 0 18px var(--live); animation-duration: 1.3s; }
  @keyframes beat { 50% { transform: scale(.72); opacity: .55; } }
  .grid { display:grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; margin-top: 28px; }
  button, .field button { background:#101814; color:var(--ink); border:1px solid var(--line); border-radius: 14px; padding: 14px 12px; cursor:pointer; font: inherit; }
  button:hover { border-color: var(--live); }
  body.waiting button:hover { border-color: var(--wait); }
  form { display:flex; gap:8px; margin-top: 22px; flex-wrap: wrap; }
  input { flex:1; min-width: 140px; background:#0c1210; color:var(--ink); border:1px solid var(--line); border-radius: 12px; padding: 12px; font: inherit; }
  h2 { font-size: 13px; letter-spacing: .16em; text-transform: uppercase; color: var(--dim); margin: 32px 0 0; }
</style>
</head>
<body class="waiting">
<main>
  <h1><span class="pulse"></span><span id="title">Waiting</span></h1>
  <div class="sub" id="sub">No phone on the line.</div>
  <h2>Room</h2>
  <div class="grid" id="controls"></div>
  <h2>Shortcuts</h2>
  <div class="grid" id="shortcuts"></div>
  <form id="add">
    <input id="label" placeholder="Shortcut name" required>
    <input id="path" placeholder="C:\Path\App.exe" required>
    <button type="submit">Pin on this PC</button>
  </form>
  <h2>Browser</h2>
  <form id="web">
    <input id="url" placeholder="example.com">
    <button type="submit">Open on this PC</button>
  </form>
</main>
<script>
const actions = [
  [1,"Volume up"],[2,"Volume down"],[3,"Mute"],
  [4,"Play / pause"],[5,"Next"],[6,"Previous"],
  [7,"Settings"],[8,"Sound"],[9,"Display"],
  [10,"Wi-Fi"],[11,"Bluetooth"],[17,"Night light"],
  [13,"Files"],[14,"Task manager"],[18,"Notepad"],
  [19,"Calculator"],[15,"Lock PC"]
];
const controls = document.getElementById("controls");
const shortcuts = document.getElementById("shortcuts");
function button(label, on) {
  const b = document.createElement("button");
  b.textContent = label;
  b.onclick = on;
  return b;
}
async function act(action, payload) {
  await fetch("/api/control?action=" + action, {method:"POST", body: payload || ""});
}
actions.forEach(([id, label]) => controls.appendChild(button(label, () => act(id))));
async function tick() {
  const data = await (await fetch("/api/status")).json();
  const live = data.clients > 0;
  document.body.className = live ? "live" : "waiting";
  document.getElementById("title").textContent = data.name || "This PC";
  document.getElementById("sub").textContent = live
    ? data.clients + (data.clients === 1 ? " phone on the line" : " phones on the line")
    : "Quiet. Waiting for the next line.";
  shortcuts.replaceChildren(...(data.shortcuts || []).map(name => button(name, () => act(16, name))));
}
document.getElementById("add").onsubmit = async (event) => {
  event.preventDefault();
  await fetch("/api/shortcut", {method:"POST", body: document.getElementById("label").value + "\n" + document.getElementById("path").value});
  document.getElementById("label").value = "";
  document.getElementById("path").value = "";
  tick();
};
document.getElementById("web").onsubmit = async (event) => {
  event.preventDefault();
  await act(12, document.getElementById("url").value);
};
tick();
setInterval(tick, 2000);
</script>
</body>
</html>)HTML";
}

void http_reply(asio::ip::tcp::socket& socket, const std::string& type, const std::string& body, int status = 200)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Not Found") << "\r\n";
    out << "Content-Type: " << type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\nCache-Control: no-store\r\n\r\n";
    out << body;
    auto text = out.str();
    asio::write(socket, asio::buffer(text));
}

std::string query_value(const std::string& target, const std::string& key)
{
    auto q = target.find('?');
    if (q == std::string::npos) {
        return {};
    }
    std::string query = target.substr(q + 1);
    std::string needle = key + "=";
    auto at = query.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    auto value = query.substr(at + needle.size());
    auto end = value.find('&');
    if (end != std::string::npos) {
        value = value.substr(0, end);
    }
    return value;
}

void serve()
{
    try {
        asio::io_context ioc;
        asio::ip::tcp::acceptor acceptor(ioc, { asio::ip::make_address("127.0.0.1"), 65531 });
        acceptor.non_blocking(true);
        while (!g_stop) {
            asio::ip::tcp::socket socket(ioc);
            asio::error_code ec;
            acceptor.accept(socket, ec);
            if (ec) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                continue;
            }
            std::string request(8192, '\0');
            size_t got = 0;
            while (got < request.size()) {
                size_t n = socket.read_some(asio::buffer(request.data() + got, request.size() - got), ec);
                if (ec || n == 0) {
                    break;
                }
                got += n;
                if (request.find("\r\n\r\n") != std::string::npos && request.find("Content-Length") == std::string::npos) {
                    break;
                }
                auto header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    auto len_at = request.find("Content-Length:");
                    int length = 0;
                    if (len_at != std::string::npos && len_at < header_end) {
                        length = std::atoi(request.c_str() + len_at + 15);
                    }
                    if ((int)got >= (int)header_end + 4 + length) {
                        break;
                    }
                }
            }
            request.resize(got);
            auto line_end = request.find("\r\n");
            auto header_end = request.find("\r\n\r\n");
            if (line_end == std::string::npos || header_end == std::string::npos) {
                continue;
            }
            std::string line = request.substr(0, line_end);
            std::string body = request.substr(header_end + 4);
            auto sp1 = line.find(' ');
            auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            std::string method = sp1 == std::string::npos ? "" : line.substr(0, sp1);
            std::string target = (sp1 == std::string::npos || sp2 == std::string::npos) ? "/" : line.substr(sp1 + 1, sp2 - sp1 - 1);
            auto path = target.substr(0, target.find('?'));
            if (method == "GET" && (path == "/" || path == "/index.html")) {
                http_reply(socket, "text/html; charset=utf-8", page());
            } else if (method == "GET" && path == "/api/status") {
                http_reply(socket, "application/json", status_json());
            } else if (method == "POST" && path == "/api/control") {
                uint32_t action = (uint32_t)std::strtoul(query_value(target, "action").c_str(), nullptr, 10);
                bool ok = remote_control::run(action, body);
                http_reply(socket, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
            } else if (method == "POST" && path == "/api/shortcut") {
                auto split = body.find('\n');
                bool ok = split != std::string::npos && remote_control::add_shortcut(body.substr(0, split), body.substr(split + 1));
                http_reply(socket, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
            } else {
                http_reply(socket, "text/plain", "not found", 404);
            }
        }
    } catch (const std::exception&) {
    }
}

} // namespace

void note(const std::string& name, int clients)
{
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_name = name;
    g_clients = clients;
}

void start()
{
    if (g_thread.joinable()) {
        return;
    }
    g_stop = false;
    g_thread = std::thread(serve);
}

void stop()
{
    g_stop = true;
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

} // namespace local_dashboard
