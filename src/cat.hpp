#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace radio {
struct CatRequest {
    std::string name;
    std::vector<std::string> arguments;
    std::string vfo = "currVFO";
    std::uint64_t client = 0, generation = 0;
    bool modern = false;
};
struct CatReply {
    int result = 0;
    bool changed = false;
    std::string raw;
    std::vector<std::pair<std::string, std::string>> values;
};
struct CatJob {
    CatRequest request;
    std::promise<CatReply> reply;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> started{false};
    std::chrono::steady_clock::time_point queueDeadline = std::chrono::steady_clock::time_point::max();
};
inline bool releasesPtt(const CatRequest& request) {
    return request.name == "__release_client" ||
        (request.name == "set_ptt" && request.arguments.size() == 1 && request.arguments[0] == "0");
}
}
