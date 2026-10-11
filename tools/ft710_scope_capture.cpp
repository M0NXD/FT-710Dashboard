#include <windows.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kFtOpenByDescription = 2;
constexpr std::uint32_t kSpiIoSingle = 1;
constexpr std::uint32_t kClockDiv64 = 6;
constexpr std::uint32_t kClockIdleHigh = 1;
constexpr std::uint32_t kClockLeading = 0;
constexpr std::uint32_t kSystemClock24Mhz = 1;
constexpr std::uint32_t kFt4222TransferInProgress = 10;
constexpr std::size_t kScopeFrameBytes = 4096;
constexpr std::size_t kSpectrumBins = 850;
constexpr std::array<std::uint8_t, 4> kSyncTail{0xFF, 0x01, 0xEE, 0x01};

using FtHandle = void*;
using FtStatus = std::uint32_t;

using FtOpenEx = FtStatus(WINAPI*)(void*, std::uint32_t, FtHandle*);
using FtClose = FtStatus(WINAPI*)(FtHandle);
using FtSetTimeouts = FtStatus(WINAPI*)(FtHandle, std::uint32_t, std::uint32_t);
using FtSetLatencyTimer = FtStatus(WINAPI*)(FtHandle, std::uint8_t);
using Ft4222SpiMasterInit = std::uint32_t(__cdecl*)(FtHandle, std::uint32_t, std::uint32_t,
                                                      std::uint32_t, std::uint32_t, std::uint8_t);
using Ft4222SetClock = std::uint32_t(__cdecl*)(FtHandle, std::uint32_t);
using Ft4222SpiMasterSingleRead = std::uint32_t(__cdecl*)(FtHandle, std::uint8_t*, std::uint16_t,
                                                           std::uint16_t*, BOOL);
using Ft4222Uninitialize = std::uint32_t(__cdecl*)(FtHandle);

struct Libraries {
    HMODULE d2xxModule = nullptr;
    HMODULE ft4222Module = nullptr;
    FtOpenEx openEx = nullptr;
    FtClose close = nullptr;
    FtSetTimeouts setTimeouts = nullptr;
    FtSetLatencyTimer setLatencyTimer = nullptr;
    Ft4222SpiMasterInit spiMasterInit = nullptr;
    Ft4222SetClock setClock = nullptr;
    Ft4222SpiMasterSingleRead singleRead = nullptr;
    Ft4222Uninitialize uninitialize = nullptr;

    ~Libraries() {
        if (ft4222Module) FreeLibrary(ft4222Module);
        if (d2xxModule) FreeLibrary(d2xxModule);
    }
};

struct Device {
    Libraries& libs;
    FtHandle handle = nullptr;
    bool initialized = false;

    ~Device() {
        if (handle && initialized && libs.uninitialize) libs.uninitialize(handle);
        if (handle && libs.close) libs.close(handle);
    }
};

template <typename T>
bool loadSymbol(HMODULE module, const char* name, T& target) {
    const auto address = GetProcAddress(module, name);
    static_assert(sizeof(target) == sizeof(address), "Windows function pointer size mismatch");
    std::memcpy(&target, &address, sizeof(target));
    if (!target) std::cerr << "Missing function " << name << " in FTDI library.\n";
    return target != nullptr;
}

std::optional<std::uint32_t> parsePositive(std::string_view value) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result == 0) return std::nullopt;
    return result;
}

void printUsage() {
    std::cout << "FT710ScopeCapture [--frames N] [--raw FILE] [--csv FILE]\n"
                 "  Captures FT-710 scope frames through its FT4222 USB-SPI interface.\n"
                 "  Defaults: one frame, scope-frames.bin, spectrum.csv\n";
}

