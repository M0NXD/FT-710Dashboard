#include "audio.hpp"
#include <portaudio.h>
#include <pa_win_wasapi.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

extern "C" PaError Dashboard_KsDeviceIdentity(PaDeviceIndex index, wchar_t* identity, unsigned capacity);

namespace audio {
namespace {
std::wstring widen(const char* text) {
    if (!text) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring result(count, 0);
    if (count) { MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count); result.pop_back(); }
    return result;
}
std::wstring deviceLabel(const char* text) {
    auto name = widen(text);
    for (auto& c : name) if (c < L' ') c = L' ';
    // Some Bluetooth drivers return an unresolved resource string followed by
    // the actual device name. Keep the user-facing pin and device portions.
    auto resource = name.find(L"(@System32");
    auto device = name.rfind(L";(");
    if (resource != std::wstring::npos && device != std::wstring::npos && device > resource) {
        auto suffix = name.substr(device + 2);
        while (!suffix.empty() && suffix.back() == L')') suffix.pop_back();
        name = name.substr(0, resource) + L"(" + suffix + L")";
    }
    return name;
}
void check(PaError result, const char* operation) {
    if (result >= 0) return;
    std::string message = std::string(operation) + ": " + Pa_GetErrorText(result);
    if (result == paUnanticipatedHostError) {
        const auto* error = Pa_GetLastHostErrorInfo();
        if (error && error->errorText) message += std::string(" / ") + error->errorText;
    }
    throw std::runtime_error(message);
}

// All PortAudio control calls/COM objects belong to one STA thread. Two route
// threads may stream concurrently; callbacks never wait on this control queue.
class Driver;
std::atomic<Driver*> activeDriver{nullptr};
class Driver {
public:
    PaError initialized = paNotInitialized;
    unsigned streams = 0;
    Driver() { thread_ = std::thread([this] { run(); }); activeDriver = this; }
    ~Driver() { stop(); activeDriver = nullptr; }
    void stop() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    template<class F> auto call(F action) -> decltype(action()) {
        using Result = decltype(action());
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(action));
        auto future = task->get_future();
        { std::lock_guard lock(mutex_);
          if (stopping_) throw std::runtime_error("Audio driver is shutting down");
          jobs_.push_back([task] { (*task)(); }); }
        wake_.notify_one(); return future.get();
    }
    void refresh() {
        if (streams) return;
        if (initialized == paNoError) Pa_Terminate();
        initialized = Pa_Initialize();
        check(initialized, "Initialize direct audio driver");
    }
private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
    bool stopping_ = false;
    void run() {
        HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) break;
                job = std::move(jobs_.front()); jobs_.pop_front();
            }
            job();
        }
        if (initialized == paNoError) Pa_Terminate();
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};
Driver& driver() { static Driver instance; return instance; }
std::mutex issueMutex;
std::wstring deviceIssue;

std::wstring identity(PaDeviceIndex index) {
    const auto* info = Pa_GetDeviceInfo(index); if (!info) return {};
    const auto* api = Pa_GetHostApiInfo(info->hostApi); if (!api) return {};
    if (api->type == paWDMKS) {
        wchar_t path[1024]{};
        return Dashboard_KsDeviceIdentity(index, path, 1024) == paNoError ? path : L"";
    }
    if (api->type == paWASAPI) {
        IMMDevice* endpoint = nullptr;
        if (PaWasapi_GetIMMDevice(index, reinterpret_cast<void**>(&endpoint)) != paNoError || !endpoint) return {};
        LPWSTR id = nullptr; if (FAILED(endpoint->GetId(&id))) return {};
        std::wstring result(id); CoTaskMemFree(id); return result; // endpoint is borrowed from PortAudio
    }
    return {};
}
PaDeviceIndex resolve(const std::wstring& id, bool input) {
    PaDeviceIndex match = paNoDevice;
    for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || (input ? info->maxInputChannels : info->maxOutputChannels) < 1 || identity(i) != id) continue;
        if (match != paNoDevice) throw std::runtime_error("Audio device identity is ambiguous; choose a different endpoint.");
        match = i;
    }
    if (match == paNoDevice) throw std::runtime_error("Selected audio device is unavailable. Reconnect it and Refresh devices; no default device was substituted.");
    return match;
}

