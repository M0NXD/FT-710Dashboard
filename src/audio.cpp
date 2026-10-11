#include "audio.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wtsapi32.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <sstream>

namespace audio {
namespace {
template<class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** out() { return &p; }
};
struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct Event {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() { if (value) CloseHandle(value); }
};
struct AudioError : std::runtime_error {
    HRESULT hr; std::wstring operation;
    AudioError(HRESULT result, const wchar_t* where) : std::runtime_error("Windows audio operation failed."), hr(result), operation(where) {}
};
void check(HRESULT result, const wchar_t* operation) {
    if (FAILED(result)) throw AudioError(result, operation);
}
std::wstring errorText(const AudioError& error) {
    std::wostringstream result;
    result << error.operation << L" failed (0x" << std::hex << static_cast<unsigned long>(error.hr) << L")";
    if (error.hr == AUDCLNT_E_DEVICE_INVALIDATED || error.hr == AUDCLNT_E_RESOURCES_INVALIDATED)
        result << L". Device/session changed; refresh devices and enable again.";
    else if (error.hr == E_ACCESSDENIED) result << L". Allow desktop microphone access in Windows Settings.";
    else if (error.hr == AUDCLNT_E_DEVICE_IN_USE) result << L". Another app has exclusive access.";
    return result.str();
}
}

bool remoteSession() {
    LPWSTR buffer = nullptr; DWORD bytes = 0;
    bool remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSClientProtocolType, &buffer, &bytes)) {
        if (bytes >= sizeof(USHORT)) remote = remote || *reinterpret_cast<USHORT*>(buffer) == 2;
        WTSFreeMemory(buffer);
    }
    return remote;
}
std::wstring savedDeviceName(const std::wstring& identity, bool input) {
    // Recover labels for mappings made by v1.3, which saved IDs but not names.
    // Reading cached metadata does not make an unavailable device usable.
    auto guidAt = identity.rfind(L'.');
    if (guidAt == std::wstring::npos || identity.starts_with(L"ks:")) return {};
    std::wstring guidText = identity.substr(guidAt + 1);
    GUID guid{};
    if (FAILED(CLSIDFromString(guidText.c_str(), &guid))) return {};
    wchar_t canonical[40]{}; StringFromGUID2(guid, canonical, 40);
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\";
    key += input ? L"Capture\\" : L"Render\\";
    key += canonical; key += L"\\Properties";
    wchar_t name[1024]{}; DWORD bytes = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"{a45c254e-df1c-4efd-8020-67d146a850e0},14", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_SUCCESS)
        return name;
    return {};
}
std::vector<Device> devices() {
    Apartment apartment;
    check(apartment.hr, L"Audio COM initialization");
    Com<IMMDeviceEnumerator> enumeration;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumeration.out())), L"Audio device enumeration");
    std::vector<Device> result;
    for (auto flow : {eCapture, eRender}) {
        Com<IMMDeviceCollection> collection;
        check(enumeration->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.out()), L"List audio endpoints");
        UINT count = 0; check(collection->GetCount(&count), L"Count audio endpoints");
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> device; if (FAILED(collection->Item(i, device.out()))) continue;
            LPWSTR id = nullptr; if (FAILED(device->GetId(&id))) continue;
            Device entry; entry.id = id; entry.input = flow == eCapture; CoTaskMemFree(id);
            Com<IPropertyStore> properties;
            PROPVARIANT name{}; PropVariantInit(&name);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.out())) &&
                SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                entry.name = name.pwszVal;
            PropVariantClear(&name);
            PROPVARIANT form{}; PropVariantInit(&form);
            if (properties.p && SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_FormFactor, &form)) && form.vt == VT_UI4)
                entry.remote = form.ulVal == RemoteNetworkDevice;
            PropVariantClear(&form);
            if (entry.name.empty()) entry.name = entry.id;
            // Covers redirected drivers that omit their form-factor property.
            std::wstring lower = entry.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.find(L"remote audio") != std::wstring::npos || lower.find(L"rdp") != std::wstring::npos) entry.remote = true;
            entry.name += entry.remote ? L" [RDP / phone]" : L" [Windows shared]";
            result.push_back(std::move(entry));
        }
    }
    auto hardware = directDevices();
    result.insert(result.end(), hardware.begin(), hardware.end());
    result.push_back({std::wstring(BrowserReceive), L"Web browser / phone speakers", false, false, true});
    result.push_back({std::wstring(BrowserMicrophone), L"Web browser / phone microphone", true, false, true});
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.direct != b.direct) return a.direct > b.direct;
        return a.name < b.name;
    });
    return result;
}

