#include "browser_audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio {
BrowserAudio& browserAudio() { static BrowserAudio hub; return hub; }
void BrowserAudio::source(double rate) {
    receiveEpoch_.fetch_add(1, std::memory_order_acq_rel);
    rate_ = rate; next_ = 0; input_ = 0; havePrevious_ = false;
    receiveEpoch_.fetch_add(1, std::memory_order_release);
}
void BrowserAudio::output(float sample) noexcept {
    auto write = receiveWrite_.load(std::memory_order_relaxed);
    receive_[write & (Capacity - 1)].store(sample, std::memory_order_relaxed);
    receiveWrite_.store(write + 1, std::memory_order_release);
}
void BrowserAudio::receive(float sample) noexcept {
    if (receiveReset_.exchange(false)) { next_ = 0; input_ = 0; havePrevious_ = false; }
    sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0;
    if (!havePrevious_) { previous_ = sample; havePrevious_ = true; output(sample); next_ = rate_ / 48000; input_ = 1; return; }
    while (next_ <= input_) {
        output(previous_ + (sample - previous_) * static_cast<float>(next_ - (input_ - 1)));
        next_ += rate_ / 48000;
    }
    previous_ = sample; ++input_;
}
bool BrowserAudio::readReceive(std::uint64_t& cursor, std::uint64_t& epoch, std::vector<char>& packet) const {
    auto generation = receiveEpoch_.load(std::memory_order_acquire);
    if (generation & 1) return false;
    auto write = receiveWrite_.load(std::memory_order_acquire);
    if (generation != epoch) { epoch = generation; cursor = write; return false; }
    // A stalled connection resumes near live audio rather than playing a backlog.
    if (write - cursor > 7200) cursor = write - 2880;
    constexpr unsigned frames = 960;
    if (write - cursor < frames) return false;
    packet.resize(8 + frames * 2);
    std::uint32_t type = 1, sequence = static_cast<std::uint32_t>(cursor);
    std::memcpy(packet.data(), &type, 4); std::memcpy(packet.data() + 4, &sequence, 4);
    for (unsigned i = 0; i < frames; ++i) {
        float sample = receive_[(cursor + i) & (Capacity - 1)].load(std::memory_order_relaxed);
        auto pcm = static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767));
        std::memcpy(packet.data() + 8 + i * 2, &pcm, 2);
    }
    if (receiveEpoch_.load(std::memory_order_acquire) != generation || receiveWrite_.load() - cursor > Capacity) { cursor = receiveWrite_.load(); return false; }
    cursor += frames; return true;
}
void BrowserAudio::microphoneOwner(std::uint64_t owner, bool enabled) {
    std::lock_guard lock(producerMutex_);
    if (owner_.load() != owner || micEnabled_.load() != enabled) {
        micEnabled_ = false; owner_ = owner;
        micEpoch_.fetch_add(1, std::memory_order_release); micEnabled_ = enabled;
    }
}
bool BrowserAudio::microphone(std::uint64_t client, std::uint32_t lease, const char* pcm, unsigned bytes) {
    if (!bytes || bytes > 8192 || bytes % 2) return false;
    std::lock_guard lock(producerMutex_);
    if (!micEnabled_.load() || owner_.load() != client || lease != static_cast<std::uint32_t>(micEpoch_.load())) return false;
    auto write = micWrite_.load(std::memory_order_relaxed), read = micRead_.load(std::memory_order_acquire);
    for (unsigned i = 0; i < bytes / 2; ++i) {
        if (write - read >= Capacity) break;
        std::int16_t value; std::memcpy(&value, pcm + i * 2, 2);
        microphone_[write++ & (Capacity - 1)] = value / 32768.0f;
    }
    micWrite_.store(write, std::memory_order_release); return true;
}
unsigned BrowserAudio::pullMicrophone(float* samples, unsigned count) noexcept {
    auto read = micRead_.load(std::memory_order_relaxed), write = micWrite_.load(std::memory_order_acquire);
    auto epoch = micEpoch_.load(std::memory_order_acquire);
    if (epoch != renderEpoch_ || !micEnabled_.load()) { read = write; renderEpoch_ = epoch; }
    if (write - read > 4800) read = write - 1440;
    unsigned available = static_cast<unsigned>(std::min<std::uint64_t>(count, write - read));
    for (unsigned i = 0; i < available; ++i) samples[i] = microphone_[read++ & (Capacity - 1)];
    micRead_.store(read, std::memory_order_release); return available;
}
}