struct Bridge {
    static constexpr std::uint64_t Capacity = 32768;
    std::array<float, Capacity> samples{};
    std::atomic<std::uint64_t> written{0}, read{0};
    std::atomic<float> inputPeak{0}, outputPeak{0};
    std::atomic<bool> discontinuity{false};
    std::atomic<bool>* gate;
    std::atomic<unsigned>* epoch;
    std::atomic<float>* gain;
    Spectrum* spectrum;
    bool browserInput = false, browserOutput = false, receiveTap = false;
    int inputChannels = 1, outputChannels = 1;
    double inputRate = 48000, outputRate = 48000, phase = 0;
    unsigned previousEpoch = 0;
    bool buffering = true;
    static void peak(std::atomic<float>& target, float value) {
        float previous = target.load(std::memory_order_relaxed);
        while (value > previous && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
    }
    static int capture(const void* input, void*, unsigned long count, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags, void* context) {
        auto& b = *static_cast<Bridge*>(context);
        const auto* data = static_cast<const float*>(input);
        if (flags & paInputOverflow) { b.discontinuity = true; b.spectrum->discontinuity(); if (b.receiveTap) browserAudio().discontinuity(); }
        auto write = b.written.load(std::memory_order_relaxed), read = b.read.load(std::memory_order_acquire);
        float maximum = 0;
        bool enabled = b.gate->load();
        for (unsigned long i = 0; i < count; ++i) {
            float sample = data ? data[i * b.inputChannels] : 0;
            if (data && b.inputChannels == 2) sample = (sample + data[i * 2 + 1]) * .5f;
            if (!std::isfinite(sample)) sample = 0;
            maximum = std::max(maximum, std::abs(sample));
            if (enabled) b.spectrum->push(sample);
            if (enabled && b.receiveTap) browserAudio().receive(sample * b.gain->load());
            if (enabled && b.browserOutput) peak(b.outputPeak, std::abs(sample * b.gain->load()));
            if (enabled && !b.browserOutput && write - read < Capacity - 1) b.samples[write++ & (Capacity - 1)] = sample;
        }
        b.written.store(write, std::memory_order_release); peak(b.inputPeak, maximum);
        return paContinue;
    }
    static int render(const void*, void* output, unsigned long count, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* context) {
        auto& b = *static_cast<Bridge*>(context);
        auto* data = static_cast<float*>(output); if (!data) return paContinue;
        if (b.browserInput) {
            std::array<float, 8192> incoming{};
            unsigned copied = browserAudio().pullMicrophone(incoming.data(), static_cast<unsigned>(incoming.size()));
            auto write = b.written.load(), read = b.read.load();
            float maximum = 0;
            for (unsigned i = 0; i < copied && write - read < Capacity - 1; ++i) {
                b.samples[write++ & (Capacity - 1)] = incoming[i]; maximum = std::max(maximum, std::abs(incoming[i]));
            }
            b.written.store(write); peak(b.inputPeak, maximum);
        }
        auto read = b.read.load(std::memory_order_relaxed), write = b.written.load(std::memory_order_acquire);
        unsigned epoch = b.epoch->load();
        bool enabled = b.gate->load();
        if (!enabled || epoch != b.previousEpoch || b.discontinuity.exchange(false)) {
            read = write; b.phase = 0; b.buffering = true; b.previousEpoch = epoch;
        }
        // Separate devices have independent clocks. Bound accumulated local
        // delay and resume from a short buffer after a stall/discontinuity.
        if (write - read > static_cast<std::uint64_t>(b.inputRate * .1)) {
            read = write - static_cast<std::uint64_t>(b.inputRate * .03); b.phase = 0;
        }
        if (b.buffering && write - read >= static_cast<std::uint64_t>(b.inputRate * .02)) b.buffering = false;
        double step = b.inputRate / b.outputRate;
        auto needed = static_cast<std::uint64_t>(std::ceil(step)) + 1;
        float maximum = 0;
        for (unsigned long i = 0; i < count; ++i) {
            float sample = 0;
            if (enabled && !b.buffering && write - read >= needed) {
                float a = b.samples[read & (Capacity - 1)], c = b.samples[(read + 1) & (Capacity - 1)];
                sample = std::clamp(static_cast<float>(a + (c - a) * b.phase) * b.gain->load(), -1.0f, 1.0f);
                b.phase += step; auto advance = static_cast<std::uint64_t>(b.phase); read += advance; b.phase -= advance;
            } else if (enabled && !b.buffering) { b.buffering = true; b.phase = 0; }
            if (!b.gate->load()) sample = 0;
            for (int channel = 0; channel < b.outputChannels; ++channel) data[i * b.outputChannels + channel] = sample;
            maximum = std::max(maximum, std::abs(sample));
        }
        b.read.store(read, std::memory_order_release); peak(b.outputPeak, maximum);
        return paContinue;
    }
};

struct Format {
    PaStreamParameters parameters{};
    PaWasapiStreamInfo wasapi{};
    double rate = 48000;
};
Format format(PaDeviceIndex index, bool input) {
    const auto* info = Pa_GetDeviceInfo(index);
    const auto* api = Pa_GetHostApiInfo(info->hostApi);
    Format result; result.parameters.device = index; result.parameters.sampleFormat = paFloat32;
    result.parameters.suggestedLatency = .04;
    if (api->type == paWASAPI) {
        result.wasapi.size = sizeof(result.wasapi); result.wasapi.hostApiType = paWASAPI; result.wasapi.version = 1;
        result.wasapi.flags = paWinWasapiAutoConvert;
        result.parameters.hostApiSpecificStreamInfo = &result.wasapi;
    }
    int maximum = input ? info->maxInputChannels : info->maxOutputChannels;
    std::array<double, 5> rates{api->type == paWASAPI ? info->defaultSampleRate : 48000, 48000, 44100, info->defaultSampleRate, 32000};
    for (auto rate : rates) for (int channels : {std::min(maximum, 2), 1}) {
        result.parameters.channelCount = channels;
        if (Pa_IsFormatSupported(input ? &result.parameters : nullptr, input ? nullptr : &result.parameters, rate) == paFormatIsSupported) {
            result.rate = rate; result.parameters.hostApiSpecificStreamInfo = nullptr; return result;
        }
    }
    throw std::runtime_error("Selected audio endpoint has no supported mono/stereo PCM format. Choose another endpoint or use its Windows shared entry.");
}
}

