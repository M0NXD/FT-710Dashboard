#include "web_server.hpp"
#include "browser_audio.hpp"
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace web {
namespace {
std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}
std::string lower(std::string value) { for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return value; }
std::string decode(const std::string& value) {
    std::string result;
    auto nibble = [](char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') result += ' ';
        else if (value[i] == '%') {
            if (i + 2 >= value.size() || nibble(value[i + 1]) < 0 || nibble(value[i + 2]) < 0) throw std::invalid_argument("Invalid form encoding");
            char c = static_cast<char>(nibble(value[i + 1]) * 16 + nibble(value[i + 2])); if (!c) throw std::invalid_argument("NUL in form");
            result += c; i += 2;
        } else result += value[i];
    }
    return result;
}
std::string cookieToken(const std::string& cookie) {
    std::istringstream input(cookie); std::string value;
    constexpr std::string_view prefix = "ft710_tls_session=";
    while (std::getline(input, value, ';')) { value = trim(value); if (value.starts_with(prefix)) return value.substr(prefix.size()); }
    return "";
}
bool same(const std::string& a, const std::string& b) {
    unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
    for (unsigned i = 0; i < std::max(a.size(), b.size()); ++i) difference |= (i < a.size() ? static_cast<unsigned char>(a[i]) : 0) ^ (i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
    return difference == 0;
}
std::string resource(unsigned id) {
    auto module = GetModuleHandleW(nullptr); auto item = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!item) throw std::runtime_error("Embedded web asset is missing");
    auto loaded = LoadResource(module, item); return {static_cast<const char*>(LockResource(loaded)), SizeofResource(module, item)};
}
std::string response(unsigned status, const std::string& type, const std::string& body, const std::string& extra = "", const std::string& host = "") {
    return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : status == 401 ? " Unauthorized" : status == 403 ? " Forbidden" : status == 429 ? " Too Many Requests" : " Error") +
        "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n" +
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'" + (host.empty() ? "" : " wss://" + host) + "; media-src 'self' blob:; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\nPermissions-Policy: microphone=(self), camera=()\r\n" + extra + "\r\n" + body;
}
std::string setupPage(const std::string& host, const std::string& fingerprint) {
    // Host is restricted to URL-safe address characters before reaching here.
    return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>FT-710 web access</title><link rel=\"stylesheet\" href=\"/app.css\"></head><body><main class=\"login panel\">"
        "<div class=\"eyebrow\">FT-710 STATION SETUP</div><h1>Enable microphone access in Safari</h1>"
        "<p>The dashboard uses HTTPS for radio controls and audio. Install this station's certificate once on your iPhone or iPad, then enable trust.</p>"
        "<ol><li><a href=\"/station.mobileconfig\">Download the iPhone / iPad certificate profile</a>. Allow the download when Safari asks.</li>"
        "<li>Open <b>Settings &rarr; Profile Downloaded</b> and tap <b>Install</b>. If that item is missing, look in <b>General &rarr; VPN &amp; Device Management</b>.</li>"
        "<li>Open <b>Settings &rarr; General &rarr; About &rarr; Certificate Trust Settings</b>. Enable full trust for <b>M0NXD FT710 Local Web Root</b>.</li>"
        "<li>Return to Safari and <a href=\"https://" + host + "/\">open the HTTPS dashboard</a>. Enter the access code shown by <b>Web: 8888</b> in the desktop app.</li></ol>"
        "<p>In <b>Audio routing</b>, choose the radio USB devices, select <b>Use browser audio mappings</b>, then tap <b>Listen in this browser</b> and <b>Arm browser microphone</b>. Allow microphone access when Safari asks.</p>"
        "<p>This profile contains only the station's public root certificate. Compare its SHA-256 fingerprint with the desktop <b>Web: 8888</b> dialog:</p><pre>" + fingerprint + "</pre>"
        "<p><a href=\"/certificate.cer\">Download the root certificate for another device</a></p>"
        "<p><a href=\"https://support.apple.com/en-gb/102390\" rel=\"noreferrer\">Apple's certificate trust instructions</a></p>"
        "<p>Keep the dashboard running. This HTTP page is only for certificate setup; it does not accept station sign-ins or radio commands.</p>"
        "</main></body></html>";
}
bool frame(Stream& stream, unsigned opcode, const std::string& payload) {
    std::string packet; packet += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126) packet += static_cast<char>(payload.size());
    else if (payload.size() <= 65535) { packet += 126; packet += static_cast<char>(payload.size() >> 8); packet += static_cast<char>(payload.size()); }
    else { packet += 127; for (int shift = 56; shift >= 0; shift -= 8) packet += static_cast<char>(static_cast<std::uint64_t>(payload.size()) >> shift); }
    packet += payload; return stream.send(packet);
}
bool validHost(const std::string& host) { return !host.empty() && host.size() < 256 && host.ends_with(":8888") && std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == ':' || c == '[' || c == ']' || c == '-'; }); }
}
std::string json(const std::string& value) {
    std::string output = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { output += '\\'; output += c; }
        else if (c < 32) { output += "\\u00"; output += digits[c >> 4]; output += digits[c & 15]; }
        else output += c;
    }
    return output + '"';
}
std::map<std::string, std::string> form(const std::string& text) {
    if (text.size() > 8192) throw std::invalid_argument("Command is too large");
    std::map<std::string, std::string> fields; std::istringstream input(text); std::string part;
    while (std::getline(input, part, '&')) {
        auto equal = part.find('='); if (equal == std::string::npos) throw std::invalid_argument("Invalid command field");
        auto key = decode(part.substr(0, equal)); if (key.size() > 64 || fields.size() >= 32 || fields.contains(key)) throw std::invalid_argument("Invalid command fields");
        fields.emplace(std::move(key), decode(part.substr(equal + 1)));
    }
    return fields;
}
Server::Server(Handler handler, Log log) : handler_(std::move(handler)), log_(std::move(log)) {}
Server::~Server() { stop(); }
void Server::start(const std::wstring& certificateFolder, const std::string& accessCode) {
    stop(); setAccessCode(accessCode);
    WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data)) throw std::runtime_error("Could not initialize web networking"); winsock_ = true;
    try {
        credentials_ = std::make_unique<Credentials>(); credentials_->create(certificateFolder);
        listener_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        DWORD dual = 0; BOOL exclusive = TRUE;
        bool dualStack = listener_ != INVALID_SOCKET && setsockopt(listener_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&dual), sizeof dual) == 0;
        if (!dualStack) {
            if (listener_ != INVALID_SOCKET) closesocket(listener_);
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == INVALID_SOCKET) throw std::runtime_error("Cannot create web socket (Winsock " + std::to_string(WSAGetLastError()) + ")");
        }
        setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof exclusive);
        sockaddr_in6 address6{}; address6.sin6_family = AF_INET6; address6.sin6_addr = in6addr_any; address6.sin6_port = htons(8888);
        sockaddr_in address4{}; address4.sin_family = AF_INET; address4.sin_addr.s_addr = htonl(INADDR_ANY); address4.sin_port = htons(8888);
        auto* address = dualStack ? reinterpret_cast<sockaddr*>(&address6) : reinterpret_cast<sockaddr*>(&address4);
        if (bind(listener_, address, dualStack ? sizeof address6 : sizeof address4) || listen(listener_, 32)) throw std::runtime_error("Cannot listen on port 8888 (Winsock " + std::to_string(WSAGetLastError()) + "). Another dashboard/server may be using it.");
        running_ = true; acceptThread_ = std::thread([this] { acceptLoop(); });
        log_(std::string("Web dashboard listening on all ") + (dualStack ? "IPv4/IPv6" : "IPv4") + " addresses, HTTPS port 8888. HTTP serves certificate setup only. Use the Web: 8888 button for access details.");
    } catch (...) { stop(); throw; }
}
void Server::stop() {
    running_ = false;
    if (acceptThread_.joinable()) acceptThread_.join();
    if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
    { std::lock_guard lock(mutex_); for (auto& client : sessions_) if (client->socket != INVALID_SOCKET) shutdown(client->socket, SD_BOTH); }
    for (auto& client : sessions_) if (client->thread.joinable()) client->thread.join();
    sessions_.clear(); clients_ = 0;
    { std::lock_guard lock(loginMutex_); tokens_.clear(); attempts_.clear(); }
    credentials_.reset();
    if (winsock_) { WSACleanup(); winsock_ = false; }
}
void Server::setAccessCode(const std::string& accessCode) {
    std::lock_guard lock(loginMutex_);
    accessCode_ = accessCode; tokens_.clear(); attempts_.clear(); ++accessRevision_;
}
void Server::publish(std::string state, bool urgent) {
    std::lock_guard lock(stateMutex_);
    state_ = std::move(state);
    ++revision_;
    if (urgent) urgentRevision_ = revision_;
}
void Server::spectrum(const audio::SpectrumFrame& frameData) {
    if (!frameData.valid) { std::lock_guard lock(stateMutex_); spectrum_.clear(); spectrumTick_ = 0; ++spectrumRevision_; return; }
    std::string payload(12 + frameData.db.size() * 4, 0);
    std::uint32_t type = 2, rate = static_cast<std::uint32_t>(frameData.sampleRate), generation = static_cast<std::uint32_t>(frameData.generation);
    std::memcpy(payload.data(), &type, 4); std::memcpy(payload.data() + 4, &rate, 4); std::memcpy(payload.data() + 8, &generation, 4); std::memcpy(payload.data() + 12, frameData.db.data(), frameData.db.size() * 4);
    std::lock_guard lock(stateMutex_); spectrum_ = std::move(payload); spectrumTick_ = GetTickCount64(); ++spectrumRevision_;
}
void Server::rfSpectrum(const rf_scope::Frame& frameData, std::uint64_t centerHz, std::uint32_t spanHz) {
    std::lock_guard lock(stateMutex_);
    if (!frameData.valid || !centerHz || !spanHz) { rfSpectrum_.clear(); rfSpectrumTick_ = 0; ++rfSpectrumRevision_; return; }
    std::string payload; payload.reserve(12 + frameData.bins.size());
    auto append32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) payload.push_back(static_cast<char>(value >> shift));
    };
    append32(4); append32(static_cast<std::uint32_t>(centerHz)); append32(spanHz);
    payload.append(reinterpret_cast<const char*>(frameData.bins.data()), frameData.bins.size());
    rfSpectrum_ = std::move(payload); rfSpectrumTick_ = GetTickCount64(); ++rfSpectrumRevision_;
}
bool Server::authenticated(const std::string& cookie) {
    auto token = cookieToken(cookie); std::lock_guard lock(loginMutex_); auto item = tokens_.find(token);
    return item != tokens_.end() && item->second > GetTickCount64();
}
void Server::acceptLoop() {
    while (running_) {
        fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable); timeval timeout{0,250000};
        if (select(0, &readable, nullptr, nullptr, &timeout) > 0 && running_) {
            sockaddr_storage peer{}; int size = sizeof peer; SOCKET socket = accept(listener_, reinterpret_cast<sockaddr*>(&peer), &size);
            if (socket != INVALID_SOCKET) {
                DWORD timeoutMs = 1000; BOOL noDelay = TRUE;
                setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeoutMs), sizeof timeoutMs);
                setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof noDelay);
                std::lock_guard lock(mutex_);
                if (sessions_.size() >= 32) closesocket(socket);
                else {
                    auto client = std::make_unique<Client>(); client->socket = socket; client->id = nextClient_++;
                    char address[INET6_ADDRSTRLEN]{};
                    if (peer.ss_family == AF_INET6) inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr, address, sizeof address);
                    else if (peer.ss_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, address, sizeof address);
                    client->peer = address;
                    auto* ptr = client.get();
                    try { client->thread = std::thread([this, ptr] { serve(*ptr); }); sessions_.push_back(std::move(client)); }
                    catch (...) { closesocket(socket); }
                }
            }
        }
        std::lock_guard lock(mutex_);
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](auto& client) { if (!client->done) return false; if (client->thread.joinable()) client->thread.join(); return true; }), sessions_.end());
    }
}
void Server::serve(Client& client) {
    try {
        Stream stream(client.socket);
        if (!stream.negotiate(*credentials_)) {
            if (!stream.error().empty()) log_("HTTPS connection from " + client.peer + " failed: " + stream.error());
            throw std::runtime_error("HTTPS handshake failed");
        }
        std::string input; auto deadline = GetTickCount64() + 10000;
        while (input.find("\r\n\r\n") == std::string::npos) {
            if (stream.read(input, 500) < 0 || input.size() > 16384 || GetTickCount64() > deadline || !running_) throw std::runtime_error("Invalid HTTP headers");
        }
        auto end = input.find("\r\n\r\n"); std::istringstream header(input.substr(0, end));
        std::string line, method, path, version; std::getline(header, line); std::istringstream first(line); first >> method >> path >> version;
        std::map<std::string,std::string> headers;
        while (std::getline(header, line)) { auto colon = line.find(':'); if (colon == std::string::npos) throw std::runtime_error("Invalid HTTP header"); auto key = lower(trim(line.substr(0, colon))); if (headers.contains(key)) throw std::runtime_error("Duplicate HTTP header"); headers[key] = trim(line.substr(colon + 1)); }
        if (version != "HTTP/1.1" || !validHost(headers["host"]) || headers.contains("transfer-encoding")) throw std::runtime_error("Invalid HTTP request");
        if (method == "GET" && path == "/certificate.cer")
            stream.send(response(200,"application/pkix-cert",credentials_->certificate,"Content-Disposition: attachment; filename=ft710-web-root.cer\r\n"));
        else if (method == "GET" && path == "/station.mobileconfig")
            stream.send(response(200,"application/x-apple-aspen-config",credentials_->appleProfile,"Content-Disposition: attachment; filename=ft710-station.mobileconfig\r\n"));
        else if (method == "GET" && path == "/setup") stream.send(response(200,"text/html; charset=utf-8",setupPage(headers["host"],credentials_->fingerprint)));
        else if (!stream.secure()) {
            if (method == "GET" && (path == "/" || path == "/index.html")) stream.send(response(200,"text/html; charset=utf-8",setupPage(headers["host"],credentials_->fingerprint)));
            else if (method == "GET" && path == "/app.css") stream.send(response(200,"text/css; charset=utf-8",resource(202)));
            else stream.send(response(403,"application/json","{\"error\":\"Open the HTTPS dashboard after certificate setup\"}"));
        } else {
        std::string body = input.substr(end + 4); unsigned length = 0;
        if (headers.contains("content-length")) { std::size_t consumed = 0; auto number = std::stoul(headers["content-length"], &consumed); if (consumed != headers["content-length"].size() || number > 4096) throw std::runtime_error("Invalid body size"); length = static_cast<unsigned>(number); }
        while (body.size() < length) if (stream.read(body, 500) < 0 || GetTickCount64() > deadline || !running_) throw std::runtime_error("Incomplete HTTP body");
        bool origin = lower(headers["origin"]) == "https://" + lower(headers["host"]);
        if (method == "POST" && path == "/api/login" && origin && headers["content-type"].starts_with("application/x-www-form-urlencoded")) {
            bool limited = false; std::string token;
            const auto code = form(body.substr(0,length))["code"];
            {
                std::lock_guard lock(loginMutex_); auto now = GetTickCount64(); auto& previous = attempts_[client.peer]; limited = previous && now - previous < 1000; previous = now; if (attempts_.size() > 256) attempts_.clear();
                if (!limited && same(code,accessCode_)) {
                    std::erase_if(tokens_, [now](const auto& pair) { return pair.second < now; });
                    if (tokens_.size() >= 64) throw std::runtime_error("Too many logins");
                    token = randomToken(); tokens_[token] = now + 8 * 60 * 60 * 1000ULL;
                }
            }
            if (limited) stream.send(response(429, "application/json", "{\"error\":\"Wait a second before trying again\"}"));
            else if (token.empty()) stream.send(response(401, "application/json", "{\"error\":\"Incorrect station access code\"}"));
            else {
                stream.send(response(200, "application/json", "{\"ok\":true}", "Set-Cookie: ft710_tls_session=" + token + "; Path=/; Max-Age=28800; Secure; HttpOnly; SameSite=Strict\r\n"));
            }
        } else if (method == "POST" && path == "/api/logout" && origin) {
            { std::lock_guard lock(loginMutex_); tokens_.erase(cookieToken(headers["cookie"])); }
            stream.send(response(200, "application/json", "{\"ok\":true}", "Set-Cookie: ft710_tls_session=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict\r\n"));
        } else if (method == "GET" && path == "/ws" && origin && authenticated(headers["cookie"]) && lower(headers["upgrade"]) == "websocket" && lower(headers["connection"]).find("upgrade") != std::string::npos && headers["sec-websocket-version"] == "13") {
            DWORD decodedSize = 0; auto key = headers["sec-websocket-key"];
            if (key.size() != 24 || !CryptStringToBinaryA(key.c_str(), key.size(), CRYPT_STRING_BASE64, nullptr, &decodedSize, nullptr, nullptr) || decodedSize != 16 || clients_ >= 8) throw std::runtime_error("Invalid WebSocket handshake");
            if (stream.send("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + websocketAccept(key) + "\r\n\r\n")) websocket(client, stream, body, cookieToken(headers["cookie"]));
        } else if (method == "GET" && path == "/api/state") {
            if (!authenticated(headers["cookie"])) stream.send(response(401,"application/json","{\"error\":\"Sign in first\"}"));
            else { std::string state; { std::lock_guard lock(stateMutex_); state = state_; } stream.send(response(200,"application/json",state)); }
        } else if (method == "GET" && (path == "/" || path == "/index.html")) stream.send(response(200,"text/html; charset=utf-8",resource(201),"",headers["host"]));
        else if (method == "GET" && path == "/app.css") stream.send(response(200,"text/css; charset=utf-8",resource(202)));
        else if (method == "GET" && path == "/app.js") stream.send(response(200,"application/javascript; charset=utf-8",resource(203)));
        else if (method == "GET" && path == "/audio-worklet.js") stream.send(response(200,"application/javascript; charset=utf-8",resource(204)));
        else if (method == "GET" && path == "/audio-playback.js") stream.send(response(200,"application/javascript; charset=utf-8",resource(205)));
        else stream.send(response(404,"application/json","{\"error\":\"Not found or request not allowed\"}"));
        }
    } catch (...) { /* Malformed/unauthenticated peers never reach radio control. */ }
    { std::lock_guard lock(mutex_); closesocket(client.socket); client.socket = INVALID_SOCKET; }
    client.done = true;
}
void Server::websocket(Client& client, Stream& stream, std::string input, const std::string& token) {
    if (clients_.fetch_add(1) >= 8) { --clients_; frame(stream,8,std::string("\x03\xf0",2)); return; }
    struct OnClose { std::function<void()> action; ~OnClose() { try { action(); } catch (...) {} } } cleanup{[this,&client] {
        --clients_;
        auto gone = std::make_shared<Request>(); gone->client = client.id; gone->fields["action"] = "browserGone"; handler_(std::move(gone));
    }};
    std::vector<std::pair<std::string,std::future<std::string>>> pending;
    std::uint64_t revision = ~0ULL, urgentRevision = 0, spectrumRevision = 0, rfSpectrumRevision = 0, cursor = 0, epoch = 0;
    const auto accessRevision = accessRevision_.load();
    bool listening = false, alive = true; auto seen = GetTickCount64(), sentState = seen, sentSpectrum = seen, sentRfSpectrum = seen;
    unsigned commands = 0; auto commandPeriod = seen;
    frame(stream,1,"{\"type\":\"hello\",\"client\":" + std::to_string(client.id) + ",\"rate\":48000}");
    while (alive && running_) {
        auto now = GetTickCount64();
        if (now - seen > 15000 || accessRevision != accessRevision_.load() || !authenticated("ft710_tls_session=" + token)) break;
        int received = stream.read(input, 10); if (received < 0 || input.size() > 131072) break;
        while (input.size() >= 2 && alive) {
            auto a = static_cast<unsigned char>(input[0]), b = static_cast<unsigned char>(input[1]);
            unsigned opcode = a & 15; std::uint64_t length = b & 127; unsigned header = 2;
            if (!(a & 128) || (a & 112) || !(b & 128) || opcode == 0) { alive = false; break; }
            if (length == 126) { if (input.size() < 4) break; length = (static_cast<unsigned char>(input[2]) << 8) | static_cast<unsigned char>(input[3]); header = 4; }
            else if (length == 127) { if (input.size() < 10) break; length = 0; for (unsigned i = 2; i < 10; ++i) length = (length << 8) | static_cast<unsigned char>(input[i]); header = 10; }
            if (length > 16384 || ((opcode & 8) && length > 125)) { alive = false; break; }
            if (input.size() < header + 4 + length) break;
            std::string payload = input.substr(header + 4, static_cast<unsigned>(length));
            for (unsigned i = 0; i < length; ++i) payload[i] ^= input[header + (i & 3)];
            input.erase(0, header + 4 + length); seen = now;
            if (opcode == 8) { frame(stream,8,payload); alive = false; break; }
            if (opcode == 9) { alive = frame(stream,10,payload); continue; }
            if (opcode == 10) continue;
            if (opcode == 2) {
                if (payload.size() < 8) continue;
                std::uint32_t type, lease; std::memcpy(&type,payload.data(),4); std::memcpy(&lease,payload.data() + 4,4);
                if (type == 3) audio::browserAudio().microphone(client.id,lease,payload.data() + 8,static_cast<unsigned>(payload.size() - 8));
                continue;
            }
            if (opcode != 1) { alive = false; break; }
            try {
                auto fields = form(payload); auto action = fields["action"], seq = fields["seq"];
                if (action == "heartbeat") continue;
                if (now - commandPeriod >= 1000) { commandPeriod = now; commands = 0; }
                if (++commands > 40 || pending.size() >= 16) throw std::runtime_error("Too many pending commands");
                if (action == "listen") { listening = fields["enabled"] == "1"; cursor = 0; epoch = ~0ULL; frame(stream,1,"{\"type\":\"ack\",\"seq\":" + json(seq) + ",\"ok\":true}"); continue; }
                auto request = std::make_shared<Request>(); request->client = client.id; request->accessRevision = accessRevision; request->fields = std::move(fields);
                pending.emplace_back(seq,request->reply.get_future()); handler_(std::move(request));
            } catch (const std::exception& error) { frame(stream,1,"{\"type\":\"error\",\"error\":" + json(error.what()) + "}"); }
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto reply = it->second.get(); if (!frame(stream,1,"{\"type\":\"ack\",\"seq\":" + json(it->first) + "," + reply + "}")) alive = false;
                it = pending.erase(it);
            } else ++it;
        }
        std::string state, spectrum, rfSpectrum;
        { std::lock_guard lock(stateMutex_);
            if (revision != revision_ && (now - sentState >= 200 || urgentRevision < urgentRevision_)) {
                state = state_; revision = revision_; urgentRevision = urgentRevision_; sentState = now;
            }
            if (spectrumRevision != spectrumRevision_ && now - sentSpectrum >= 90) { if (spectrumTick_ && now - spectrumTick_ < 500) spectrum = spectrum_; spectrumRevision = spectrumRevision_; sentSpectrum = now; }
            if (rfSpectrumRevision != rfSpectrumRevision_ && now - sentRfSpectrum >= 90) { if (rfSpectrumTick_ && now - rfSpectrumTick_ < 500) rfSpectrum = rfSpectrum_; rfSpectrumRevision = rfSpectrumRevision_; sentRfSpectrum = now; }
        }
        if (!state.empty() && !frame(stream,1,state)) alive = false;
        if (!spectrum.empty() && !frame(stream,2,spectrum)) alive = false;
        if (!rfSpectrum.empty() && !frame(stream,2,rfSpectrum)) alive = false;
        if (listening) {
            std::vector<char> packet;
            for (unsigned i = 0; i < 3 && audio::browserAudio().readReceive(cursor,epoch,packet); ++i)
                if (!frame(stream,2,std::string(packet.begin(),packet.end()))) { alive = false; break; }
        }
    }
}
}
