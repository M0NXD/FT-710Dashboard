#pragma once
#include <array>
#include <atomic>
#include <complex>
#include <cstdint>

namespace audio {
struct SpectrumFrame {
    static constexpr unsigned Size = 4096, Bins = Size / 2 + 1;
    std::array<float, Bins> db{};
    double sampleRate = 0;
    std::uint64_t generation = 0;
    bool valid = false;
};

// Single capture producer, single GUI consumer. Capture only copies samples;
// FFT/windowing and all display work run on the GUI thread.
class Spectrum {
public:
    void enable(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void configure(double sampleRate);
    void discontinuity() { gap_.store(true, std::memory_order_release); }
    void push(float sample) noexcept;
    bool poll(SpectrumFrame& frame);
    void discard(); // Consumer thread only; producer counters are never reset.
private:
    static constexpr std::uint64_t Capacity = 32768;
    std::array<float, Capacity> queue_{};
    std::atomic<std::uint64_t> written_{0}, read_{0}, generation_{0};
    std::atomic<double> rate_{0};
    std::atomic<bool> enabled_{false}, gap_{false};
    std::array<float, SpectrumFrame::Size> samples_{};
    std::array<std::complex<float>, SpectrumFrame::Size> fft_{};
    unsigned position_ = 0, count_ = 0, sinceFrame_ = 0;
    std::uint64_t consumerGeneration_ = 0;
    void transform(SpectrumFrame& frame);
};
}
