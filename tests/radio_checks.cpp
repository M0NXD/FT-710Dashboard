#include <winsock2.h>
#include <ws2tcpip.h>
#include "radio.hpp"
#include "cat_server.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace std::chrono_literals;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void check(int result, const std::string& message) {
    require(result == RIG_OK, message + ": " + rigerror(result));
}
void expectClose(double value, double expected, const std::string& message) {
    require(std::abs(value - expected) < .01, message + " got " + std::to_string(value));
}
bool sendAll(SOCKET socket, const std::string& text) {
    std::size_t count = 0;
    while (count < text.size()) {
        int result = send(socket, text.data() + count, static_cast<int>(text.size() - count), 0);
        if (result <= 0) return false;
        count += result;
    }
    return true;
}

// This endpoint implements only the documented CAT fields used by these
// checks. It is a loopback fixture, not a radio connection or a meter source
// used by the production application.
class Ft710Fixture {
public:
    std::atomic<bool> transmitting{true};
    std::atomic<int> power{50}, vfoWrites{0}, meterReads{0}, filterWrites{0};
    std::atomic<int> frequencyA{14200000}, frequencyB{7074000};
    std::atomic<int> activeVfo{0}, split{0};
    std::atomic<char> mode{'2'};
    unsigned port = 0;
    Ft710Fixture() {
        WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "Fixture bind");
        int size = sizeof address; getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
        require(listen(listener_, 1) == 0, "Fixture listen");
        thread_ = std::thread([this] { serve(); });
    }
    ~Ft710Fixture() {
        stopping_ = true;
        { std::lock_guard lock(mutex_); if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH); }
        closesocket(listener_);
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }
    bool saw(const std::string& command) {
        std::lock_guard lock(mutex_);
        return std::find(commands_.begin(), commands_.end(), command) != commands_.end();
    }
    std::size_t commandCount() { std::lock_guard lock(mutex_); return commands_.size(); }
private:
    std::atomic<bool> stopping_{false};
    SOCKET listener_ = INVALID_SOCKET, socket_ = INVALID_SOCKET;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> commands_;
    std::string answer(const std::string& command) {
        { std::lock_guard lock(mutex_); commands_.push_back(command); }
        if (command == "ID") return "ID0800;";
        if (command == "AI") return "AI0;";
        if (command == "PS") return "PS1;";
        if (command == "FA" || command == "FB") {
            char value[24]{};
            std::snprintf(value, sizeof value, "%s%09d;", command.c_str(), command == "FA" ? frequencyA.load() : frequencyB.load());
            return value;
        }
        if (command.size() == 11 && (command.starts_with("FA") || command.starts_with("FB"))) {
            (command.starts_with("FA") ? frequencyA : frequencyB) = std::stoi(command.substr(2)); return {};
        }
        if (command == "IF" || command == "OI") {
            char value[48]{};
            std::snprintf(value, sizeof value, "%s001%09d+000000%c00000;", command.c_str(),
                command == "IF" ? frequencyA.load() : frequencyB.load(), mode.load());
            return value;
        }
        if (command == "VS") return "VS" + std::to_string(activeVfo.load()) + ";";
        if (command.size() == 3 && command.starts_with("VS")) { ++vfoWrites; activeVfo = command[2] - '0'; return {}; }
        if (command == "TX") return transmitting ? "TX2;" : "TX0;";
        if (command.starts_with("TX")) { transmitting = command != "TX0"; return {}; }
        if (command == "PC") {
            char value[16]{}; std::snprintf(value, sizeof value, "PC%03d;", power.load()); return value;
        }
        if (command.starts_with("PC") && command.size() == 5) { power = std::stoi(command.substr(2)); return {}; }
        if (command == "MD0" || command == "MD1") return command + mode.load() + ";";
        if (command.size() == 4 && (command.starts_with("MD0") || command.starts_with("MD1"))) {
            mode = command[3]; return {};
        }
        if (command == "SH0" || command == "SH1") return command + "013;";
        if (command == "NA0" || command == "NA1") return command + "0;";
        if (command.size() > 3 && (command.starts_with("SH") || command.starts_with("NA"))) { ++filterWrites; return {}; }
        if (command == "SM0" || command == "SM1") return command + "130;";
        if (command == "RM3") { ++meterReads; return "RM3135000;"; } // 10 dB compression
        if (command == "RM4") { ++meterReads; return "RM4032000;"; } // Hamlib ALC 0.5
        if (command == "RM5") { ++meterReads; return "RM5147000;"; } // calibrated 50 W
        if (command == "RM6") { ++meterReads; return "RM6052000;"; } // SWR 1.5
        if (command == "RM7") { ++meterReads; return "RM7053000;"; } // 5 A
        if (command == "RM8") { ++meterReads; return "RM8192000;"; } // 13.8 V
        if (command == "ST") return "ST" + std::to_string(split.load()) + ";";
        if (command.size() == 3 && command.starts_with("ST")) { split = command[2] - '0'; return {}; }
        if (command == "FT") return "FT" + std::to_string(split ? 1 - activeVfo.load() : activeVfo.load()) + ";";
        if (command.size() == 3 && command.starts_with("FT")) { split = command[2] - '0' != activeVfo; return {}; }
        if (command == "MG") return "MG050;";
        if (command == "AG0" || command == "AG1" || command == "RG0" || command == "RG1") return command + "128;";
        if (command == "SQ0" || command == "SQ1") return command + "000;";
        if (command == "IS0" || command == "IS1") return command + "+0000;";
        if (command == "GT0" || command == "GT1") return command + "4;";
        if (command == "PA0" || command == "RA0" || command == "NB0" || command == "NR0" || command == "BC0" || command == "AC") return command + "0;";
        return "?;";
    }
    void serve() {
        SOCKET peer = accept(listener_, nullptr, nullptr);
        if (peer == INVALID_SOCKET) return;
        { std::lock_guard lock(mutex_); socket_ = peer; }
        BOOL noDelay = TRUE; setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof noDelay);
        std::string buffer;
        while (!stopping_) {
            char text[2048]; int size = recv(peer, text, sizeof text, 0); if (size <= 0) break;
            buffer.append(text, size);
            while (true) {
                auto end = buffer.find(';'); if (end == std::string::npos) break;
                auto command = buffer.substr(0, end); buffer.erase(0, end + 1);
                auto reply = answer(command); if (!reply.empty() && !sendAll(peer, reply)) break;
            }
        }
        { std::lock_guard lock(mutex_); closesocket(peer); socket_ = INVALID_SOCKET; }
    }
};

