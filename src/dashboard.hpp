#pragma once
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include "radio.hpp"
#include "cat_server.hpp"
#include "audio.hpp"
#include "scope_view.hpp"
#include "rf_scope.hpp"
#include "web_server.hpp"
#include <memory>
#include <array>
#include <deque>

class Dashboard {
public:
    Dashboard(HINSTANCE instance, bool demo, std::wstring capturePath, int capturePage = 0, bool preview = false, bool scopeDemo = false);
    ~Dashboard();
    int run(int show);
private:
    friend struct DashboardChecks;
    HINSTANCE instance_;
    HWND window_ = nullptr;
    std::unique_ptr<radio::Worker> worker_;
    std::unique_ptr<CatServer> catServer_;
    std::unique_ptr<web::Server> webServer_;
    std::string webAccessCode_;
    std::wstring webError_;
    std::uint64_t browserPttOwner_ = 0;
    ULONGLONG webSpectrumTick_ = 0;
    radio::Snapshot state_;
    std::map<int, HWND> controls_;
    std::vector<std::string> visibleRows_;
    std::vector<std::string> renderedSettingIds_;
    std::vector<std::wstring> activity_;
    int page_ = 0;
    unsigned dpi_ = 96;
    HFONT bodyFont_ = nullptr, smallFont_ = nullptr, titleFont_ = nullptr, frequencyFont_ = nullptr, meterFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr, inputBrush_ = nullptr;
    bool demoAtStart_ = false, updating_ = false, preview_ = false, layingOut_ = false;
    bool frequencyDirty_ = false, widthDirty_ = false, dialDragging_ = false;
    int dialLastX_ = 0, dialMotion_ = 0, wheelRemainder_ = 0;
    std::array<std::string, 2> requestedModes_;
    std::array<std::uint64_t, 2> pendingFrequency_{}, pendingMode_{};
    std::uint64_t nextUiRequest_ = 0, pendingVfo_ = 0;
    int canvasWidth_ = 1104, canvasHeight_ = 860, scrollX_ = 0, scrollY_ = 0;
    HDWP layoutBatch_ = nullptr;
    std::map<int, unsigned> buttonVisuals_;
    ULONGLONG clockTick_ = 0;
    bool pttLatched_ = false, pttPending_ = false;
    std::chrono::steady_clock::time_point pttRequested_{};
    bool sharingWanted_ = true;
    std::wstring sharingError_;
    std::array<bool, 6> quickDirty_{};
    std::array<bool, 6> quickLoaded_{};
    std::array<std::uint64_t, 6> pendingQuick_{};
    std::vector<audio::Device> audioDevices_;
    std::array<std::vector<std::wstring>, 4> audioDeviceIds_;
    std::array<std::wstring, 4> audioSelectedIds_;
    std::array<std::wstring, 4> audioSelectedNames_;
    audio::Route receiveAudio_, microphoneAudio_;
    ScopeView scope_;
    rf_scope::Capture rfCapture_;
    rf_scope::Frame rfFrame_{};
    std::deque<std::array<std::uint8_t, rf_scope::BinCount>> rfHistory_;
    std::wstring rfUiStatus_ = L"RF scope capture is off";
    unsigned rfLastSpan_ = 7;
    std::uint64_t rfRenderedCenterHz_ = 0;
    std::uint64_t rfRenderedSequence_ = 0;
    bool scopeDemo_ = false;
    std::uint64_t demoSample_ = 0;
    std::uint32_t demoNoise_ = 42;
    bool receiveWanted_ = false, microphoneWanted_ = false;
    std::wstring audioError_;
    std::wstring capturePath_, iniPath_;
    std::uint64_t renderedGeneration_ = 0;
    void createControls();
    HWND control(int id, const wchar_t* cls, const wchar_t* text, DWORD style, int page = -1);
    void fonts();
    void layout();
    void scrollCanvas(int bar, UINT code);
    void invalidateArea(int x, int y, int width, int height);
    void invalidateLive();
    bool buttonSelected(int id) const;
    void refreshButton(int id);
    void paint(HDC dc);
    void drawButton(const DRAWITEMSTRUCT& item);
    void update(const radio::Snapshot& state);
    void rebuildRows();
    void updateRows();
    void log(const std::wstring& text);
    std::uint64_t send(radio::Command command);
    void command(int id, int notification);
    void syncTuningEditors(bool reset = false);
    void selectVfo(vfo_t vfo);
    void tuneBySteps(int steps);
    bool tuningWheel(WPARAM, LPARAM);
    void connect();
    void startSharing();
    void editSelected();
    void exportCsv();
    void releasePtt();
    void refreshAudioDevices();
    void updateAudio();
    void updateScope();
    void updateRfScope();
    bool setRfCapture(bool enabled);
    void startWeb();
    void openWeb();
    void editWebPasscode();
    void publishWeb(bool urgent = false);
    void webCommand(const std::shared_ptr<web::Request>& request);
    void stopAudio();
    void toggleAudio(bool microphone);
    void savePreferences();
    void loadPreferences();
    void capture();
    void move(int id, int x, int y, int width, int height);
    int scale(int value) const;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK tuningProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK buttonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT, WPARAM, LPARAM);
};
