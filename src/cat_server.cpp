#include "cat_server.hpp"
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
bool sendAll(SOCKET socket, const std::string& message) {
    std::size_t sent = 0;
    while (sent < message.size()) {
        int n = send(socket, message.data() + sent, static_cast<int>(message.size() - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}
std::string canonical(std::string name) {
    static const std::map<std::string, std::string> aliases{
        {"f","get_freq"},{"F","set_freq"},{"m","get_mode"},{"M","set_mode"},
        {"v","get_vfo"},{"V","set_vfo"},{"t","get_ptt"},{"T","set_ptt"},
        {"i","get_split_freq"},{"I","set_split_freq"},{"x","get_split_mode"},{"X","set_split_mode"},
        {"k","get_split_freq_mode"},{"K","set_split_freq_mode"},{"s","get_split_vfo"},{"S","set_split_vfo"},
        {"l","get_level"},{"L","set_level"},{"u","get_func"},{"U","set_func"},
        {"p","get_parm"},{"P","set_parm"},{"j","get_rit"},{"J","set_rit"},
        {"z","get_xit"},{"Z","set_xit"},{"n","get_ts"},{"N","set_ts"},
        {"e","get_mem"},{"E","set_mem"},{"B","set_bank"},{"G","vfo_op"},
        {"r","get_rptr_shift"},{"R","set_rptr_shift"},{"o","get_rptr_offs"},{"O","set_rptr_offs"},
        {"c","get_ctcss_tone"},{"C","set_ctcss_tone"},{"d","get_dcs_code"},{"D","set_dcs_code"},
        {"y","get_ant"},{"Y","set_ant"},{"_","get_info"},{"a","get_trn"},{"A","set_trn"},
        {"2","power2mW"},{"4","mW2power"},{"1","dump_caps"},{"b","send_morse"},
        {"q","quit"},{"Q","quit"}
    };
    if (!name.empty() && name.front() == '\\') return name.substr(1);
    auto it = aliases.find(name);
    return it == aliases.end() ? name : it->second;
}
bool hasVfo(const std::string& name) {
    return name == "get_freq" || name == "set_freq" || name == "get_mode" || name == "set_mode" ||
        name == "get_ptt" || name == "set_ptt" || name == "vfo_op" || name == "set_bank" ||
        name == "get_ant" || name == "set_ant" || name.starts_with("get_split_") || name.starts_with("set_split_") ||
        name.starts_with("get_level") || name.starts_with("set_level") || name.starts_with("get_func") || name.starts_with("set_func") ||
        name.ends_with("_rit") || name.ends_with("_xit") || name.ends_with("_ts") || name.ends_with("_mem") ||
        name.find("_rptr_") != std::string::npos || name.find("_ctcss_") != std::string::npos || name.find("_dcs_") != std::string::npos;
}
std::string response(const radio::CatRequest& request, const radio::CatReply& reply, bool extended, char separator, bool vfoMode) {
    std::string text;
    if (request.name == "chk_vfo") separator = '\n';
    if (extended && request.name != "chk_vfo") {
        text = request.name + ":";
        if (vfoMode && hasVfo(request.name)) text += " " + request.vfo;
        for (std::size_t i = 0; i < request.arguments.size(); ++i)
            text += (i == 0 && vfoMode && hasVfo(request.name) ? ":" : " ") + request.arguments[i];
        text += separator;
    }
    if (reply.result == 0) {
        text += reply.raw;
        for (const auto& [label, value] : reply.values) {
            if (extended && request.name != "chk_vfo") text += label + ": ";
            text += value;
            text += separator;
        }
    }
    bool terminator = request.name == "client_version" || request.name == "hamlib_version" ||
        request.name == "dump_caps" || request.name == "get_mode_bandwidths";
    if (reply.result != 0 || (request.name != "chk_vfo" && (extended || terminator || (reply.values.empty() && reply.raw.empty()))))
        text += "RPRT " + std::to_string(reply.result) + "\n";
    if (text.empty() || text.back() != '\n') text += '\n';
    return text;
}
}

CatServer::CatServer(Handler handler, Log log) : handler_(std::move(handler)), log_(std::move(log)) {}
CatServer::~CatServer() { stop(); }
void CatServer::start(unsigned port, std::uint64_t generation) {
    stop();
    if (!port || port > 65535) throw std::invalid_argument("CAT TCP port must be 1 to 65535.");
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Could not initialize Windows networking.");
    winsock_ = true;
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) == SOCKET_ERROR ||
        listen(listener_, 16) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        stop();
        throw std::runtime_error("Cannot share CAT on 127.0.0.1:" + std::to_string(port) +
            (error == WSAEADDRINUSE || error == WSAEACCES ? ". Port is in use; stop the process using it or choose another port." : ". Winsock error " + std::to_string(error)));
    }
    port_ = port;
    generation_ = generation;
    sessions_.reserve(16);
    running_ = true;
    try { acceptThread_ = std::thread([this] { acceptLoop(); }); }
    catch (...) { stop(); throw; }
    log_("CAT sharing listening on 127.0.0.1:" + std::to_string(port) + " (Hamlib / NET rigctl). Multiple clients can connect.");
}
void CatServer::requestStop() {
    running_ = false;
    std::lock_guard lock(mutex_);
    for (auto& client : sessions_) if (client->socket != INVALID_SOCKET) shutdown(client->socket, SD_BOTH);
}
void CatServer::stop() {
    bool wasRunning = winsock_;
    requestStop();
    if (acceptThread_.joinable()) acceptThread_.join();
    if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
    {
        std::lock_guard lock(mutex_);
        for (auto& client : sessions_) if (client->socket != INVALID_SOCKET) shutdown(client->socket, SD_BOTH);
    }
    for (auto& client : sessions_) if (client->thread.joinable()) client->thread.join();
    sessions_.clear();
    clients_ = 0;
    if (winsock_) { WSACleanup(); winsock_ = false; }
    if (wasRunning) log_("CAT sharing stopped; clients disconnected.");
}
void CatServer::acceptLoop() {
    while (running_) {
        fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable);
        timeval timeout{0, 250000};
        int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready > 0 && running_) {
            SOCKET socket = accept(listener_, nullptr, nullptr);
            if (socket != INVALID_SOCKET) {
                DWORD sendTimeout = 2000, receiveTimeout = 500;
                setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&sendTimeout), sizeof sendTimeout);
                setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&receiveTimeout), sizeof receiveTimeout);
                BOOL noDelay = TRUE;
                setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof noDelay);
                std::lock_guard lock(mutex_);
                if (!running_) closesocket(socket);
                else if (clients_ >= 16) { sendAll(socket, "RPRT -21\n"); closesocket(socket); }
                else {
                    auto client = std::make_unique<Client>();
                    client->socket = socket; client->id = nextClient_++;
                    ++clients_;
                    Client* ptr = client.get();
                    try { client->thread = std::thread([this, ptr] { serve(*ptr); }); sessions_.push_back(std::move(client)); }
                    catch (...) { --clients_; closesocket(socket); }
                }
            }
        }
        std::lock_guard lock(mutex_);
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](auto& client) {
            if (!client->done) return false;
            if (client->thread.joinable()) client->thread.join();
            return true;
        }), sessions_.end());
    }
}
void CatServer::serve(Client& client) {
    log_("CAT client " + std::to_string(client.id) + " connected.");
    bool vfoMode = false, modern = false, connected = true;
    std::string buffer;
    while (running_ && connected) {
        char data[1024];
        int n = recv(client.socket, data, sizeof data, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) continue;
        if (n <= 0) break;
        buffer.append(data, n);
        while (connected && running_) {
            auto end = buffer.find('\n');
            if (end == std::string::npos) break;
            if (end > 4096) { sendAll(client.socket, "RPRT -8\n"); connected = false; break; }
            std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#') continue;
            line.erase(0, start);
            bool extended = line.front() == '+' || line.front() == ';' || line.front() == '|' || line.front() == ',';
            char separator = extended && line.front() != '+' ? line.front() : '\n';
            if (extended) line.erase(0, 1);
            std::istringstream input(line);
            std::string name, arg;
            input >> name;
            radio::CatRequest request;
            request.name = canonical(name); request.client = client.id; request.generation = generation_; request.modern = modern;
            while (input >> arg) request.arguments.push_back(arg);
            if (request.name == "quit") { connected = false; break; }
            radio::CatReply reply;
            if (request.name == "chk_vfo") {
                // Hamlib clients negotiate explicit VFO addressing, which
                // avoids one client's VFO selection redirecting another's
                // reads/writes. Plain text clients retain their initial mode.
                if (!modern) vfoMode = true;
                modern = true;
                reply.values.emplace_back("ChkVFO", vfoMode ? "1" : "0");
            } else if (request.name == "set_vfo_opt") {
                if (request.arguments.size() != 1 || (request.arguments[0] != "0" && request.arguments[0] != "1")) reply.result = -1;
                else vfoMode = request.arguments[0] == "1";
            } else {
                if (vfoMode && hasVfo(request.name)) {
                    if (request.arguments.empty()) reply.result = -1;
                    else {
                        auto value = request.arguments.front();
                        request.arguments.erase(request.arguments.begin());
                        auto colon = value.find(':');
                        request.vfo = value.substr(0, colon);
                        if (colon != std::string::npos) request.arguments.insert(request.arguments.begin(), value.substr(colon + 1));
                    }
                }
                if (!reply.result) {
                    auto job = std::make_shared<radio::CatJob>(); job->request = request;
                    try {
                        auto future = handler_(job);
                        auto deadline = std::chrono::steady_clock::now() + 45s;
                        while (running_ && std::chrono::steady_clock::now() < deadline && future.wait_for(50ms) != std::future_status::ready) {}
                        if (!running_ || future.wait_for(0ms) != std::future_status::ready) {
                            job->cancelled = true; reply.result = -5; connected = false;
                        } else reply = future.get();
                    } catch (...) { reply.result = -7; connected = false; }
                }
            }
            if (!sendAll(client.socket, response(request, reply, extended, separator, vfoMode))) connected = false;
        }
        if (buffer.size() > 4096) { sendAll(client.socket, "RPRT -8\n"); break; }
    }
    // The worker releases CAT PTT only when this client still owns it.
    auto release = std::make_shared<radio::CatJob>();
    release->request.name = "__release_client"; release->request.client = client.id; release->request.generation = generation_;
    try { handler_(release); } catch (...) {}
    {
        std::lock_guard lock(mutex_);
        closesocket(client.socket); client.socket = INVALID_SOCKET;
    }
    --clients_;
    log_("CAT client " + std::to_string(client.id) + " disconnected.");
    client.done = true;
}
