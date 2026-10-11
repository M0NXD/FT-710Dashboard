#include "rf_scope.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

namespace rf_scope {
namespace {
constexpr std::array<std::uint32_t, 10> Spans{1000, 2000, 5000, 10000, 20000,
    50000, 100000, 200000, 500000, 1000000};
constexpr std::array<std::uint8_t, 4> SyncTail{0xFF, 0x01, 0xEE, 0x01};
constexpr std::uint32_t FtOpenByDescription = 2;
constexpr std::uint32_t SpiIoSingle = 1;
constexpr std::uint32_t ClockDiv64 = 6;
constexpr std::uint32_t ClockIdleHigh = 1;
constexpr std::uint32_t ClockLeading = 0;
constexpr std::uint32_t SystemClock24Mhz = 1;
constexpr std::uint32_t TransferInProgress = 10;

using FtHandle = void*;
using FtStatus = std::uint32_t;
using FtOpenEx = FtStatus(WINAPI*)(void*, std::uint32_t, FtHandle*);
using FtClose = FtStatus(WINAPI*)(FtHandle);
using FtSetTimeouts = FtStatus(WINAPI*)(FtHandle, std::uint32_t, std::uint32_t);
using FtSetLatencyTimer = FtStatus(WINAPI*)(FtHandle, std::uint8_t);
using SpiMasterInit = std::uint32_t(__cdecl*)(FtHandle, std::uint32_t, std::uint32_t,
                                               std::uint32_t, std::uint32_t, std::uint8_t);
using SetClock = std::uint32_t(__cdecl*)(FtHandle, std::uint32_t);
using SpiSingleRead = std::uint32_t(__cdecl*)(FtHandle, std::uint8_t*, std::uint16_t,
                                               std::uint16_t*, BOOL);
using Uninitialize = std::uint32_t(__cdecl*)(FtHandle);

template <typename T>
bool symbol(HMODULE library, const char* name, T& function) {
    auto address = GetProcAddress(library, name);
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function != nullptr;
}

struct Ftdi {
    HMODULE d2xx = nullptr, ft4222 = nullptr;
    FtOpenEx openEx = nullptr;
    FtClose close = nullptr;
    FtSetTimeouts setTimeouts = nullptr;
    FtSetLatencyTimer setLatencyTimer = nullptr;
    SpiMasterInit spiInit = nullptr;
    SetClock setClock = nullptr;
    SpiSingleRead read = nullptr;
    Uninitialize uninit = nullptr;
    FtHandle handle = nullptr;
    bool initialized = false;

    ~Ftdi() {
        if (handle && initialized && uninit) uninit(handle);
        if (handle && close) close(handle);
        if (ft4222) FreeLibrary(ft4222);
        if (d2xx) FreeLibrary(d2xx);
    }

    bool load() {
        d2xx = LoadLibraryW(L"FTD2XX.dll");
        if (!d2xx) d2xx = LoadLibraryW(L"ftd2xx.dll");
        ft4222 = LoadLibraryW(L"LibFT4222.dll");
        if (!ft4222) ft4222 = LoadLibraryW(L"libft4222.dll");
        if (!d2xx || !ft4222) return false;
        return symbol(d2xx, "FT_OpenEx", openEx) && symbol(d2xx, "FT_Close", close) &&
            symbol(d2xx, "FT_SetTimeouts", setTimeouts) &&
            symbol(d2xx, "FT_SetLatencyTimer", setLatencyTimer) &&
            symbol(ft4222, "FT4222_SPIMaster_Init", spiInit) &&
            symbol(ft4222, "FT4222_SetClock", setClock) &&
            symbol(ft4222, "FT4222_SPIMaster_SingleRead", read) &&
            symbol(ft4222, "FT4222_UnInitialize", uninit);
    }