std::wstring directDeviceIssue() { std::lock_guard lock(issueMutex); return deviceIssue; }
void shutdownDriver() { if (auto* current = activeDriver.load()) current->stop(); }
std::vector<Device> directDevices() {
    try {
        auto result = driver().call([&] {
            driver().refresh(); std::vector<Device> result;
            for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
                const auto* info = Pa_GetDeviceInfo(i); const auto* api = info ? Pa_GetHostApiInfo(info->hostApi) : nullptr;
                if (!api || api->type != paWDMKS) continue;
                auto id = identity(i); if (id.empty()) continue;
                for (bool input : {true, false}) if ((input ? info->maxInputChannels : info->maxOutputChannels) > 0)
                    result.push_back({id, deviceLabel(info->name) + L" [Host hardware / direct]", input, true, false});
            }
            return result;
        });
        std::lock_guard lock(issueMutex); deviceIssue.clear(); return result;
    } catch (const std::exception& error) {
        std::lock_guard lock(issueMutex); deviceIssue = widen(error.what()); return {};
    }
}

void Route::runDirect(std::wstring source, std::wstring destination) {
    Bridge bridge; bridge.gate = &gate_; bridge.epoch = &gateEpoch_; bridge.gain = &gain_; bridge.spectrum = &spectrum_;
    bridge.browserInput = source == BrowserMicrophone; bridge.browserOutput = destination == BrowserReceive; bridge.receiveTap = receiveTap_;
    PaStream* capture = nullptr; PaStream* render = nullptr;
    try {
        driver().call([&] {
            if (shuttingDown_) return;
            if (!driver().streams) driver().refresh();
            check(driver().initialized, "Direct audio driver");
            Format input{}, output{};
            if (!bridge.browserInput) input = format(resolve(source, true), true);
            else { input.rate = 48000; input.parameters.channelCount = 1; }
            if (!bridge.browserOutput) output = format(resolve(destination, false), false);
            else { output.rate = 48000; output.parameters.channelCount = 1; }
            bridge.inputChannels = input.parameters.channelCount; bridge.outputChannels = output.parameters.channelCount;
            bridge.inputRate = input.rate; bridge.outputRate = output.rate;
            spectrum_.configure(input.rate);
            if (receiveTap_) browserAudio().source(input.rate);
            // Repair pointers after returning the value-owned format objects.
            if (!bridge.browserInput) {
                if (Pa_GetHostApiInfo(Pa_GetDeviceInfo(input.parameters.device)->hostApi)->type == paWASAPI) input.parameters.hostApiSpecificStreamInfo = &input.wasapi;
                check(Pa_OpenStream(&capture, &input.parameters, nullptr, input.rate, paFramesPerBufferUnspecified, paNoFlag, Bridge::capture, &bridge), "Open capture device");
                ++driver().streams;
            }
            if (!bridge.browserOutput) {
                if (Pa_GetHostApiInfo(Pa_GetDeviceInfo(output.parameters.device)->hostApi)->type == paWASAPI) output.parameters.hostApiSpecificStreamInfo = &output.wasapi;
                check(Pa_OpenStream(&render, nullptr, &output.parameters, output.rate, paFramesPerBufferUnspecified, paNoFlag, Bridge::render, &bridge), "Open playback device");
                ++driver().streams;
                check(Pa_StartStream(render), "Start playback device");
            }
            if (capture) check(Pa_StartStream(capture), "Start capture device");
        });
        { std::lock_guard lock(mutex_); status_.active = true; status_.starting = false; status_.text = L"Running / host hardware"; }
        while (!shuttingDown_) {
            DWORD wait = WaitForSingleObject(stopEvent_, 100);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) throw std::runtime_error("Direct audio stop wait failed.");
            driver().call([&] {
                int input = capture ? Pa_IsStreamActive(capture) : 1, output = render ? Pa_IsStreamActive(render) : 1;
                check(input, "Capture device state"); check(output, "Playback device state");
                if (!input || !output) throw std::runtime_error("Audio device stopped or disconnected. Refresh devices and enable again.");
            });
            std::lock_guard lock(mutex_);
            status_.inputPeak = bridge.inputPeak.exchange(0); status_.outputPeak = bridge.outputPeak.exchange(0);
            status_.text = gate_ ? L"Routing / host hardware" : L"Enabled / muted";
        }
    } catch (const std::exception& error) {
        gate(false);
        std::lock_guard lock(mutex_); status_.active = status_.starting = false; status_.text = widen(error.what());
    } catch (...) {
        gate(false);
        std::lock_guard lock(mutex_); status_.active = status_.starting = false; status_.text = L"Direct hardware audio failed.";
    }
    // Closing waits for callbacks before their bridge/context leaves scope.
    driver().call([&] {
        if (capture) { Pa_AbortStream(capture); Pa_CloseStream(capture); --driver().streams; }
        if (render) { Pa_AbortStream(render); Pa_CloseStream(render); --driver().streams; }
    });
    std::lock_guard lock(mutex_); status_.active = status_.starting = false; status_.inputPeak = status_.outputPeak = 0;
}
}
