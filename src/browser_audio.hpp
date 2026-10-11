#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace audio {
inline constexpr std::wstring_view BrowserReceive = L"browser:playback";
inline constexpr std::wstring_view BrowserMicrophone = L"browser:capture";
// Fixed 48 kHz mono PCM wire format. Socket threads never touch audio drivers.
// Capture broadcasts through atomic slots; microphone render uses an SPSC ring.
class BrowserAudio {
public:
    void source(double rate);
    void receive(float sample) noexcept;
    void discontinuity() { receiveReset_.store(true); receiveEpoch_.fetch_add(2, std::memory_order_release); }
    bool readReceive(std::uint64_t& cursor, std::uint64_t& epoch, std::vector<char>& packet) const;
    void microphoneOwner(std::uint64_t owner, bool enabled);
    bool microphone(std::uint64_t client, std::uint32_t lease, const char* pcm, unsigned bytes);
    unsigned pullMicrophone(float* samples, unsigned count) noexcept;
    std::uint64_t owner() const { return owner_.load(); }
    std::uint32_t lease() const { return static_cast<std::uint32_t>(micEpoch_.load()); }
private:
    static constexpr std::uint64_t Capacity = 32768;
    std::array<std::atomic<float>, Capacity> receive_{};
    std::atomic<std::uint64_t> receiveWrite_{0}, receiveEpoch_{0};
    std::atomic<bool> receiveReset_{false};
    double rate_ = 48000, next_ = 0;
    std::uint64_t input_ = 0;
    float previous_ = 0;
    bool havePrevious_ = false;
    void output(float sample) noexcept;
    std::array<float, Capacity> microphone_{};
    std::atomic<std::uint64_t> micWrite_{0}, micRead_{0}, owner_{0}, micEpoch_{0};
    std::atomic<bool> micEnabled_{false};
    std::uint64_t renderEpoch_ = 0;
    std::mutex producerMutex_;
};
BrowserAudio& browserAudio();
}
