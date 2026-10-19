#include "dashboard.hpp"
#include "station_settings.hpp"
#include <algorithm>
#include <iostream>
#include <condition_variable>
#include <stdexcept>

struct DashboardChecks {
    static constexpr int TuneVfo = 201, Frequency = 202, Mode = 204, Width = 205, Ptt = 214, PttLatch = 215;
    static inline int checks = 0, modeSelectionWrites = 0;
    static void expect(bool value, const char* label) {
        if (!value) throw std::runtime_error(label);
        ++checks;
    }
    static std::wstring text(HWND window) {
        std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
        value.resize(GetWindowTextW(window, value.data(), static_cast<int>(value.size())));
        return value;
    }
    static LRESULT CALLBACK countSelections(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
        if (message == CB_SETCURSEL) ++modeSelectionWrites;
        return DefSubclassProc(window, message, wp, lp);
    }
    static void mode(Dashboard& app, const wchar_t* name) {
        auto index = SendMessageW(app.controls_[Mode], CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>(name));
        expect(index != CB_ERR, "mode choice exists");
        SendMessageW(app.controls_[Mode], CB_SETCURSEL, index, 0);
        app.command(Mode, CBN_SELCHANGE);
    }
    static void layout(Dashboard& app) {
        std::vector<std::pair<int, RECT>> visible;
        for (auto [id, window] : app.controls_) {
            if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT rect{}; GetWindowRect(window, &rect);
            MapWindowPoints(nullptr, app.window_, reinterpret_cast<POINT*>(&rect), 2);
            OffsetRect(&rect, app.scale(app.scrollX_), app.scale(app.scrollY_));
            expect(rect.left >= 0 && rect.top >= 0 && rect.right <= app.scale(app.canvasWidth_) && rect.bottom <= app.scale(app.canvasHeight_), "visible control is inside scrollable canvas");
            for (const auto& [otherId, other] : visible) {
                RECT overlap{};
                if (IntersectRect(&overlap, &rect, &other)) {
                    std::cerr << "Overlap: " << otherId << " and " << id << '\n';
                    expect(false, "visible controls do not overlap");
                }
            }
            visible.emplace_back(id, rect);
        }
    }
    static void radioRevisions() {
        std::mutex mutex; std::condition_variable ready; radio::Snapshot snapshot;
        radio::Worker worker([&](const radio::Snapshot& value) { std::lock_guard lock(mutex); snapshot = value; ready.notify_all(); }, [](const std::string&) {});
        auto wait = [&](auto predicate) {
            std::unique_lock lock(mutex);
            expect(ready.wait_for(lock, std::chrono::seconds(7), [&] { return predicate(snapshot); }), "Dummy radio command completes");
            return snapshot;
        };
        radio::Command cmd; cmd.action = radio::Action::Connect; cmd.connection.type = radio::ConnectionType::Demo; worker.submit(cmd);
        auto connected = wait([](const auto& s) { return s.online; });
        cmd = {}; cmd.generation = connected.generation; cmd.vfo = RIG_VFO_B;
        cmd.action = radio::Action::Frequency; cmd.number = 7250000; cmd.uiRequest = 100; worker.submit(cmd);
        cmd.action = radio::Action::Mode; cmd.text = "LSB"; cmd.width = 0; cmd.uiRequest = 101; worker.submit(cmd);
        cmd.action = radio::Action::SelectVfo; cmd.uiRequest = 102; worker.submit(cmd);
        auto confirmed = wait([](const auto& s) { return s.frequencyRevision[1] == 100 && s.modeRevision[1] == 101 && s.vfoRevision == 102; });
        expect(confirmed.b.frequencyValid && confirmed.b.frequency == 7250000, "worker acknowledgement includes frequency readback");
        expect(confirmed.b.modeValid && confirmed.b.mode == "LSB" && confirmed.activeVfo == RIG_VFO_B, "worker acknowledgement includes mode and VFO readback");
        cmd.action = radio::Action::StepFrequency; cmd.number = 100; cmd.uiRequest = 103; worker.submit(cmd);
        cmd.number = 200; cmd.uiRequest = 104; worker.submit(cmd);
        confirmed = wait([](const auto& s) { return s.frequencyRevision[1] == 104; });
        expect(confirmed.b.frequency == 7250300, "rapid queued steps accumulate exactly");
        cmd.action = radio::Action::Mode; cmd.text = "invalid-mode"; cmd.uiRequest = 105; worker.submit(cmd);
        confirmed = wait([](const auto& s) { return s.modeRevision[1] == 105 && !s.error.empty(); });
        expect(confirmed.b.mode == "LSB", "failed command acknowledges while keeping confirmed mode");
        worker.stop();
    }
    static int run() {
        station_settings::Fields fields{{"kind","0"},{"endpoint","COM4"},{"baud","38400"},{"stopBits","1"},{"flow","None"},{"poll","750"},{"rfCat2Port","COM5"},{"rfSpan","7"},{"rfCat2Baud","4800"},{"rfWaterfallLevel","-5.0"}};
        expect(station_settings::connection(fields).endpoint == "COM4","shared connection model validates native/browser fields");
        expect(station_settings::scope(fields,true).level == -5.0,"shared scope model validates level and CAT-2 settings");
        auto invalid = fields;invalid["flow"]="invalid";bool rejected=false;try{station_settings::connection(invalid);}catch(const std::exception&){rejected=true;}
        expect(rejected,"invalid connection request is rejected before applying fields");
        invalid=fields;invalid["rfWaterfallLevel"]="-5.25";rejected=false;try{station_settings::scope(invalid);}catch(const std::exception&){rejected=true;}
        expect(rejected,"invalid scope request is rejected before applying fields");
        HINSTANCE instance = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        // An isolated, hidden dashboard: no Worker, serial port, audio streams,
        // sharing server or saved preference writes. Injected radio snapshots
        // exercise the same GUI code as the normal application.
        Dashboard app(instance, false, L"isolated-gui-check", 0, true);
        WNDCLASSW cls{}; cls.hInstance = instance; cls.lpszClassName = L"FT710GuiChecks"; cls.lpfnWndProc = Dashboard::windowProc;
        RegisterClassW(&cls);
        HWND window = CreateWindowW(cls.lpszClassName, L"Offline GUI checks", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL,
            0, 0, 1280, 900, nullptr, nullptr, instance, &app);
        expect(window != nullptr, "dashboard window created");
        radio::Snapshot s;
        s.portOpen = s.online = s.demo = s.canPtt = s.pttKnown = true; s.generation = 7;
        s.a = {14200000, "USB", 2400, true, true}; s.b = {7074000, "PKTUSB", 3000, true, true};
        s.modes = {"USB", "LSB", "CW", "PKTUSB"};
        radio::Setting af; af.id = "level.AF"; af.name = "AF gain"; af.kind = radio::Kind::Level; af.mask = RIG_LEVEL_AF;
        af.type = radio::ValueType::Real; af.readable = af.writable = af.valid = true; af.value = "0.35";
        s.settings.push_back(af);
        auto discovering = s; discovering.settings.clear(); app.update(discovering);
        expect(ListView_GetItemCount(app.controls_[503]) == 0, "settings table starts empty during discovery");
        app.update(s);
        expect(ListView_GetItemCount(app.controls_[503]) == 1, "settings table populates when capabilities arrive within same generation");
        expect(text(app.controls_[116]) == L"CAT sharing: Off", "isolated preview sharing caption matches actual state");
        expect(text(app.controls_[Frequency]) == L"14.200000", "VFO A frequency readback");
        expect(text(app.controls_[Mode]) == L"USB", "VFO A mode readback");
        SetWindowTextW(app.controls_[300], L"80"); app.command(310, BN_CLICKED);
        const auto gainRequest = app.pendingQuick_[0]; app.update(s);
        expect(text(app.controls_[300]) == L"80", "gain submission survives older readback");
        s.settingRevision["level.AF"] = gainRequest; s.settings[0].value = "0.8"; app.update(s);
        expect(text(app.controls_[300]) == L"80" && app.pendingQuick_[0] == 0, "gain command settles to acknowledged readback");
        app.selectVfo(RIG_VFO_B);
        const auto vfoRequest = app.pendingVfo_;
        mode(app, L"LSB");
        const auto modeRequest = app.pendingMode_[1];
        app.update(s); // The pre-command snapshot must not roll the target back.
        expect(SendMessageW(app.controls_[TuneVfo], CB_GETCURSEL, 0, 0) == 1, "pending VFO selection survives older snapshot");
        expect(text(app.controls_[Mode]) == L"LSB", "pending mode survives older snapshot");
        s.activeVfo = RIG_VFO_B; s.vfoRevision = vfoRequest;
        app.update(s);
        expect(text(app.controls_[Mode]) == L"LSB", "VFO acknowledgement does not reset later pending mode");
        s.modeRevision[1] = modeRequest; s.b.mode = "LSB"; s.b.width = 2800;
        app.update(s);
        expect(text(app.controls_[Width]) == L"2800", "acknowledged filter uses actual readback");
        SetWindowTextW(app.controls_[Frequency], L"7.125000");
        app.command(203, BN_CLICKED);
        const auto frequencyRequest = app.pendingFrequency_[1];
        app.update(s);
        expect(text(app.controls_[Frequency]) == L"7.125000", "submitted frequency survives pre-command readback");
        s.frequencyRevision[1] = frequencyRequest; s.b.frequency = 7125000;
        app.update(s);
        expect(text(app.controls_[Frequency]) == L"7.125000", "submitted frequency settles to radio readback");
        mode(app, L"CW");
        s.modeRevision[1] = app.pendingMode_[1]; s.error = "Simulated mode rejection";
        app.update(s);
        expect(text(app.controls_[Mode]) == L"LSB", "failed mode command returns to confirmed mode");
        app.selectVfo(RIG_VFO_A);
        s.vfoRevision = app.pendingVfo_; // Simulated failure leaves radio on B.
        app.update(s);
        expect(SendMessageW(app.controls_[TuneVfo], CB_GETCURSEL, 0, 0) == 1, "failed VFO selection returns to confirmed target");
        SetWindowTextW(app.controls_[Frequency], L"7.199999");
        SetWindowTextW(app.controls_[Width], L"2500");
        s.b.frequency = 7180000; s.b.width = 2700; app.update(s);
        expect(text(app.controls_[Frequency]) == L"7.199999", "typed frequency draft survives polling");
        expect(text(app.controls_[Width]) == L"2500", "typed filter draft survives polling");
        app.syncTuningEditors(true);
        expect(text(app.controls_[Frequency]) == L"7.180000", "reset follows radio frequency");
        SetWindowSubclass(app.controls_[Mode], countSelections, 2, 0);
        modeSelectionWrites = 0;
        for (int i = 0; i < 8; ++i) app.update(s);
        expect(modeSelectionWrites == 0, "unchanged mode is not repeatedly reselected or repainted");
        RemoveWindowSubclass(app.controls_[Mode], countSelections, 2);
        app.pttLatched_ = true; app.pttPending_ = true; app.pttRequested_ = std::chrono::steady_clock::now(); app.update(s);
        expect(text(app.controls_[Ptt]) == L"PTT off" && text(app.controls_[PttLatch]) == L"PTT off", "both PTT buttons stay synchronized");
        app.pttLatched_ = app.pttPending_ = false;
        app.scopeApply_ = [](const auto&,unsigned,unsigned,double) { std::this_thread::sleep_for(std::chrono::milliseconds(150));return std::wstring{}; };
        auto started = std::chrono::steady_clock::now();app.applyRfSettings(L"COM99",7,4800,-5);
        expect(std::chrono::steady_clock::now()-started < std::chrono::milliseconds(100),"slow scope settings run without blocking GUI dispatch");
        app.microphoneWanted_ = true;app.audioSelectedIds_[2] = audio::BrowserMicrophone;
        app.browserMicClients_ = {1,2};app.browserPttOwner_ = 2;
        app.browserMicrophone(1,false);
        expect(app.microphoneWanted_ && app.browserPttOwner_ == 2,"disarming non-owner keeps shared mic route and owner");
        app.pttLatched_ = true;app.browserMicrophone(2,false);
        expect(!app.microphoneWanted_ && !app.browserPttOwner_,"disconnecting final owner releases PTT and mic route");
        // All pages, small/full windows and 100/125/150/200 percent font scales.
        for (unsigned dpi : {96u, 120u, 144u, 192u}) {
            app.dpi_ = dpi;
            for (auto size : {SIZE{1280, 900}, SIZE{800, 540}}) {
                SetWindowPos(window, nullptr, 0, 0, MulDiv(size.cx, dpi, 96), MulDiv(size.cy, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
                for (int page = 0; page < 6; ++page) { app.page_ = page; app.layout(); layout(app); }
                if (size.cx == 800) {
                    RECT client{}; GetClientRect(window, &client);
                    expect(!(GetWindowLongPtrW(window, GWL_STYLE) & (WS_HSCROLL | WS_VSCROLL)), "small window fits without outer scrollbars");
                    expect(app.scale(app.canvasWidth_) <= client.right && app.scale(app.canvasHeight_) <= client.bottom, "scaled canvas fits the client area");
                    app.scrollCanvas(SB_VERT, SB_BOTTOM); app.scrollCanvas(SB_HORZ, SB_BOTTOM);
                    expect(app.scrollX_ == 0 && app.scrollY_ == 0, "fitted canvas stays at the origin");
                }
            }
        }
        app.audioJobs_.stop(); app.scopeJobs_.stop();
        MSG completion{};
        while (PeekMessageW(&completion,window,WM_APP+4,WM_APP+4,PM_REMOVE)) app.handle(completion.message,completion.wParam,completion.lParam);
        DestroyWindow(window); app.window_ = nullptr;
        radioRevisions();
        std::cout << "GUI checks passed: " << checks << " assertions (snapshot ordering, drafts, PTT, all six tabs, resize and DPI).\n";
        return 0;
    }
};
int main() {
    try { return DashboardChecks::run(); }
    catch (const std::exception& error) { std::cerr << "GUI check failed: " << error.what() << '\n'; return 1; }
}