    std::uint32_t open() {
        char description[] = "FT4222 A";
        auto result = openEx(description, FtOpenByDescription, &handle);
        if (result != 0) return result;
        if ((result = setTimeouts(handle, 100, 100)) != 0) return result;
        if ((result = setLatencyTimer(handle, 2)) != 0) return result;
        result = spiInit(handle, SpiIoSingle, ClockDiv64, ClockIdleHigh, ClockLeading, 0x01);
        if (result != 0) return result;
        initialized = true;
        return setClock(handle, SystemClock24Mhz);
    }

    std::uint32_t readFrame(std::array<std::uint8_t, FrameBytes>& bytes,
                            std::uint16_t& received, bool& waiting) {
        waiting = false;
        auto result = read(handle, bytes.data(), static_cast<std::uint16_t>(bytes.size()), &received, FALSE);
        waiting = result == TransferInProgress;
        return result;
    }
};

bool syncValid(const std::array<std::uint8_t, FrameBytes>& raw) {
    return std::equal(SyncTail.begin(), SyncTail.end(), raw.end() - SyncTail.size());
}

bool decodeBcdFrequency(const std::array<std::uint8_t, FrameBytes>& raw, std::uint64_t& frequency) {
    constexpr std::size_t offset = 2900 + 64;
    frequency = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto hi = static_cast<unsigned>(raw[offset + i] >> 4);
        const auto lo = static_cast<unsigned>(raw[offset + i] & 0x0F);
        if (hi > 9 || lo > 9) return false;
        frequency = frequency * 100 + hi * 10 + lo;
    }
    return frequency >= 30000 && frequency <= 60000000;
}

std::wstring errorText(const wchar_t* action, std::uint32_t code) {
    return std::wstring(action) + L" (FTDI error " + std::to_wstring(code) + L")";
}

} // namespace

Capture::~Capture() { stop(); }

bool Capture::start() {
    if (running_.load(std::memory_order_relaxed)) return true;
    if (thread_.joinable()) thread_.join();
    stopRequested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    setStatus(L"Opening FT4222 A for RF scope capture…");
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        running_.store(false, std::memory_order_relaxed);
        setStatus(L"Could not start RF scope capture thread");
        return false;
    }
    return true;
}

void Capture::stop() {
    stopRequested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_relaxed);
    setStatus(L"RF scope capture stopped");
}

bool Capture::latest(Frame& frame) const {
    std::lock_guard lock(mutex_);
    if (!latest_.valid) return false;
    frame = latest_;
    return true;
}

std::wstring Capture::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void Capture::setStatus(std::wstring status) {
    std::lock_guard lock(mutex_);
    status_ = std::move(status);
}

