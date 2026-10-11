#pragma once
#include <winsock2.h>
#include "cat.hpp"
#include <functional>
#include <mutex>
#include <thread>

// A loopback rigctld protocol server. It never opens a radio or calls Hamlib.
class CatServer {
public:
    using Handler = std::function<std::future<radio::CatReply>(std::shared_ptr<radio::CatJob>)>;
    using Log = std::function<void(const std::string&)>;
    CatServer(Handler handler, Log log);
    ~CatServer();
    void start(unsigned port, std::uint64_t generation);
    void stop();
    bool running() const { return running_.load(); }
    unsigned clients() const { return clients_.load(); }
    unsigned port() const { return port_; }
private:
    struct Client {
        SOCKET socket = INVALID_SOCKET;
        std::uint64_t id = 0;
        std::thread thread;
        std::atomic<bool> done{false};
    };
    Handler handler_;
    Log log_;
    SOCKET listener_ = INVALID_SOCKET;
    bool winsock_ = false;
    std::atomic<bool> running_{false};
    std::atomic<unsigned> clients_{0};
    unsigned port_ = 0;
    std::uint64_t generation_ = 0, nextClient_ = 1;
    std::thread acceptThread_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Client>> sessions_;
    void acceptLoop();
    void serve(Client& client);
};