bool parseArgs(int argc, char** argv, std::uint32_t& frames,
               std::filesystem::path& rawPath, std::filesystem::path& csvPath) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << ".\n";
            return false;
        }
        const std::string_view value(argv[++i]);
        if (arg == "--frames") {
            const auto parsed = parsePositive(value);
            if (!parsed || *parsed > 10000) {
                std::cerr << "--frames must be between 1 and 10000.\n";
                return false;
            }
            frames = *parsed;
        } else if (arg == "--raw") {
            rawPath = std::filesystem::path(value);
        } else if (arg == "--csv") {
            csvPath = std::filesystem::path(value);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

bool loadLibraries(Libraries& libs) {
    // The normal Windows DLL search path is used, so the FTDI DLLs may be
    // beside this executable or installed in a location already on PATH.
    libs.d2xxModule = LoadLibraryW(L"FTD2XX.dll");
    if (!libs.d2xxModule) libs.d2xxModule = LoadLibraryW(L"ftd2xx.dll");
    libs.ft4222Module = LoadLibraryW(L"LibFT4222.dll");
    if (!libs.ft4222Module) libs.ft4222Module = LoadLibraryW(L"libft4222.dll");
    if (!libs.d2xxModule || !libs.ft4222Module) {
        std::cerr << "Could not load FTD2XX.dll and LibFT4222.dll. Install the official FTDI "
                     "runtime or place matching x64 DLLs beside this executable.\n";
        return false;
    }

    return loadSymbol(libs.d2xxModule, "FT_OpenEx", libs.openEx) &&
           loadSymbol(libs.d2xxModule, "FT_Close", libs.close) &&
           loadSymbol(libs.d2xxModule, "FT_SetTimeouts", libs.setTimeouts) &&
           loadSymbol(libs.d2xxModule, "FT_SetLatencyTimer", libs.setLatencyTimer) &&
           loadSymbol(libs.ft4222Module, "FT4222_SPIMaster_Init", libs.spiMasterInit) &&
           loadSymbol(libs.ft4222Module, "FT4222_SetClock", libs.setClock) &&
           loadSymbol(libs.ft4222Module, "FT4222_SPIMaster_SingleRead", libs.singleRead) &&
           loadSymbol(libs.ft4222Module, "FT4222_UnInitialize", libs.uninitialize);
}

bool openDevice(Libraries& libs, Device& device) {
    char description[] = "FT4222 A";
    auto status = libs.openEx(description, kFtOpenByDescription, &device.handle);
    if (status != 0) {
        std::cerr << "FT_OpenEx(FT4222 A) failed: " << status << "\n";
        return false;
    }

    status = libs.setTimeouts(device.handle, 100, 100);
    if (status != 0) {
        std::cerr << "FT_SetTimeouts failed: " << status << "\n";
        return false;
    }
    status = libs.setLatencyTimer(device.handle, 2);
    if (status != 0) {
        std::cerr << "FT_SetLatencyTimer failed: " << status << "\n";
        return false;
    }

    auto spiStatus = libs.spiMasterInit(device.handle, kSpiIoSingle, kClockDiv64,
                                        kClockIdleHigh, kClockLeading, 0x01);
    if (spiStatus != 0) {
        std::cerr << "FT4222_SPIMaster_Init failed: " << spiStatus << "\n";
        return false;
    }
    device.initialized = true;
    spiStatus = libs.setClock(device.handle, kSystemClock24Mhz);
    if (spiStatus != 0) {
        std::cerr << "FT4222_SetClock failed: " << spiStatus << "\n";
        return false;
    }
    return true;
}

bool readFrame(Libraries& libs, FtHandle handle,
               std::array<std::uint8_t, kScopeFrameBytes>& frame) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        std::uint16_t bytesRead = 0;
        const auto status = libs.singleRead(handle, frame.data(),
                                            static_cast<std::uint16_t>(frame.size()),
                                            &bytesRead, FALSE);
        if (status == kFt4222TransferInProgress) {
            Sleep(2);
            continue;
        }
        if (status != 0) {
            std::cerr << "FT4222_SPIMaster_SingleRead failed: " << status << "\n";
            return false;
        }
        if (bytesRead != frame.size()) {
            std::cerr << "Short FT4222 read: " << bytesRead << " of " << frame.size() << " bytes.\n";
            return false;
        }
        return true;
    }
    std::cerr << "Timed out waiting for a complete FT-710 scope frame.\n";
    return false;
}

bool hasSyncTail(const std::array<std::uint8_t, kScopeFrameBytes>& frame) {
    for (std::size_t i = 0; i < kSyncTail.size(); ++i) {
        if (frame[frame.size() - kSyncTail.size() + i] != kSyncTail[i]) return false;
    }
    return true;
}

void writeCsvFrame(std::ofstream& csv, std::uint32_t frameNumber,
                   const std::array<std::uint8_t, kScopeFrameBytes>& frame) {
    for (std::size_t bin = 0; bin < kSpectrumBins; ++bin) {
        // Community decoders report the 850 display bytes as inverted.
        const auto level = static_cast<std::uint8_t>(~frame[bin]);
        csv << frameNumber << ',' << bin << ',' << static_cast<unsigned>(level) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        printUsage();
        return 0;
    }
    std::uint32_t frameCount = 1;
    std::filesystem::path rawPath = L"scope-frames.bin";
    std::filesystem::path csvPath = L"spectrum.csv";
    if (!parseArgs(argc, argv, frameCount, rawPath, csvPath)) return 2;

    std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
    std::ofstream csv(csvPath, std::ios::trunc);
    if (!raw || !csv) {
        std::cerr << "Could not create output files. Check the paths and write permissions.\n";
        return 2;
    }
    csv << "frame,bin,level_raw\n";

    Libraries libs;
    if (!loadLibraries(libs)) return 3;
    Device device{libs};
    if (!openDevice(libs, device)) return 4;

    std::array<std::uint8_t, kScopeFrameBytes> frame{};
    for (std::uint32_t number = 1; number <= frameCount; ++number) {
        if (!readFrame(libs, device.handle, frame)) return 5;
        raw.write(reinterpret_cast<const char*>(frame.data()),
                  static_cast<std::streamsize>(frame.size()));
        writeCsvFrame(csv, number, frame);
        if (!raw || !csv) {
            std::cerr << "Failed while writing capture output.\n";
            return 6;
        }
        std::cout << "Captured frame " << number << '/' << frameCount
                  << (hasSyncTail(frame) ? " (sync OK)" : " (sync tail differs; raw data kept)")
                  << "\n";
    }

    std::cout << "Wrote " << frameCount << " raw frame(s) to " << rawPath.string()
              << " and " << (frameCount * kSpectrumBins) << " spectrum bins to "
              << csvPath.string() << ".\n";
    return 0;
}
