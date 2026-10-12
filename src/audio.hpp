#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "audio_scope.hpp"
#include "browser_audio.hpp"

namespace audio {
struct Device { std::wstring id, name; bool input = false, direct = false, remote = false; };
std::vector<Device> devices();
std::vector<Device> directDevices();
std::wstring directDeviceIssue();
bool remoteSession();
std::wstring savedDeviceName(const std::wstring& identity, bool input);
struct Status {
    bool active = false, starting = false;
    float inputPeak = 0, outputPeak = 0;
    std::wstring text = L"Off";
};
// One explicit capture -> render route. No implicit default endpoint changes.
class Route {
public:
    Route();
    ~Route();
    void start(const std::wstring& source, const std::wstring& destination);
    void stop();
    void requestStop();
    void gate(bool enabled);
    void gain(float value);
    Status status() const;
    Spectrum& spectrum() { return spectrum_; }
    void receivedAudio(bool enabled) { receiveTap_ = enabled; spectrum_.enable(enabled); }
private:
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> gate_{false};
    std::atomic<unsigned> gateEpoch_{0};
    std::atomic<float> gain_{1};
    mutable std::mutex mutex_;
    Status status_;
    Spectrum spectrum_;
    bool receiveTap_ = false; // Set before starting streams.
    void run(std::wstring source, std::wstring destination);
    void runDirect(std::wstring source, std::wstring destination);
};
}
