#include "audio_scope.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace audio {
void Spectrum::configure(double sampleRate) {
    // Called before starting capture; the GUI discards the previous source.
    generation_.fetch_add(1, std::memory_order_acq_rel); // Odd while changing format.
    rate_.store(sampleRate, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);
}
void Spectrum::push(float sample) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    auto write = written_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) >= Capacity) {
        gap_.store(true, std::memory_order_release);
        return; // Never overwrite samples while the consumer is reading them.
    }
    queue_[write & (Capacity - 1)] = std::isfinite(sample) ? sample : 0;
    written_.store(write + 1, std::memory_order_release);
}
void Spectrum::discard() {
    read_.store(written_.load(std::memory_order_acquire), std::memory_order_release);
    position_ = count_ = sinceFrame_ = 0;
}
bool Spectrum::poll(SpectrumFrame& frame) {
    auto generation = generation_.load(std::memory_order_acquire);
    if (generation & 1) return false;
    double rate = rate_.load(std::memory_order_relaxed);
    bool reset = generation != consumerGeneration_;
    if (gap_.exchange(false, std::memory_order_acq_rel)) reset = true;
    if (reset) {
        discard(); consumerGeneration_ = generation;
        frame = {}; frame.sampleRate = rate; frame.generation = generation;
        return true;
    }
    auto read = read_.load(std::memory_order_relaxed), write = written_.load(std::memory_order_acquire);
    for (; read < write; ++read) {
        samples_[position_] = queue_[read & (Capacity - 1)];
        position_ = (position_ + 1) & (SpectrumFrame::Size - 1);
        count_ = std::min(count_ + 1, SpectrumFrame::Size);
        ++sinceFrame_;
    }
    read_.store(read, std::memory_order_release);
    if (generation_.load(std::memory_order_acquire) != generation || gap_.load(std::memory_order_acquire)) {
        discard(); frame = {}; frame.sampleRate = rate; frame.generation = generation;
        return true;
    }
    unsigned hop = static_cast<unsigned>(std::max(1.0, rate / 20));
    if (rate <= 0 || count_ < SpectrumFrame::Size || sinceFrame_ < hop) return false;
    sinceFrame_ = 0;
    frame.sampleRate = rate; frame.generation = generation; frame.valid = true;
    transform(frame);
    return true;
}
void Spectrum::transform(SpectrumFrame& frame) {
    constexpr unsigned size = SpectrumFrame::Size;
    constexpr float pi = std::numbers::pi_v<float>;
    float mean = 0;
    for (float sample : samples_) mean += sample;
    mean /= size;
    float windowSum = 0;
    for (unsigned i = 0; i < size; ++i) {
        float window = .5f - .5f * std::cos(2 * pi * i / (size - 1));
        fft_[i] = {(samples_[(position_ + i) & (size - 1)] - mean) * window, 0};
        windowSum += window;
    }
    for (unsigned i = 1, j = 0; i < size; ++i) {
        unsigned bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(fft_[i], fft_[j]);
    }
    for (unsigned length = 2; length <= size; length <<= 1) {
        auto step = std::polar(1.0f, -2 * pi / length);
        for (unsigned start = 0; start < size; start += length) {
            std::complex<float> phase{1, 0};
            for (unsigned j = 0; j < length / 2; ++j) {
                auto even = fft_[start + j], odd = fft_[start + j + length / 2] * phase;
                fft_[start + j] = even + odd; fft_[start + j + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
    // Coherent Hann gain: a bin-centred full-scale sine reads 0 dBFS.
    // These are peak amplitudes per FFT bin, not RF strength or dB/Hz PSD.
    for (unsigned i = 0; i < SpectrumFrame::Bins; ++i) {
        float amplitude = std::abs(fft_[i]) * (i == 0 || i == size / 2 ? 1 : 2) / windowSum;
        frame.db[i] = std::max(-140.0f, 20 * std::log10(std::max(amplitude, 1e-7f)));
    }
}
}