class SharedClient {
public:
    RIG* rig = nullptr;
    explicit SharedClient(unsigned port) {
        rig = rig_init(RIG_MODEL_NETRIGCTL); require(rig != nullptr, "NET rigctl init");
        auto set = [&](const char* key, const std::string& value) { check(rig_set_conf(rig, rig_token_lookup(rig, key), value.c_str()), key); };
        set("rig_pathname", "127.0.0.1:" + std::to_string(port)); set("timeout", "3000"); set("retry", "0");
        check(rig_open(rig), "NET rigctl open / capability negotiation");
    }
    ~SharedClient() { if (rig) { rig_close(rig); rig_cleanup(rig); } }
    double level(setting_t mask, vfo_t vfo = RIG_VFO_CURR) {
        value_t value{}; check(rig_get_level(rig, vfo, mask, &value), std::string("Get ") + rig_strlevel(mask)); return value.f;
    }
};

// Send the wire request directly so Hamlib's client-side caches cannot skip it.
class CatWireClient {
public:
    explicit CatWireClient(unsigned port) {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket_ != INVALID_SOCKET, "CAT wire socket");
        DWORD timeout = 5000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof timeout);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u_short>(port));
        if (connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
            closesocket(socket_); throw std::runtime_error("CAT wire connect");
        }
    }
    ~CatWireClient() { closesocket(socket_); }
    std::string command(const std::string& text) {
        require(sendAll(socket_, text + "\n"), "CAT wire send");
        std::string line;
        for (char ch; line.size() < 4096;) {
            require(recv(socket_, &ch, 1, 0) == 1, "CAT wire reply");
            if (ch == '\n') return line;
            line += ch;
        }
        throw std::runtime_error("CAT wire reply too long");
    }
private:
    SOCKET socket_ = INVALID_SOCKET;
};