void Capture::run() {
    Ftdi ftdi;
    if (!ftdi.load()) {
        setStatus(L"FTDI libraries missing (FTD2XX.dll and LibFT4222.dll)");
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    const auto opened = ftdi.open();
    if (opened != 0) {
        setStatus(errorText(L"FT4222 A unavailable or already held", opened));
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    setStatus(L"FT4222 RF scope connected · waiting for valid frames");
    std::array<std::uint8_t, FrameBytes> raw{};
    unsigned badFrames = 0;
    while (!stopRequested_.load(std::memory_order_relaxed)) {
        std::uint16_t received = 0;
        bool waiting = false;
        const auto result = ftdi.readFrame(raw, received, waiting);
        if (waiting) { Sleep(2); continue; }
        if (result != 0) {
            setStatus(errorText(L"FT4222 SPI read failed", result));
            break;
        }
        if (received != raw.size() || !syncValid(raw)) {
            if (++badFrames >= 20) {
                setStatus(L"FT4222 stream did not produce aligned 4096-byte scope frames; restart capture to retry");
                break;
            }
            continue;
        }
        badFrames = 0;
        Frame decoded{};
        for (std::size_t i = 0; i < BinCount; ++i) decoded.bins[i] = static_cast<std::uint8_t>(~raw[i]);
        decoded.spanIndex = raw[2900 + 32];
        decoded.frequencyValid = decodeBcdFrequency(raw, decoded.centerHz);
        decoded.valid = decoded.spanIndex < Spans.size();
        if (!decoded.valid) continue;
        {
            std::lock_guard lock(mutex_);
            decoded.sequence = ++sequence_;
            latest_ = decoded;
            status_ = L"Live FT-710 RF scope · " + spanLabel(decoded.spanIndex) + L" · 850 bins";
        }
    }
    running_.store(false, std::memory_order_relaxed);
}

std::uint32_t spanHz(unsigned spanIndex) {
    return spanIndex < Spans.size() ? Spans[spanIndex] : 0;
}

std::wstring spanLabel(unsigned spanIndex) {
    if (spanIndex >= Spans.size()) return L"Unknown span";
    auto value = Spans[spanIndex];
    if (value < 1000) return std::to_wstring(value) + L" Hz";
    if (value < 1000000) return std::to_wstring(value / 1000) + L" kHz";
    return L"1 MHz";
}

std::wstring applySpan(const std::wstring& cat2Port, unsigned spanIndex, unsigned baudRate, double waterfallLevelDb) {
    if (spanIndex >= Spans.size()) return L"Select a supported FT-710 scope span";
    if (baudRate != 4800 && baudRate != 9600 && baudRate != 19200 && baudRate != 38400 && baudRate != 115200)
        return L"Select a supported CAT-2 baud rate";
    if (!std::isfinite(waterfallLevelDb) || waterfallLevelDb < -30.0 || waterfallLevelDb > 30.0 ||
        std::abs(waterfallLevelDb * 2.0 - std::round(waterfallLevelDb * 2.0)) > 0.001)
        return L"Waterfall level must be between -30 and +30 dB in 0.5 dB steps";
    std::wstring port = cat2Port;
    if (port.empty()) return L"Select the radio's Standard / CAT-2 COM port first";
    if (port.rfind(L"\\\\.\\", 0) != 0) port = L"\\\\.\\" + port;
    HANDLE serial = CreateFileW(port.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (serial == INVALID_HANDLE_VALUE)
        return L"Could not open " + cat2Port + L" for CAT-2 (Windows error " + std::to_wstring(GetLastError()) + L")";

    DCB settings{};
    settings.DCBlength = sizeof(settings);
    COMMTIMEOUTS timeouts{};
    timeouts.ReadTotalTimeoutConstant = 1500;
    timeouts.WriteTotalTimeoutConstant = 1000;
    bool configured = GetCommState(serial, &settings) != FALSE;
    if (configured) {
        settings.BaudRate = baudRate;
        settings.ByteSize = 8;
        settings.Parity = NOPARITY;
        settings.StopBits = ONESTOPBIT;
        settings.fBinary = TRUE;
        settings.fParity = FALSE;
        settings.fOutxCtsFlow = FALSE;
        settings.fOutxDsrFlow = FALSE;
        settings.fDtrControl = DTR_CONTROL_DISABLE;
        settings.fDsrSensitivity = FALSE;
        settings.fTXContinueOnXoff = TRUE;
        settings.fOutX = FALSE;
        settings.fInX = FALSE;
        settings.fRtsControl = RTS_CONTROL_DISABLE;
        settings.fAbortOnError = FALSE;
        configured = SetCommState(serial, &settings) != FALSE && SetCommTimeouts(serial, &timeouts) != FALSE;
    }
    if (!configured) {
        const auto error = GetLastError();
        CloseHandle(serial);
        return L"Could not configure CAT-2 serial port (Windows error " + std::to_wstring(error) + L")";
    }

    // SS P1=0 (fixed), P2=6 (scope mode), P3=0 (3DSS CENTER), P4-P7=0.
    // CAT is an 8-bit ASCII byte stream; don't pass Windows UTF-16 buffers to WriteFile.
    const std::string mode = "SS06000000;";
    // P3 is a single decimal digit (0–9), not a two-character value.
    // SS P1=0, P2=5 (span), P3=selected span, P4-P7=0.
    const std::string span = "SS05" + std::to_string(spanIndex) + "0000;";
    const int levelTenths = static_cast<int>(std::lround(std::abs(waterfallLevelDb) * 10.0));
    std::string level = waterfallLevelDb < 0 ? "-" : "+";
    level += (levelTenths / 10 < 10 ? "0" : "") + std::to_string(levelTenths / 10) + "." + (levelTenths % 10 ? "5" : "0");
    const std::string levelCommand = "SS04" + level + ";";
    auto writeCommand = [&](const std::string& command) {
        DWORD written = 0;
        if (!WriteFile(serial, command.data(), static_cast<DWORD>(command.size()), &written, nullptr)) return false;
        return written == command.size();
    };
    if (!writeCommand(mode) || !writeCommand(span) || !writeCommand(levelCommand)) {
        const auto error = GetLastError();
        CloseHandle(serial);
        return L"CAT-2 scope command write failed (Windows error " + std::to_wstring(error) + L")";
    }

    // Confirm the radio accepted both requested controls. A successful
    // WriteFile only means Windows queued the bytes.
    auto readReply = [&](const char* command, const char* prefix, std::string& result) {
        PurgeComm(serial, PURGE_RXCLEAR);
        if (!writeCommand(command)) return -1;
        const auto deadline = GetTickCount64() + 1800;
        while (GetTickCount64() < deadline && result.size() < 128) {
            char byte = 0; DWORD received = 0;
            if (!ReadFile(serial, &byte, 1, &received, nullptr)) return -2;
            if (!received) break;
            result.push_back(byte);
            const auto start = result.rfind(prefix);
            if (start != std::string::npos && result.find(';', start) != std::string::npos) return 0;
        }
        return 1;
    };
    Sleep(50);
    std::string spanReply;
    auto readStatus = readReply("SS05;", "SS05", spanReply);
    if (readStatus != 0) {
        const auto error = GetLastError(); CloseHandle(serial);
        if (readStatus == -2) return L"CAT-2 span readback failed (Windows error " + std::to_wstring(error) + L")";
        return L"No valid CAT-2 span readback. Check the Standard / CAT-2 COM port and CAT-2 baud rate.";
    }
    const auto spanStart = spanReply.rfind("SS05");
    if (spanStart == std::string::npos || spanStart + 9 >= spanReply.size() || spanReply[spanStart + 9] != ';' ||
        spanReply[spanStart + 4] < '0' || spanReply[spanStart + 4] > '9') {
        CloseHandle(serial); return L"No valid CAT-2 span readback. Check the Standard / CAT-2 COM port and CAT-2 baud rate.";
    }
    const auto reportedSpan = static_cast<unsigned>(spanReply[spanStart + 4] - '0');
    if (reportedSpan != spanIndex) {
        CloseHandle(serial); return L"Radio reports " + spanLabel(reportedSpan) + L" after the " + spanLabel(spanIndex) + L" request.";
    }
    std::string levelReply;
    readStatus = readReply("SS04;", "SS04", levelReply);
    const auto readbackError = GetLastError();
    CloseHandle(serial);
    if (readStatus != 0) {
        if (readStatus == -2) return L"CAT-2 waterfall-level readback failed (Windows error " + std::to_wstring(readbackError) + L")";
        return L"No valid CAT-2 waterfall-level readback. Check the Standard / CAT-2 COM port and CAT-2 baud rate.";
    }
    const auto levelStart = levelReply.rfind("SS04");
    if (levelStart == std::string::npos || levelStart + 9 >= levelReply.size() || levelReply[levelStart + 9] != ';')
        return L"No valid CAT-2 waterfall-level readback.";
    const auto field = levelReply.substr(levelStart + 4, 5);
    char* parsedEnd = nullptr;
    const double reportedLevel = std::strtod(field.c_str(), &parsedEnd);
    if (parsedEnd != field.c_str() + field.size() || !std::isfinite(reportedLevel)) return L"Radio returned an invalid CAT-2 waterfall level.";
    if (std::abs(reportedLevel - waterfallLevelDb) > 0.01)
        return L"Radio reports " + std::to_wstring(reportedLevel) + L" dB after the " + std::to_wstring(waterfallLevelDb) + L" dB request.";
    return {};
}

} // namespace rf_scope
