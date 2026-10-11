#include "dashboard.hpp"
#include <shellapi.h>
#include <filesystem>
#include <fstream>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool demo = false;
    bool preview = false;
    bool scopeDemo = false;
    int capturePage = 0;
    std::wstring capture;
    std::wstring audioInventory;
    for (int i = 1; i < count; ++i) {
        if (std::wstring(args[i]) == L"--demo") demo = true;
        if (std::wstring(args[i]) == L"--preview") { preview = true; demo = true; }
        if (std::wstring(args[i]) == L"--scope-demo") { scopeDemo = true; preview = true; demo = true; }
        if (std::wstring(args[i]) == L"--list-audio" && i + 1 < count) audioInventory = args[++i];
        if (std::wstring(args[i]) == L"--capture" && i + 1 < count) { capture = args[++i]; demo = true; }
        if (std::wstring(args[i]) == L"--capture-page" && i + 1 < count) {
            std::wstring page = args[++i];
            capturePage = page == L"scope" ? 4 : page == L"audio" ? 3 : page == L"settings" ? 1 : page == L"activity" ? 2 : 0;
        }
    }
    LocalFree(args);
    if (!audioInventory.empty()) {
        // Read-only diagnostics: enumerate devices without opening streams/CAT.
        try {
            auto toUtf8 = [](const std::wstring& value) {
                int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
                std::string text(count, 0); WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), count, nullptr, nullptr); return text;
            };
            std::ofstream file{std::filesystem::path(audioInventory)};
            if (!file) return 2;
            file << "RDP session: " << (audio::remoteSession() ? "yes" : "no") << '\n';
            for (const auto& device : audio::devices()) file << (device.input ? "INPUT " : "OUTPUT ") << toUtf8(device.name) << '\n' << "  " << toUtf8(device.id) << '\n';
            file << "Direct driver: " << toUtf8(audio::directDeviceIssue()) << '\n';
            return file ? 0 : 3;
        } catch (...) { return 4; }
    }
    Dashboard app(instance, demo, capture, capturePage, preview, scopeDemo);
    return app.run(show);
}
