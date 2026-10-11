#pragma once
#include "web_tls.hpp"
#include "audio_scope.hpp"
#include "rf_scope.hpp"
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace web {
std::string json(const std::string& value);
std::map<std::string, std::string> form(const std::string& text);
struct Request {
    std::uint64_t client = 0;
    std::uint64_t accessRevision = 0;
    std::map<std::string, std::string> fields;
    std::promise<std::string> reply;
};
class Server {
public:
    using Handler = std::function<void(std::shared_ptr<Request>)>;
    using Log = std::function<void(const std::string&)>;
    Server(Handler handler, Log log);
    ~Server();
    void start(const std::wstring& certificateFolder, const std::string& accessCode);
    void stop();
    void setAccessCode(const std::string& accessCode);
    std::uint64_t accessRevision() const { return accessRevision_.load(); }
    void publish(std::string state, bool urgent = false);
    void spectrum(const audio::SpectrumFrame& frame);
    void rfSpectrum(const rf_scope::Frame& frame, std::uint64_t centerHz, std::uint32_t spanHz);
    bool running() const { return running_; }
    unsigned clients() const { return clients_; }
    std::string fingerprint() const { return credentials_ ? credentials_->fingerprint : ""; }
private:
    struct Client {
        SOCKET socket = INVALID_SOCKET;
        std::uint64_t id = 0;
        std::string peer;
        std::thread thread;
        std::atomic<bool> done{false};
    };
    Handler handler_;
    Log log_;
    SOCKET listener_ = INVALID_SOCKET;
    bool winsock_ = false;
    std::atomic<bool> running_{false};
    std::atomic<unsigned> clients_{0};
    std::atomic<std::uint64_t> accessRevision_{0};
    std::thread acceptThread_;
    std::mutex mutex_, stateMutex_, loginMutex_;
    std::vector<std::unique_ptr<Client>> sessions_;
    std::map<std::string, ULONGLONG> tokens_, attempts_;
    std::string accessCode_, state_ = "{\"type\":\"state\"}", spectrum_, rfSpectrum_;
    std::unique_ptr<Credentials> credentials_;
    std::uint64_t revision_ = 0, urgentRevision_ = 0, spectrumRevision_ = 0, rfSpectrumRevision_ = 0, nextClient_ = 1;
    ULONGLONG spectrumTick_ = 0, rfSpectrumTick_ = 0;
    void acceptLoop();
    void serve(Client& client);
    bool authenticated(const std::string& cookie);
    void websocket(Client& client, Stream& stream, std::string input, const std::string& token);
};
}