int main() {
    try {
        Ft710Fixture fixture;
        std::mutex mutex;
        radio::Snapshot state;
        std::vector<std::string> logs;
        auto current = [&] { std::lock_guard lock(mutex); return state; };
        radio::Worker worker([&](const auto& update) { std::lock_guard lock(mutex); state = update; },
            [&](const auto& text) { std::lock_guard lock(mutex); logs.push_back(text); });
        auto wait = [&](auto condition, const std::string& message) {
            auto deadline = std::chrono::steady_clock::now() + 12s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (condition(current())) return;
                std::this_thread::sleep_for(25ms);
            }
            auto snapshot = current(); throw std::runtime_error(message + " / " + snapshot.connectionStatus + " / " + snapshot.error);
        };
        radio::Command connect; connect.action = radio::Action::Connect; connect.connection.type = radio::ConnectionType::Serial;
        connect.connection.endpoint = "127.0.0.1:" + std::to_string(fixture.port); connect.connection.pollMs = 2000;
        worker.submit(connect);
        wait([](const auto& s) { auto meter = radio::findSetting(s, "level.RFPOWER_METER_WATTS"); return s.online && meter && meter->valid; }, "FT-710 worker did not publish TX meters");
        std::cout << "PASS actual Hamlib FT-710 driver connects to loopback CAT fixture\n";
        auto snapshot = current();
        for (auto [id, expected] : std::vector<std::pair<std::string, double>>{{"RFPOWER_METER_WATTS",50},{"ALC",.5},{"SWR",1.5},{"COMP_METER",10},{"ID_METER",5},{"VD_METER",13.8}}) {
            auto s = radio::findSetting(snapshot, "level." + id); require(s && s->valid, id + " missing/invalid"); expectClose(std::stod(s->value), expected, id);
        }
        std::cout << "PASS RF=50 W, ALC=0.5, SWR=1.5, compression=10 dB, current=5 A, voltage=13.8 V\n";
        auto previous = radio::findSetting(snapshot, "level.ALC")->updated;
        wait([&](const auto& s) { auto alc = radio::findSetting(s, "level.ALC"); return alc && alc->valid && alc->updated > previous; }, "Independent meter refresh");
        require(radio::findSetting(current(), "level.ALC")->updated - previous < 1500ms, "Meter update waited for 2 s full scan");
        std::cout << "PASS independent TX meter refresh before the 2 s full polling interval\n";
        radio::Command power; power.action = radio::Action::Set; power.id = "level.RFPOWER"; power.text = "0.25"; power.generation = snapshot.generation;
        worker.submit(power);
        wait([](const auto& s) { auto p = radio::findSetting(s, "level.RFPOWER"); return p && p->valid && std::abs(std::stod(p->value) - .25) < .001; }, "Power write/readback");
        require(fixture.saw("PC025"), "25 W did not send PC025;");
        std::cout << "PASS GUI worker power change sends PC025; and reads PC; back\n";
        CatServer server([&](auto job) { return worker.request(std::move(job)); }, [&](const auto&) {});
        unsigned sharedPort = 0;
        for (unsigned port = 46520; port < 46540; ++port) {
            try { server.start(port, snapshot.generation); sharedPort = port; break; } catch (...) {}
        }
        require(sharedPort != 0, "Could not start sharing test port");
        {
            SharedClient first(sharedPort), second(sharedPort);
            require(first.rig->caps->timeout >= 5000, "Sharing timeout does not allow for queued serial work");
            auto a = std::async(std::launch::async, [&] { return first.level(RIG_LEVEL_ALC, RIG_VFO_B); });
            auto b = std::async(std::launch::async, [&] { return second.level(RIG_LEVEL_RFPOWER_METER_WATTS, RIG_VFO_B); });
            expectClose(a.get(), .5, "Shared ALC"); expectClose(b.get(), 50, "Shared RF watts");
            require(fixture.vfoWrites == 0, "A global meter query switched VFO");
            value_t value{}; value.f = .75f;
            check(rig_set_level(first.rig, RIG_VFO_B, RIG_LEVEL_RFPOWER, value), "Shared power set");
            expectClose(second.level(RIG_LEVEL_RFPOWER), .75, "Second client power readback");
            expectClose(first.level(RIG_LEVEL_RFPOWER_METER_WATTS), 50, "Configured 75 W must not replace measured 50 W");
            require(fixture.saw("PC075"), "Shared power control did not send PC075;");
            unsigned int mw{}; check(rig_power2mW(first.rig, &mw, .5f, 14200000, RIG_MODE_USB), "Correct FT-710 power metadata");
            require(mw == 50000, "Power metadata still advertises 200 W");
            value.f = 1.01f; require(rig_set_level(first.rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, value) != RIG_OK, "Accepted RF power above 100 W");
            value.f = .04f; require(rig_set_level(first.rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, value) != RIG_OK, "Accepted RF power below 5 W");
            std::cout << "PASS two genuine Hamlib NET clients read meters, set/read power, preserve VFO and distinguish measured watts from the setting\n";
            fixture.mode = '5'; fixture.power = 25;
            wait([](const auto& s) { return s.a.modeValid && s.a.mode == "AM"; }, "AM mode not polled");
            value.f = .5f; require(rig_set_level(first.rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, value) != RIG_OK, "Accepted 50 W in AM");
            require(fixture.power == 25, "Invalid AM power altered PC setting");
            value.f = .25f; check(rig_set_level(first.rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, value), "AM 25 W set");
            std::cout << "PASS AM power accepts 25 W and rejects 50 W before writing CAT\n";
        }
        server.stop();
        fixture.transmitting = false;
        wait([](const auto& s) { auto alc = radio::findSetting(s, "level.ALC"); auto rf = radio::findSetting(s, "level.RFPOWER_METER_WATTS");
            return s.pttKnown && !s.transmitting && alc && !alc->valid && rf && !rf->valid; }, "TX readings did not clear on RX");
        std::cout << "PASS TX meters clear on RX; no latched readings are shown as live\n";
        server.start(sharedPort, snapshot.generation);
        {
            CatWireClient client(sharedPort);
            require(client.command("\\chk_vfo") == "1", "WSJT-X VFO negotiation");
            require(client.command("T VFOA 0") == "RPRT 0", "WSJT-X initial PTT release");
            // An opening NET client sends a sequence of capability queries.
            // These must not each wait behind another background serial read.
            require(client.command("\\get_modes").find("PKTUSB") != std::string::npos, "Startup mode capabilities");
            const auto burstStart = std::chrono::steady_clock::now();
            for (int i = 0; i < 32; ++i)
                require(client.command("\\get_modes").find("PKTUSB") != std::string::npos, "Startup capability burst");
            const auto burstMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - burstStart).count();
            std::cout << "CAT startup burst: 32 capability replies in " << burstMs << " ms\n";
            require(burstMs < 1000, "Startup queries blocked behind background polling");
            const auto serialBefore = fixture.commandCount();
            const auto trafficDeadline = std::chrono::steady_clock::now() + 1200ms;
            while (std::chrono::steady_clock::now() < trafficDeadline)
                require(client.command("\\get_modes").find("PKTUSB") != std::string::npos, "Sustained capability traffic");
            require(fixture.commandCount() > serialBefore, "CAT burst starved background radio reads");
            std::cout << "PASS background radio polling progresses during sustained CAT traffic\n";
            // WS/WSJT-X sends this exact request when entering data mode:
            // M VFOA PKTUSB -1. The sentinel means retain the radio's filter.
            require(client.command("M VFOA PKTUSB -1") == "RPRT 0", "WSJT-X data mode with unchanged filter");
            wait([&](const auto&) { return fixture.saw("MD0C"); }, "Data mode was not sent to the FT-710 backend");
            require(fixture.mode == 'C', "Data mode not applied");
            const auto splitReply = client.command("X VFOA PKTUSB -1");
            require(splitReply == "RPRT 0", "Split mode with unchanged filter: " + splitReply);
            const auto combinedReply = client.command("K VFOA 14075000 PKTUSB -1");
            require(combinedReply == "RPRT 0", "Combined split mode with unchanged filter: " + combinedReply);
            require(client.command("M VFOA PKTUSB -2") == "RPRT -1", "Accepted invalid negative passband");
            require(client.command("X VFOA PKTUSB -2") == "RPRT -1", "Accepted invalid split passband");
            require(client.command("K VFOA 14075000 PKTUSB -2") == "RPRT -1", "Accepted invalid combined split passband");
            require(fixture.filterWrites == 0, "Unchanged-filter request changed the radio's filter");
            std::cout << "PASS WSJT-X data mode and split requests retain the filter; invalid passbands still rejected\n";
        }
        server.stop();
        worker.stop();
        std::cout << "ALL OFFLINE CHECKS PASSED (no physical radio or RF transmission)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