Route::Route() { stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
Route::~Route() { stop(); if (stopEvent_) CloseHandle(stopEvent_); }
Status Route::status() const { std::lock_guard lock(mutex_); return status_; }
void Route::gate(bool enabled) {
    if (gate_.exchange(enabled) != enabled) { ++gateEpoch_; spectrum_.discontinuity(); if (receiveTap_) browserAudio().discontinuity(); }
}
void Route::gain(float value) { gain_ = std::clamp(value, 0.0f, 1.0f); }
void Route::stop() {
    gate(false);
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_); status_ = {};
}
void Route::start(const std::wstring& source, const std::wstring& destination) {
    stop();
    if (!stopEvent_) throw std::runtime_error("Cannot create the audio stop event.");
    if (source.empty() || destination.empty()) throw std::invalid_argument("Select both audio devices first.");
    ResetEvent(stopEvent_);
    { std::lock_guard lock(mutex_); status_.starting = true; status_.text = L"Opening selected devices..."; }
    thread_ = std::thread([this, source, destination] { run(source, destination); });
}
void Route::run(std::wstring source, std::wstring destination) {
    if (source.starts_with(L"ks:") || destination.starts_with(L"ks:") || source.starts_with(L"browser:") || destination.starts_with(L"browser:")) { runDirect(std::move(source), std::move(destination)); return; }
    Apartment apartment;
    HANDLE scheduler = nullptr;
    try {
        check(apartment.hr, L"Audio COM initialization");
        Com<IMMDeviceEnumerator> enumeration;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumeration.out())), L"Audio enumeration");
        Com<IMMDevice> sourceDevice, destinationDevice;
        check(enumeration->GetDevice(source.c_str(), sourceDevice.out()), L"Open source");
        check(enumeration->GetDevice(destination.c_str(), destinationDevice.out()), L"Open destination");
        // Event handles outlive the clients, including exception cleanup.
        Event captureEvent, renderEvent;
        if (!captureEvent.value || !renderEvent.value) throw std::runtime_error("Cannot create audio events.");
        Com<IAudioClient> input, output;
        check(sourceDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(input.out())), L"Activate source");
        check(destinationDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(output.out())), L"Activate destination");
        // Windows' shared audio engine converts endpoint channels/sample rates.
        // A uniform float stereo stream keeps both sides in the same frame domain.
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2; format.nSamplesPerSec = 48000; format.wBitsPerSample = 32;
        format.nBlockAlign = 8; format.nAvgBytesPerSec = 48000 * 8;
        spectrum_.configure(format.nSamplesPerSec);
        if (receiveTap_) browserAudio().source(format.nSamplesPerSec);
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        check(input->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000, 0, &format, nullptr), L"Initialize source audio");
        check(input->SetEventHandle(captureEvent.value), L"Set capture event");
        check(output->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000, 0, &format, nullptr), L"Initialize destination audio");
        check(output->SetEventHandle(renderEvent.value), L"Set playback event");
        Com<IAudioCaptureClient> capture;
        Com<IAudioRenderClient> render;
        check(input->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.out())), L"Get capture stream");
        check(output->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.out())), L"Get playback stream");
        UINT32 capacity = 0; check(output->GetBufferSize(&capacity), L"Playback buffer size");
        // Prime with silence; microphone audio cannot start before confirmed PTT.
        BYTE* data = nullptr; check(render->GetBuffer(capacity, &data), L"Prime playback");
        check(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT), L"Prime silence");
        check(output->Start(), L"Start playback");
        check(input->Start(), L"Start capture");
        DWORD task = 0; scheduler = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        { std::lock_guard lock(mutex_); status_.active = true; status_.starting = false; status_.text = L"Running"; }
        std::deque<float> frames;
        bool buffering = true;
        unsigned epoch = gateEpoch_;
        float sourcePeak = 0, destinationPeak = 0;
        auto reported = std::chrono::steady_clock::now();
        HANDLE events[]{stopEvent_, captureEvent.value, renderEvent.value};
        while (true) {
            DWORD signaled = WaitForMultipleObjects(3, events, FALSE, 100);
            if (signaled == WAIT_OBJECT_0) break;
            if (signaled == WAIT_FAILED) throw std::runtime_error("Audio event wait failed.");
            bool enabled = gate_;
            if (epoch != gateEpoch_.load()) {
                epoch = gateEpoch_; frames.clear(); buffering = true;
                // Remove already queued audio on a gate transition.
                check(output->Stop(), L"Pause playback"); check(output->Reset(), L"Clear playback");
                check(output->Start(), L"Resume playback");
            }
            UINT32 packet = 0; check(capture->GetNextPacketSize(&packet), L"Read capture availability");
            while (packet) {
                UINT32 count = 0; DWORD captureFlags = 0;
                check(capture->GetBuffer(&data, &count, &captureFlags, nullptr, nullptr), L"Read source audio");
                auto samples = reinterpret_cast<const float*>(data);
                if (captureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    frames.clear(); buffering = true; spectrum_.discontinuity();
                    if (receiveTap_) browserAudio().discontinuity();
                }
                for (UINT32 i = 0; i < count; ++i) {
                    float sample = captureFlags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : (samples[i * 2] + samples[i * 2 + 1]) * .5f;
                    if (!std::isfinite(sample)) sample = 0;
                    sourcePeak = std::max(sourcePeak, std::abs(sample));
                    if (enabled) { frames.push_back(sample); spectrum_.push(sample); if (receiveTap_) browserAudio().receive(sample * gain_.load()); }
                }
                check(capture->ReleaseBuffer(count), L"Release capture buffer");
                check(capture->GetNextPacketSize(&packet), L"Read next capture packet");
            }
            // Bound local delay to 100 ms. Drop old samples after stalls/clock drift.
            if (frames.size() > 4800) frames.erase(frames.begin(), frames.end() - 1440);
            UINT32 padding = 0; check(output->GetCurrentPadding(&padding), L"Read playback availability");
            UINT32 available = capacity > padding ? capacity - padding : 0;
            if (available) {
                check(render->GetBuffer(available, &data), L"Get playback buffer");
                auto samples = reinterpret_cast<float*>(data);
                if (buffering && frames.size() >= 960) buffering = false;
                bool underrun = false;
                for (UINT32 i = 0; i < available; ++i) {
                    float sample = 0;
                    if (enabled && !buffering && !frames.empty()) {
                        sample = std::clamp(frames.front() * gain_.load(), -1.0f, 1.0f); frames.pop_front();
                    } else if (enabled && !buffering) underrun = true;
                    samples[i * 2] = samples[i * 2 + 1] = sample;
                    destinationPeak = std::max(destinationPeak, std::abs(sample));
                }
                if (underrun) buffering = true;
                // Recheck the gate so a PTT-off cannot race this render buffer.
                check(render->ReleaseBuffer(available, gate_ ? 0 : AUDCLNT_BUFFERFLAGS_SILENT), L"Write destination audio");
            }
            if (std::chrono::steady_clock::now() - reported >= std::chrono::milliseconds(200)) {
                std::lock_guard lock(mutex_); status_.inputPeak = sourcePeak; status_.outputPeak = enabled ? destinationPeak : 0;
                status_.text = enabled ? L"Routing audio" : L"Enabled / muted";
                sourcePeak = destinationPeak = 0; reported = std::chrono::steady_clock::now();
            }
        }
        input->Stop(); output->Stop();
    } catch (const AudioError& error) {
        std::lock_guard lock(mutex_); status_.text = errorText(error);
    } catch (const std::exception& error) {
        std::string text = error.what(); std::lock_guard lock(mutex_); status_.text.assign(text.begin(), text.end());
    } catch (...) {
        std::lock_guard lock(mutex_); status_.text = L"Audio route failed.";
    }
    if (scheduler) AvRevertMmThreadCharacteristics(scheduler);
    std::lock_guard lock(mutex_); status_.active = status_.starting = false; status_.inputPeak = status_.outputPeak = 0;
}
}
