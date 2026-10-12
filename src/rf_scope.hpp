#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace rf_scope {

inline constexpr std::size_t FrameBytes = 4096;
inline constexpr std::size_t BinCount = 850;

struct Frame {
    std::array<std::uint8_t, BinCount> bins{};
    std::uint64_t centerHz = 0;
    std::uint64_t sequence = 0;
    std::uint32_t spanIndex = 0;
    bool frequencyValid = false;
    bool valid = false;
    ULONGLONG acquiredAt = 0;
    std::uint64_t generation = 0;
    bool fresh() const { return valid && acquiredAt && GetTickCount64() - acquiredAt < 1000; }
};

class Capture {
public:
    Capture() = default;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool start();
    void stop();
    void requestStop() { stopRequested_ = true; }
    bool latest(Frame& frame) const;
    std::wstring status() const;
    bool running() const { return running_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> stopRequested_{false}, running_{false};
    mutable std::mutex mutex_;
    std::thread thread_;
    Frame latest_{};
    std::wstring status_ = L"RF scope stopped";
    std::uint64_t sequence_ = 0;
    std::uint64_t generation_ = 0;
    void run();
    void setStatus(std::wstring status);
};

// Applies radio RF-scope display span and CENTER mode through the FT-710's
// second (Standard/CAT-2) virtual COM port. It opens the port only for this
// explicit operation; it never opens the CAT-1 port owned by the dashboard.
std::wstring applySpan(const std::wstring& cat2Port, unsigned spanIndex, unsigned baudRate = 4800, double waterfallLevelDb = -5.0);

std::uint32_t spanHz(unsigned spanIndex);
std::wstring spanLabel(unsigned spanIndex);

} // namespace rf_scope
