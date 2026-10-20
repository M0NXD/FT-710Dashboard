#include "dashboard.hpp"
#include "dashboard_ids.hpp"
#include "station_settings.hpp"
#include <dwmapi.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wtsapi32.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <string_view>
#include <tuple>

namespace {
using namespace dashboard_id;
constexpr COLORREF Background = RGB(11, 17, 25), Panel = RGB(19, 28, 40), Input = RGB(26, 38, 53);
constexpr COLORREF Border = RGB(42, 57, 75), Text = RGB(232, 240, 249), Muted = RGB(141, 162, 184);
constexpr COLORREF Accent = RGB(80, 211, 194), Blue = RGB(112, 175, 255), Red = RGB(255, 118, 128), Amber = RGB(255, 193, 101);
constexpr UINT UpdateMessage = WM_APP + 1, LogMessage = WM_APP + 2;
constexpr int CanvasMinimumWidth = 1104, CanvasMinimumHeight = 860;
constexpr std::array<int, 4> scopeSpans{3000, 6000, 12000, 24000};
constexpr std::array<int, 7> scopeGains{-20, 0, 10, 20, 30, 40, 60};
constexpr std::array<int, 4> scopeFloors{-60, -80, -100, -120};
constexpr std::array<double, 8> tuningSteps{10, 100, 500, 1000, 2500, 5000, 10000, 100000};
const std::vector<std::string> quickIds{"level.AF", "level.RF", "level.RFPOWER", "level.MICGAIN", "level.SQL", "level.IF"};
const std::vector<std::string> quickLabels{"AF gain (%)", "RF gain (%)", "RF power (W)", "Mic gain (%)", "Squelch (%)", "IF shift (Hz)"};
const std::vector<std::string> functionIds{"func.ANF", "func.APF", "func.MON", "func.COMP", "func.VOX", "func.LOCK", "func.TUNER"};
const std::vector<std::wstring> functionLabels{L"Auto notch", L"APF", L"Monitor", L"Processor", L"VOX", L"Dial lock", L"Tuner"};
const int TunerButton = Function + static_cast<int>(std::find(functionIds.begin(), functionIds.end(), "func.TUNER") - functionIds.begin());
constexpr UINT_PTR TunerClickTimer = 4;
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring textOf(HWND window) {
    std::wstring result(GetWindowTextLengthW(window) + 1, 0);
    int count = GetWindowTextW(window, result.data(), static_cast<int>(result.size()));
    result.resize(count);
    return result;
}
void textIfChanged(HWND window, const std::wstring& text) {
    if (textOf(window) != text) {
        SetWindowTextW(window, text.c_str());
        InvalidateRect(window, nullptr, FALSE);
    }
}
void combo(HWND window, const std::vector<std::wstring>& values, int selected = 0) {
    SendMessageW(window, CB_RESETCONTENT, 0, 0);
    for (const auto& value : values) SendMessageW(window, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
    SendMessageW(window, CB_SETCURSEL, selected, 0);
}
int selected(HWND window) { return static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0)); }
void selectionIfChanged(HWND window, int index) {
    if (selected(window) != index) SendMessageW(window, CB_SETCURSEL, index, 0);
}
int discreteLevel(const radio::Setting* setting, int maximum, bool normalized) {
    if (!setting || !setting->valid || maximum <= 0) return -1;
    try {
        const double value = std::stod(setting->value);
        if (!std::isfinite(value)) return -1;
        return std::clamp(static_cast<int>(std::lround(value * (normalized ? maximum : 1))), 0, maximum);
    } catch (...) { return -1; }
}
std::wstring preampName(const radio::Setting* setting) {
    if (!setting || !setting->valid) return L"--";
    if (setting->value == "0") return L"IPO";
    if (setting->value == "10") return L"AMP1";
    if (setting->value == "20") return L"AMP2";
    return wide(setting->value);
}
std::wstring noiseReductionName(const radio::Setting* function, const radio::Setting* level, int maximum, bool normalized) {
    if (!function || !function->valid) return L"--";
    if (function->value == "0") return L"Off";
    const int amount = discreteLevel(level, maximum, normalized);
    return amount < 0 ? L"On" : std::to_wstring(amount);
}
double numberOf(HWND window) {
    auto text = narrow(textOf(window));
    std::size_t count = 0;
    double result = std::stod(text, &count);
    if (count != text.size() || !std::isfinite(result)) throw std::invalid_argument("Enter a number without units.");
    return result;
}
double frequencyHzOf(HWND window) {
    std::wstring input = textOf(window);
    input.erase(std::remove_if(input.begin(), input.end(), [](wchar_t c) { return std::iswspace(c) != 0; }), input.end());
    std::transform(input.begin(), input.end(), input.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    double multiplier = 0;
    auto removeSuffix = [&](const wchar_t* suffix, double scale) {
        const std::wstring unit = suffix;
        if (input.size() >= unit.size() && input.compare(input.size() - unit.size(), unit.size(), unit) == 0) {
            input.resize(input.size() - unit.size()); multiplier = scale; return true;
        }
        return false;
    };
    if (!removeSuffix(L"mhz", 1000000.0) && !removeSuffix(L"khz", 1000.0)) removeSuffix(L"hz", 1.0);
    const auto separators = std::count_if(input.begin(), input.end(), [](wchar_t c) { return c == L'.' || c == L','; });
    if (separators > 1) input.erase(std::remove_if(input.begin(), input.end(), [](wchar_t c) { return c == L'.' || c == L','; }), input.end());
    else std::replace(input.begin(), input.end(), L',', L'.');
    std::size_t consumed = 0;
    double value = 0;
    try { value = std::stod(input, &consumed); }
    catch (...) { throw std::invalid_argument("Enter 7.074 MHz, 7074 kHz, or 7074000 Hz."); }
    if (consumed != input.size() || !std::isfinite(value)) throw std::invalid_argument("Enter 7.074 MHz, 7074 kHz, or 7074000 Hz.");
    if (multiplier == 0) multiplier = value <= 60.0 ? 1000000.0 : value <= 60000.0 ? 1000.0 : 1.0;
    const double hz = std::round(value * multiplier);
    if (hz < 30000.0 || hz > 60000000.0) throw std::invalid_argument("Frequency must be between 0.030 and 60 MHz.");
    return hz;
}
std::wstring frequencyText(const radio::Vfo& vfo) {
    if (!vfo.frequencyValid) return L"--.---.---";
    auto hz = static_cast<long long>(std::llround(vfo.frequency));
    std::wostringstream text;
    text << hz / 1000000 << L"." << std::setw(3) << std::setfill(L'0') << (hz / 1000) % 1000 << L"." << std::setw(3) << hz % 1000;
    return text.str();
}
std::wstring frequencyInput(double hz) {
    std::wostringstream text;
    text << std::fixed << std::setprecision(6) << hz / 1000000;
    return text.str();
}
std::wstring meterNumber(double value) {
    return wide(radio::formatNumber(std::round(value * 10) / 10));
}
std::wstring bandText(double frequency) {
    if (frequency >= 1800000 && frequency <= 2000000) return L"160 m";
    if (frequency >= 3500000 && frequency <= 4000000) return L"80 m";
    if (frequency >= 5250000 && frequency <= 5450000) return L"60 m";
    if (frequency >= 7000000 && frequency <= 7300000) return L"40 m";
    if (frequency >= 10100000 && frequency <= 10150000) return L"30 m";
    if (frequency >= 14000000 && frequency <= 14350000) return L"20 m";
    if (frequency >= 18068000 && frequency <= 18168000) return L"17 m";
    if (frequency >= 21000000 && frequency <= 21450000) return L"15 m";
    if (frequency >= 24890000 && frequency <= 24990000) return L"12 m";
    if (frequency >= 28000000 && frequency <= 29700000) return L"10 m";
    if (frequency >= 50000000 && frequency <= 54000000) return L"6 m";
    return L"General coverage";
}
void fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
void card(HDC dc, RECT rect, COLORREF color = Panel, COLORREF edge = Border, int radius = 12) {
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HBRUSH brush = CreateSolidBrush(color);
    auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, brush);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
}
void drawText(HDC dc, HFONT font, COLORREF color, RECT rect, const std::wstring& value, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
std::wstring utc() {
    SYSTEMTIME time{}; GetSystemTime(&time);
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%02d:%02d:%02d UTC", time.wHour, time.wMinute, time.wSecond);
    return buffer;
}
std::string csv(const std::string& text) {
    std::string result = "\"";
    for (char c : text) { if (c == '"') result += '"'; result += c; }
    return result + "\"";
}

// The setting editor uses a copied descriptor so background refreshes cannot alter an edit.
struct Editor {
    HWND window = nullptr, value = nullptr, owner = nullptr;
    radio::Setting setting;
    HFONT font = nullptr;
    bool finished = false, accepted = false;
    std::string result;
    static LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = window;
        }
        if (!self) return DefWindowProcW(window, msg, wp, lp);
        if (msg == WM_CREATE) {
            unsigned dpi = GetDpiForWindow(window);
            auto s = [&](int n) { return MulDiv(n, dpi, 96); };
            self->font = CreateFontW(-s(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            auto make = [&](const wchar_t* cls, const std::wstring& text, DWORD style, int id, int x, int y, int w, int h) {
                HWND child = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, s(x), s(y), s(w), s(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->font), TRUE);
                return child;
            };
            make(L"STATIC", wide(self->setting.name), 0, 10, 22, 18, 474, 24);
            make(L"STATIC", wide(radio::rangeText(self->setting)), 0, 11, 22, 50, 474, 48);
            bool choice = self->setting.type == radio::ValueType::Boolean || !self->setting.choices.empty();
            self->value = make(choice ? L"COMBOBOX" : L"EDIT", wide(self->setting.value), WS_TABSTOP |
                (choice ? CBS_DROPDOWNLIST | WS_VSCROLL : ES_AUTOHSCROLL | WS_BORDER), 12, 22, 110, 474, choice ? 240 : 32);
            if (choice) {
                std::vector<std::wstring> options;
                if (self->setting.type == radio::ValueType::Boolean) options = {L"Off", L"On"};
                else for (const auto& item : self->setting.choices) options.push_back(wide(item));
                int selection = 0;
                if (self->setting.type == radio::ValueType::Boolean) selection = self->setting.value == "1" ? 1 : 0;
                else for (std::size_t i = 0; i < self->setting.choiceValues.size(); ++i) if (self->setting.choiceValues[i] == self->setting.value) selection = static_cast<int>(i);
                combo(self->value, options, selection);
            }
            make(L"STATIC", L"The value is sent to the selected VFO and read back when supported.", 0, 13, 22, 156, 474, 44);
            make(L"BUTTON", L"Apply", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 294, 216, 96, 34);
            make(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, 402, 216, 94, 34);
            SetFocus(self->value);
            return 0;
        }
        if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
            if (self->setting.type == radio::ValueType::Boolean) self->result = selected(self->value) == 1 ? "1" : "0";
            else if (!self->setting.choices.empty()) {
                int index = selected(self->value);
                if (index < 0 || static_cast<std::size_t>(index) >= self->setting.choiceValues.size()) return 0;
                self->result = self->setting.choiceValues[index];
            } else self->result = narrow(textOf(self->value));
            self->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (msg == WM_DESTROY) { self->finished = true; if (self->font) DeleteObject(self->font); return 0; }
        return DefWindowProcW(window, msg, wp, lp);
    }
    bool show(HWND parent, HINSTANCE instance) {
        owner = parent;
        WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = instance; cls.lpszClassName = L"FT710SettingEditor";
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        RECT parentRect{}; GetWindowRect(parent, &parentRect);
        unsigned dpi = GetDpiForWindow(parent);
        int width = MulDiv(536, dpi, 96), height = MulDiv(302, dpi, 96);
        HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"Edit radio setting", WS_POPUP | WS_CAPTION | WS_SYSMENU,
            parentRect.left + (parentRect.right - parentRect.left - width) / 2, parentRect.top + 120, width, height, parent, nullptr, instance, this);
        if (!dialog) return false;
        EnableWindow(parent, FALSE);
        ShowWindow(dialog, SW_SHOW);
        MSG message{};
        while (!finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        EnableWindow(parent, TRUE); SetForegroundWindow(parent);
        return accepted;
    }
};
}

Dashboard::Dashboard(HINSTANCE instance, bool demo, std::wstring capturePath, int capturePage, bool preview, bool scopeDemo)
    : instance_(instance), page_(capturePath.empty() ? (scopeDemo ? 4 : 0) : capturePage), demoAtStart_(demo), preview_(preview), scopeDemo_(scopeDemo), capturePath_(std::move(capturePath)) {
    receiveAudio_.receivedAudio(true);
    backgroundBrush_ = CreateSolidBrush(Background);
    inputBrush_ = CreateSolidBrush(Input);
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        const auto stationFolder = std::filesystem::path(path) / L"M0NXD";
        auto folder = stationFolder / L"FT710Dashboard";
        std::filesystem::create_directories(folder);
        iniPath_ = (folder / L"settings.ini").wstring();
        // A previous RF-scope build used its own settings directory. Preserve
        // those user preferences when bringing both builds into one app.
        const auto priorIni = stationFolder / L"FT710DashboardRF" / L"settings.ini";
        std::error_code fileError;
        if (std::filesystem::exists(priorIni, fileError) && !std::filesystem::exists(iniPath_, fileError)) {
            std::filesystem::copy_file(priorIni, iniPath_, std::filesystem::copy_options::none, fileError);
        } else if (std::filesystem::exists(priorIni, fileError)) {
            std::array<wchar_t, 64> marker{};
            GetPrivateProfileStringW(L"RfScope", L"Cat2Port", L"__FT710_MISSING__", marker.data(),
                                     static_cast<DWORD>(marker.size()), iniPath_.c_str());
            if (std::wstring_view(marker.data()) == L"__FT710_MISSING__") {
                for (const wchar_t* key : {L"Span", L"Cat2Port", L"Cat2Baud", L"WaterfallLevelDb"}) {
                    std::array<wchar_t, 256> value{};
                    const auto length = GetPrivateProfileStringW(L"RfScope", key, L"", value.data(),
                                                                 static_cast<DWORD>(value.size()), priorIni.c_str());
                    if (length) WritePrivateProfileStringW(L"RfScope", key, value.data(), iniPath_.c_str());
                }
            }
        }
        CoTaskMemFree(path);
    }
}
Dashboard::~Dashboard() {
    audioJobs_.stop(); scopeJobs_.stop();
    stopAudio();
    rfCapture_.stop();
    if (webServer_) webServer_->stop();
    if (catServer_) catServer_->stop();
    if (worker_) worker_->stop();
    for (HFONT font : {bodyFont_, smallFont_, titleFont_, frequencyFont_, meterFont_}) if (font) DeleteObject(font);
    DeleteObject(backgroundBrush_); DeleteObject(inputBrush_);
}
int Dashboard::scale(int value) const { return static_cast<int>(std::lround(value * uiScale_)); }
double Dashboard::unscale(int value) const { return value / uiScale_; }
void Dashboard::fonts() {
    const std::array<int, 5> pixels{std::max(1, scale(14)), std::max(1, scale(12)),
        std::max(1, scale(27)), std::max(1, scale(39)), std::max(1, scale(25))};
    if (bodyFont_ && pixels == fontPixels_) return;
    fontPixels_ = pixels;
    const std::array<HFONT, 5> oldFonts{bodyFont_, smallFont_, titleFont_, frequencyFont_, meterFont_};
    auto make = [&](int pixels, int weight, const wchar_t* face) {
        return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    bodyFont_ = make(pixels[0], FW_NORMAL, L"Segoe UI");
    smallFont_ = make(pixels[1], FW_NORMAL, L"Segoe UI");
    titleFont_ = make(pixels[2], FW_SEMIBOLD, L"Segoe UI");
    frequencyFont_ = make(pixels[3], FW_SEMIBOLD, L"Consolas");
    meterFont_ = make(pixels[4], FW_SEMIBOLD, L"Segoe UI");
    for (auto [id, window] : controls_) { (void)id; SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), FALSE); }
    for (HFONT font : oldFonts) if (font) DeleteObject(font);
}
int Dashboard::run(int show) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&init);
    WNDCLASSEXW cls{}; cls.cbSize = sizeof cls; cls.hInstance = instance_;
    cls.lpfnWndProc = windowProc; cls.lpszClassName = L"FT710Dashboard";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = backgroundBrush_;
    RegisterClassExW(&cls);
    dpi_ = GetDpiForSystem();
    uiScale_ = dpi_ / 96.0;
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT initial{0, 0, scale(1280), scale(CanvasMinimumHeight)};
    AdjustWindowRectExForDpi(&initial, style, FALSE, 0, dpi_);
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(monitor, &monitorInfo)) workArea = monitorInfo.rcWork;
    const int workWidth = static_cast<int>(workArea.right - workArea.left);
    const int workHeight = static_cast<int>(workArea.bottom - workArea.top);
    const int availableWidth = std::max(1, workWidth - scale(32));
    const int availableHeight = std::max(1, workHeight - scale(32));
    const int windowWidth = std::min(static_cast<int>(initial.right - initial.left), availableWidth);
    const int windowHeight = std::min(static_cast<int>(initial.bottom - initial.top), availableHeight);
    const int windowX = static_cast<int>(workArea.left) + (workWidth - windowWidth) / 2;
    const int windowY = static_cast<int>(workArea.top) + (workHeight - windowHeight) / 2;
    window_ = CreateWindowExW(0, cls.lpszClassName, L"FT-710 Dashboard | M0NXD", style,
        windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, instance_, this);
    if (!window_) { if (SUCCEEDED(apartment)) CoUninitialize(); return 1; }
    worker_ = std::make_unique<radio::Worker>(
        [this](const radio::Snapshot& state) {
            auto* copy = new radio::Snapshot(state);
            if (!PostMessageW(window_, UpdateMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        },
        [this](const std::string& text) {
            auto* copy = new std::wstring(wide(text));
            if (!PostMessageW(window_, LogMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        });
    catServer_ = std::make_unique<CatServer>(
        [this](std::shared_ptr<radio::CatJob> job) { return worker_->request(std::move(job)); },
        [this](const std::string& text) {
            auto* copy = new std::wstring(wide(text));
            if (!PostMessageW(window_, LogMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        });
    if (capturePath_.empty()) startWeb();
    ShowWindow(window_, capturePath_.empty() && !preview_ ? show : SW_SHOWNOACTIVATE);
    UpdateWindow(window_);
    SetTimer(window_, 1, 250, nullptr);
    SetTimer(window_, 3, 50, nullptr);
    if (demoAtStart_) {
        SendMessageW(controls_[ConnectionKind], CB_SETCURSEL, 2, 0);
        command(ConnectionKind, CBN_SELCHANGE);
        connect();
    }
    if (!capturePath_.empty()) SetTimer(window_, 2, 4500, nullptr);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_MOUSEWHEEL && tuningWheel(msg.wParam, msg.lParam)) continue;
        if (!IsDialogMessageW(window_, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
HWND Dashboard::control(int id, const wchar_t* cls, const wchar_t* text, DWORD style, int page) {
    DWORD extra = std::wstring(cls) == L"EDIT" ? WS_EX_CLIENTEDGE : 0;
    HWND child = CreateWindowExW(extra, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        0, 0, 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    controls_[id] = child;
    SetPropW(child, L"Page", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(page + 2)));
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    return child;
}
void Dashboard::createControls() {
    fonts();
    auto dropdown = [&](int id, int page = -1) { return control(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, page); };
    auto button = [&](int id, const wchar_t* text, int page = -1) {
        HWND child = control(id, L"BUTTON", text, BS_OWNERDRAW, page);
        SetWindowSubclass(child, buttonProc, 2, reinterpret_cast<DWORD_PTR>(this));
        return child;
    };
    combo(dropdown(ConnectionKind), {L"FT-710 / USB CAT", L"Hamlib / rigctld", L"Demo / Hamlib Dummy"});
    control(Endpoint, L"EDIT", L"COM3", ES_AUTOHSCROLL);
    combo(dropdown(Baud), {L"4800", L"9600", L"19200", L"38400", L"115200"}, 3);
    combo(dropdown(StopBits), {L"1", L"2"});
    combo(dropdown(Flow), {L"None", L"Hardware", L"XONXOFF"});
    combo(dropdown(Poll), {L"500", L"750", L"1000", L"1500", L"2000"}, 1);
    button(Connect, L"Connect");
    button(ShareCat, L"CAT sharing: On");
    button(OpenWeb, L"Web: 8888");
    button(WebPasscode, L"Set passcode");
    control(SharePort, L"EDIT", L"4532", ES_NUMBER | ES_AUTOHSCROLL);
    SendMessageW(controls_[SharePort], EM_SETLIMITTEXT, 5, 0);
    button(PowerOn, L"Power on"); button(PowerOff, L"Power off");
    button(PttLatch, L"PTT on");
    button(TabOverview, L"Dashboard"); button(TabSettings, L"All settings"); button(TabActivity, L"Activity / CAT"); button(TabAudio, L"Audio routing");
    button(TabScope, L"Audio scope"); button(TabRfScope, L"RF scope");
    button(Export, L"Export CSV"); button(Refresh, L"Refresh");
    combo(dropdown(TuneVfo, 0), {L"VFO A", L"VFO B"});
    control(Frequency, L"EDIT", L"", ES_AUTOHSCROLL, 0);
    SendMessageW(controls_[Frequency], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"MHz, kHz, or Hz"));
    button(Tune, L"Go", 0);
    dropdown(Mode, 0); control(Width, L"EDIT", L"", ES_AUTOHSCROLL, 0); button(SetMode, L"Set filter", 0);
    SendMessageW(controls_[Width], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"0 = normal"));
    combo(dropdown(Step, 0), {L"10 Hz", L"100 Hz", L"500 Hz", L"1 kHz", L"2.5 kHz", L"5 kHz", L"10 kHz", L"100 kHz"}, 1);
    button(Down, L"- Down", 0); button(Up, L"+ Up", 0);
    button(TuningDial, L"Drag left / right to tune", 0);
    for (int id : {Frequency, Width, TuningDial, Down, Up})
        SetWindowSubclass(controls_[id], tuningProc, 1, reinterpret_cast<DWORD_PTR>(this));
    button(VfoA, L"Use VFO A", 0); button(VfoB, L"Use VFO B", 0);
    button(Swap, L"Swap A/B", 0); button(Copy, L"Copy active VFO", 0);
    button(Ptt, L"PTT on", 0);
    for (int i = 0; i < static_cast<int>(quickIds.size()); ++i) {
        control(QuickEdit + i, L"EDIT", L"", ES_AUTOHSCROLL, 0);
        button(QuickSet + i, L"Set", 0);
    }
    button(CyclePreamp, L"Preamp: IPO", 0); button(CycleDnr, L"DNR: Off", 0); button(CycleNb, L"NB: Off", 0);
    for (int i = 0; i < static_cast<int>(functionLabels.size()); ++i) button(Function + i, functionLabels[i].c_str(), 0);
    HWND tunerTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    if (tunerTip) {
        TOOLINFOW tip{}; tip.cbSize = sizeof tip; tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tip.hwnd = window_; tip.uId = reinterpret_cast<UINT_PTR>(controls_[TunerButton]);
        tip.lpszText = const_cast<wchar_t*>(L"Single click: tuner on/off. Double-click: start antenna tuning (transmits a tuning carrier).");
        SendMessageW(tunerTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
        SendMessageW(tunerTip, TTM_SETMAXTIPWIDTH, 0, 360);
    }
    control(Filter, L"EDIT", L"", ES_AUTOHSCROLL, 1);
    SendMessageW(controls_[Filter], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter by setting, group or Hamlib name"));
    combo(dropdown(SettingsVfo, 1), {L"Current VFO", L"VFO A", L"VFO B"});
    button(EditSetting, L"Edit selected", 1);
    HWND list = control(SettingsList, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 1);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(list, Panel); ListView_SetTextBkColor(list, Panel); ListView_SetTextColor(list, Text);
    const wchar_t* columns[]{L"Setting", L"Group", L"Value", L"Access", L"Range / choices", L"Read status"};
    int widths[]{220, 145, 150, 90, 250, 185};
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH; col.pszText = const_cast<wchar_t*>(columns[i]); col.cx = scale(widths[i]);
        ListView_InsertColumn(list, i, &col);
    }
    control(Activity, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 2);
    control(CatCommand, L"EDIT", L"ID;", ES_AUTOHSCROLL, 2);
    button(CatQuery, L"Query / read reply", 2); button(CatWrite, L"Send / no reply", 2);
    combo(dropdown(CatPreset, 2), {L"ID;  Radio identity", L"VE;  Firmware version", L"RI0;  Radio status", L"DA;  Dimmer", L"AS1;  AESS level", L"SS00;  Scope status", L"EX;  Menu (supply menu ID)"});
    button(ClearLog, L"Clear log", 2); button(Manual, L"FT-710 CAT manual", 2);
    for (int i = 0; i < 4; ++i) dropdown(AudioDevice + i, 3);
    button(AudioRefresh, L"Refresh devices", 3);
    button(AudioReceive, L"Receive audio: Off", 3); button(AudioMicrophone, L"Microphone: Off", 3);
    control(AudioReceiveGain, L"EDIT", L"100", ES_NUMBER | ES_AUTOHSCROLL, 3);
    control(AudioMicrophoneGain, L"EDIT", L"100", ES_NUMBER | ES_AUTOHSCROLL, 3);
    combo(dropdown(AudioPttLimit, 3), {L"30 seconds", L"60 seconds", L"180 seconds", L"300 seconds", L"600 seconds"}, 2);
    button(AudioUnattended,L"Unattended browser audio: Off",3);
    combo(dropdown(ScopeSpan, 4), {L"0 - 3 kHz", L"0 - 6 kHz", L"0 - 12 kHz", L"0 - 24 kHz"}, 1);
    combo(dropdown(ScopeGain, 4), {L"-20 dB", L"0 dB", L"+10 dB", L"+20 dB", L"+30 dB", L"+40 dB", L"+60 dB"}, 1);
    combo(dropdown(ScopeFloor, 4), {L"-60 dB", L"-80 dB", L"-100 dB", L"-120 dB"}, 2);
    button(ScopeFreeze, L"Freeze", 4); button(ScopePeak, L"Peak hold: Off", 4); button(ScopeClear, L"Clear", 4);
    button(ScopeReceive, L"Receive audio: Off", 4); button(ScopeRouting, L"Audio mappings", 4);
    combo(dropdown(RfSpan, 5), {L"1 kHz", L"2 kHz", L"5 kHz", L"10 kHz", L"20 kHz", L"50 kHz", L"100 kHz", L"200 kHz", L"500 kHz", L"1 MHz"}, 7);
    control(RfCat2Port, L"EDIT", L"", ES_AUTOHSCROLL, 5);
    SendMessageW(controls_[RfCat2Port], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"CAT-2 COM port, e.g. COM5"));
    combo(dropdown(RfCat2Baud, 5), {L"4800", L"9600", L"19200", L"38400", L"115200"}, 0);
    control(RfWaterfallLevel, L"EDIT", L"-5.0", ES_AUTOHSCROLL, 5);
    SendMessageW(controls_[RfWaterfallLevel], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"-30.0 to +30.0, 0.5 steps"));
    button(RfApplySpan, L"Apply to radio", 5); button(RfCapture, L"Start RF capture", 5); button(RfClear, L"Clear waterfall", 5);
    loadPreferences();
    refreshAudioDevices();
    if (!capturePath_.empty() || preview_) sharingWanted_ = false;
}
void Dashboard::move(int id, int x, int y, int width, int height) {
    if (y >= 175 && id != ShareCat && id != SharePort && id != OpenWeb && id != WebPasscode) y += 48;
    width = scale(x - scrollX_ + width) - scale(x - scrollX_);
    height = scale(y - scrollY_ + height) - scale(y - scrollY_);
    x = scale(x - scrollX_); y = scale(y - scrollY_);
    RECT old{}; GetWindowRect(controls_[id], &old); MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&old), 2);
    // Native combo boxes keep a separate collapsed height. Fit that as well
    // as the popup so a smaller font cannot leave a tall control over the next row.
    wchar_t className[16]{}; GetClassNameW(controls_[id], className, 16);
    if (CompareStringOrdinal(className, -1, L"COMBOBOX", -1, TRUE) == CSTR_EQUAL) {
        int current = static_cast<int>(SendMessageW(controls_[id], CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
        if (current != CB_ERR) {
            int chrome = std::max(0, static_cast<int>(old.bottom - old.top) - current);
            int itemHeight = std::max(1, scale(30) - chrome);
            if (current != itemHeight) {
                SendMessageW(controls_[id], CB_SETITEMHEIGHT, 0, itemHeight);
                SendMessageW(controls_[id], CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), itemHeight);
            }
        }
    }
    if (old.left == x && old.top == y && old.right - old.left == width && old.bottom - old.top == height) return;
    if (layoutBatch_) layoutBatch_ = DeferWindowPos(layoutBatch_, controls_[id], nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(controls_[id], nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Dashboard::layout() {
    if (layingOut_ || controls_.empty() || IsIconic(window_)) return;
    layingOut_ = true;
    // Fit the minimum logical layout uniformly inside the actual client area.
    // Keep physical monitor DPI separate: RDP can change both independently.
    ShowScrollBar(window_, SB_BOTH, FALSE);
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 1, 0, 0};
    SetScrollInfo(window_, SB_HORZ, &scroll, FALSE); SetScrollInfo(window_, SB_VERT, &scroll, FALSE);
    RECT rect{};
    GetClientRect(window_, &rect);
    if (rect.right <= 0 || rect.bottom <= 0) { layingOut_ = false; return; }
    uiScale_ = std::min({dpi_ / 96.0, static_cast<double>(rect.right) / CanvasMinimumWidth,
        static_cast<double>(rect.bottom) / CanvasMinimumHeight});
    fonts();
    canvasWidth_ = std::max(CanvasMinimumWidth, static_cast<int>(std::floor(unscale(rect.right))));
    canvasHeight_ = std::max(CanvasMinimumHeight, static_cast<int>(std::floor(unscale(rect.bottom))));
    scrollX_ = scrollY_ = 0;
    int width = canvasWidth_, height = canvasHeight_ - 48;
    layoutBatch_ = BeginDeferWindowPos(static_cast<int>(controls_.size()));
    move(PowerOn, width - 230, 28, 96, 35); move(PowerOff, width - 124, 28, 96, 35);
    move(PttLatch, width - 340, 28, 100, 35);
    move(ConnectionKind, 40, 120, 190, 260); move(Endpoint, 242, 120, 180, 30);
    move(Baud, 434, 120, 96, 220); move(StopBits, 542, 120, 64, 180);
    move(Flow, 618, 120, 112, 220); move(Poll, 742, 120, 90, 220);
    move(Connect, 846, 119, 118, 32);
    move(ShareCat, 40, 177, 172, 31); move(SharePort, 293, 178, 76, 29);
    move(OpenWeb, width - 172, 177, 132, 31);
    move(WebPasscode, width - 312, 177, 128, 31);
    move(TabOverview, 28, 175, 120, 34); move(TabSettings, 158, 175, 120, 34); move(TabActivity, 288, 175, 130, 34);
    move(TabAudio, 428, 175, 112, 34);
    move(TabScope, 550, 175, 112, 34); move(TabRfScope, 672, 175, 100, 34);
    move(Export, width - 264, 175, 118, 34); move(Refresh, width - 134, 175, 106, 34);
    if (page_ != 0 && GetCapture() == controls_[TuningDial]) ReleaseCapture();
    // Two tuning rows provide a touch drag strip and larger step buttons.
    int half = (width - 72) / 2;
    move(VfoA, 48, 305, 108, 28); move(VfoB, 64 + half, 305, 108, 28);
    move(Swap, width - 264, 305, 90, 28); move(Copy, width - 164, 305, 116, 28);
    move(TuneVfo, 40, 375, 104, 230); move(Frequency, 158, 375, 136, 31); move(Tune, 304, 375, 68, 31);
    move(Mode, 408, 375, 116, 290); move(Width, 538, 375, 86, 31); move(SetMode, 634, 375, 100, 31);
    move(Ptt, width - 154, 375, 114, 31);
    move(Down, 40, 410, 84, 32); move(TuningDial, 136, 410, width - 400, 32);
    move(Step, width - 250, 410, 116, 290); move(Up, width - 124, 410, 84, 32);
    int third = (width - 120) / 3;
    for (int i = 0; i < 6; ++i) {
        int x = 44 + (i % 3) * (third + 16), y = 608 + (i / 3) * 44;
        move(QuickEdit + i, x + third - 146, y, 84, 29);
        move(QuickSet + i, x + third - 52, y, 48, 29);
    }
    int buttonWidth = (width - 80) / 10;
    move(CyclePreamp, 32, 707, buttonWidth, 31);
    move(CycleDnr, 32 + buttonWidth + 2, 707, buttonWidth, 31);
    move(CycleNb, 32 + 2 * (buttonWidth + 2), 707, buttonWidth, 31);
    for (int i = 0; i < static_cast<int>(functionIds.size()); ++i)
        move(Function + i, 32 + (i + 3) * (buttonWidth + 2), 707, buttonWidth, 31);
    move(Filter, 32, 228, width - 400, 32); move(SettingsVfo, width - 354, 228, 156, 240); move(EditSetting, width - 184, 228, 152, 32);
    move(SettingsList, 32, 277, width - 64, std::max(120, height - 364));
    move(Activity, 32, 280, width - 64, std::max(90, height - 507));
    move(CatPreset, 32, height - 200, 290, 290); move(CatCommand, 338, height - 200, width - 706, 31);
    move(CatQuery, width - 350, height - 200, 152, 31); move(CatWrite, width - 184, height - 200, 152, 31);
    move(Manual, 32, height - 126, 182, 31); move(ClearLog, width - 144, 228, 112, 31);
    int audioHalf = (width - 112) / 2;
    move(AudioRefresh, width - 190, 228, 158, 31);
    move(AudioDevice, 48, 322, audioHalf, 250); move(AudioDevice + 1, 64 + audioHalf, 322, audioHalf, 250);
    move(AudioReceive, 48, 377, 192, 32); move(AudioReceiveGain, width - 140, 377, 90, 30);
    move(AudioDevice + 2, 48, 510, audioHalf, 250); move(AudioDevice + 3, 64 + audioHalf, 510, audioHalf, 250);
    move(AudioMicrophone, 48, 565, 192, 32); move(AudioMicrophoneGain, width - 140, 565, 90, 30);
    move(AudioPttLimit, width - 228, 651, 180, 240);
    move(AudioUnattended,width - 492,228,286,31);
    move(ScopeReceive, width - 224, 228, 192, 32);
    move(ScopeSpan, 48, 339, 134, 230); move(ScopeGain, 200, 339, 120, 230); move(ScopeFloor, 338, 339, 104, 230);
    move(ScopeFreeze, 462, 339, 100, 32); move(ScopePeak, 574, 339, 124, 32); move(ScopeClear, 710, 339, 80, 32);
    move(ScopeRouting, width - 210, 339, 162, 32);
    move(RfSpan, 48, 339, 114, 230); move(RfCat2Port, 178, 339, 160, 31); move(RfCat2Baud, 350, 339, 72, 220);
    move(RfWaterfallLevel, 432, 339, 80, 31); move(RfApplySpan, 522, 339, 130, 32);
    move(RfCapture, 664, 339, 130, 32); move(RfClear, 806, 339, 110, 32);
    if (layoutBatch_) EndDeferWindowPos(layoutBatch_);
    layoutBatch_ = nullptr;
    for (const auto& [id, child] : controls_) {
        (void)id;
        int page = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(child, L"Page"))) - 2;
        bool visible = page < 0 || page == page_;
        if (static_cast<bool>(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != visible) ShowWindow(child, visible ? SW_SHOWNA : SW_HIDE);
    }
    // Keep list column widths in proportion after resize or a DPI change.
    const int columns[]{220, 145, 150, 90, 250, 185};
    int available = scale(width - 64) - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    for (int i = 0; i < 6; ++i) {
        int columnWidth = MulDiv(available, columns[i], 1040);
        if (ListView_GetColumnWidth(controls_[SettingsList], i) != columnWidth) ListView_SetColumnWidth(controls_[SettingsList], i, columnWidth);
    }
    layingOut_ = false;
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void Dashboard::fitWindowToWorkArea(const RECT* suggested) {
    if (IsIconic(window_)) return;
    RECT bounds{};
    if (suggested) bounds = *suggested; else GetWindowRect(window_, &bounds);
    if (!IsZoomed(window_)) {
        MONITORINFO monitor{sizeof(MONITORINFO), {}, {}, 0};
        const HMONITOR target = suggested ? MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(target, &monitor)) {
            int width = std::min(static_cast<int>(bounds.right - bounds.left), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
            int height = std::min(static_cast<int>(bounds.bottom - bounds.top), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
            bounds.left = std::clamp(bounds.left, monitor.rcWork.left, monitor.rcWork.right - width);
            bounds.top = std::clamp(bounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
            bounds.right = bounds.left + width; bounds.bottom = bounds.top + height;
        }
    } else if (!suggested) return;
    RECT current{}; GetWindowRect(window_, &current);
    if (!EqualRect(&current, &bounds)) SetWindowPos(window_, nullptr, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Dashboard::scrollCanvas(int bar, UINT code) {
    SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL, 0, 0, 0, 0, 0};
    GetScrollInfo(window_, bar, &info);
    int position = info.nPos;
    switch (code) {
    case SB_LINEUP: position -= 32; break;
    case SB_LINEDOWN: position += 32; break;
    case SB_PAGEUP: position -= std::max(32, static_cast<int>(info.nPage) - 64); break;
    case SB_PAGEDOWN: position += std::max(32, static_cast<int>(info.nPage) - 64); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
    case SB_TOP: position = 0; break;
    case SB_BOTTOM: position = info.nMax; break;
    default: return;
    }
    position = std::clamp(position, 0, std::max(0, info.nMax - static_cast<int>(info.nPage) + 1));
    if (position == info.nPos) return;
    SetScrollPos(window_, bar, position, TRUE);
    if (bar == SB_HORZ) scrollX_ = position; else scrollY_ = position;
    layout();
}
void Dashboard::invalidateArea(int x, int y, int width, int height) {
    RECT rect{scale(x - scrollX_), scale(y - scrollY_), scale(x + width - scrollX_), scale(y + height - scrollY_)};
    InvalidateRect(window_, &rect, FALSE);
}
void Dashboard::invalidateLive() {
    invalidateArea(388, 177, canvasWidth_ - 432, 31);
    invalidateArea(0, canvasHeight_ - 55, canvasWidth_, 55);
    if (page_ == 0) {
        invalidateArea(28, 224 + 48, canvasWidth_ - 56, 115);
        invalidateArea(28, 460 + 48, canvasWidth_ - 56, 86);
        invalidateArea(28, 560 + 48, canvasWidth_ - 56, 139);
    }
}
void Dashboard::paint(HDC dc) {
    RECT client{}; GetClientRect(window_, &client);
    int w = canvasWidth_, h = canvasHeight_ - 48;
    bool body = false;
    auto r = [&](int x, int y, int width, int height) { if (body) y += 48; return RECT{scale(x - scrollX_), scale(y - scrollY_), scale(x + width - scrollX_), scale(y + height - scrollY_)}; };
    auto label = [&](int x, int y, int width, int height, const std::wstring& text, HFONT font, COLORREF color = Text, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
        drawText(dc, font, color, r(x, y, width, height), text, flags);
    };
    fill(dc, client, Background);
    label(28, 18, 410, 39, L"FT-710  /  STATION CONTROL", titleFont_);
    label(29, 60, 700, 24, L"M0NXD     ·     Yaesu HF / 6 m     ·     " + wide(rig_version()), smallFont_, Muted);
    label(w - 492, 33, 138, 24, utc(), bodyFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    card(dc, r(28, 93, w - 56, 69));
    label(40, 99, 190, 18, L"CONNECTION", smallFont_, Muted);
    label(242, 99, 180, 18, selected(controls_[ConnectionKind]) == 1 ? L"HOST : PORT" : L"ENHANCED CAT PORT", smallFont_, Muted);
    label(434, 99, 96, 18, L"BAUD", smallFont_, Muted); label(542, 99, 64, 18, L"STOP", smallFont_, Muted);
    label(618, 99, 112, 18, L"FLOW", smallFont_, Muted); label(742, 99, 90, 18, L"POLL (ms)", smallFont_, Muted);
    card(dc, r(28, 170, w - 56, 44));
    label(224, 178, 67, 29, L"TCP port", smallFont_, Muted);
    bool sharing = catServer_ && catServer_->running();
    std::wstring shareStatus = sharing ? L"127.0.0.1:" + std::to_wstring(catServer_->port()) + L"   ·   " +
        std::to_wstring(catServer_->clients()) + L" clients   ·   " + (state_.demo ? L"DEMO radio" : L"Hamlib / NET rigctl") :
        !sharingError_.empty() ? sharingError_ : selected(controls_[ConnectionKind]) == 1 ? L"Select USB CAT to share this radio" :
        sharingWanted_ ? L"Starts when USB CAT connects · Other apps: Hamlib NET rigctl" : L"CAT sharing is disabled";
    label(388, 177, w - 716, 31, shareStatus, smallFont_, !sharingError_.empty() ? Amber : sharing ? Accent : Muted);
    body = true;
    if (page_ == 0) {
        int half = (w - 72) / 2;
        auto vfoCard = [&](int x, const radio::Vfo& vfo, const wchar_t* name, vfo_t which) {
            bool active = state_.activeVfo == which;
            card(dc, r(x, 224, half, 115), Panel, active && state_.online ? Accent : Border);
            label(x + 18, 235, 180, 22, name, bodyFont_, active ? Accent : Blue);
            label(x + half - 212, 235, 194, 22, vfo.frequencyValid ? bandText(vfo.frequency) : L"Waiting for radio", smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            label(x + 18, 256, half - 170, 44, frequencyText(vfo), frequencyFont_, vfo.frequencyValid ? Text : Muted);
            label(x + half - 147, 264, 128, 25, vfo.modeValid ? wide(vfo.mode) : L"--", meterFont_, active ? Accent : Blue, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            label(x + 144, 307, half - 322, 22, vfo.modeValid ? std::to_wstring(vfo.width) + L" Hz filter" : L"-- Hz filter", smallFont_, Muted);
        };
        vfoCard(28, state_.a, L"VFO A", RIG_VFO_A);
        vfoCard(44 + half, state_.b, L"VFO B", RIG_VFO_B);
        card(dc, r(28, 349, w - 56, 101));
        label(40, 352, 104, 20, L"CONTROL VFO", smallFont_, Muted);
        label(158, 352, 242, 20, L"FREQUENCY · MHz / kHz / Hz", smallFont_, Muted);
        label(408, 352, 116, 20, L"MODE / applies", smallFont_, Muted);
        label(538, 352, 196, 20, L"FILTER (Hz) / 0 = normal", smallFont_, Muted);
        const std::vector<std::string> meterIds{"level.STRENGTH", "level.RFPOWER_METER_WATTS", "level.SWR", "level.ALC", "level.COMP_METER", "level.VD_METER", "level.ID_METER"};
        const std::vector<std::wstring> titles{L"SIGNAL", L"RF OUTPUT", L"SWR", L"ALC", L"COMPRESSION", L"SUPPLY", L"DRAIN CURRENT"};
        int meterWidth = (w - 116) / 7;
        for (int i = 0; i < 7; ++i) {
            int x = 28 + i * (meterWidth + 10);
            const radio::Setting* setting = radio::findSetting(state_, meterIds[i]);
            if (i == 1 && !setting) setting = radio::findSetting(state_, "level.RFPOWER_METER");
            double value = 0, proportion = 0;
            std::wstring display = L"--", note = state_.online ? L"Unavailable" : L"No live reading";
            bool valid = setting && setting->valid && state_.online;
            if (setting) note = wide(setting->status);
            if (valid && std::chrono::steady_clock::now() - setting->updated > std::chrono::seconds(2)) {
                valid = false; note = L"Stale / waiting for CAT";
            }
            if (setting && setting->txOnly && (!state_.pttKnown || !state_.transmitting)) {
                valid = false; note = state_.pttKnown ? L"TX only" : L"TX status unavailable";
            }
            if (valid) {
                try { value = std::stod(setting->value); } catch (...) { valid = false; }
                if (valid) {
                    display = wide(radio::displayValue(*setting));
                    if (i == 0) {
                        display = value >= 0 ? L"S9" + (value > 0 ? L" +" + std::to_wstring(static_cast<int>(value)) : L"") :
                            L"S" + std::to_wstring(std::clamp(static_cast<int>(std::lround((value + 54) / 6)), 0, 9));
                        note = wide(setting->value) + L" dB relative to S9";
                        proportion = (value + 54) / 114;
                    } else if (i == 1) {
                        if (setting->id == "level.RFPOWER_METER") { display = meterNumber(value * 100) + L" %"; proportion = value; }
                        else { display = meterNumber(value) + L" W"; proportion = value / 100; }
                        note = state_.demo ? L"Dummy backend reading" : L"Measured on transmit";
                    } else if (i == 2) { display = meterNumber(value) + L" : 1"; proportion = (value - 1) / 4; note = L"Measured on transmit"; }
                    else if (i == 3) { display = meterNumber(value * 100) + L" %"; proportion = value; note = L"ALC reference scale"; }
                    else if (i == 4) { display = meterNumber(value) + L" dB"; proportion = value / 20; }
                    else if (i == 5) { display = meterNumber(value) + L" V"; proportion = value / 16; }
                    else if (i == 6) { display = meterNumber(value) + L" A"; proportion = value / 25; }
                }
            }
            if (i == 1) {
                auto power = radio::findSetting(state_, "level.RFPOWER");
                if (power && power->valid && (valid || (state_.pttKnown && !state_.transmitting))) {
                    try { note = (valid ? L"Measured · set " : L"TX only · set ") + meterNumber(std::stod(power->value) * 100) + L" W"; } catch (...) {}
                }
            }
            COLORREF color = i == 2 && valid && value >= 3 ? Red : i == 2 && valid && value >= 2 ? Amber : i == 1 ? Blue : Accent;
            card(dc, r(x, 460, meterWidth, 86));
            label(x + 14, 466, meterWidth - 28, 18, titles[i], smallFont_, Muted);
            label(x + 14, 484, meterWidth - 28, 30, display, meterFont_, valid ? color : Muted);
            fill(dc, r(x + 14, 516, meterWidth - 28, 3), Border);
            if (valid) fill(dc, r(x + 14, 516, static_cast<int>((meterWidth - 28) * std::clamp(proportion, 0.0, 1.0)), 3), color);
            label(x + 14, 524, meterWidth - 28, 18, note, smallFont_, Muted);
        }
        card(dc, r(28, 560, w - 56, 139));
        label(44, 568, 330, 24, L"RECEIVER & TRANSMITTER", bodyFont_);
        label(w - 430, 568, 386, 24, L"RF power / meters: shared · Gains: " + wide(rig_strvfo(state_.settingsVfo == RIG_VFO_CURR ? state_.activeVfo : state_.settingsVfo)), smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        int third = (w - 120) / 3;
        for (int i = 0; i < 6; ++i) {
            int x = 44 + (i % 3) * (third + 16), y = 608 + (i / 3) * 44;
            label(x, y, third - 155, 29, wide(quickLabels[i]), bodyFont_, Muted);
            const auto* setting = radio::findSetting(state_, quickIds[i]);
            std::wstring reading = state_.online ? L"Radio: waiting / unavailable" : L"Radio: --";
            if (state_.online && setting && setting->valid) {
                try {
                    double value = std::stod(setting->value) * (i == 5 ? 1 : 100);
                    if (i != 5) value = std::round(value);
                    reading = L"Radio: " + wide(radio::formatNumber(value)) + (i == 5 ? L" Hz" : i == 2 ? L" W" : L" %");
                } catch (...) {}
            }
            label(x, y + 28, third - 155, 15, reading, smallFont_, setting && setting->valid && state_.online ? Accent : Muted);
        }
    } else if (page_ == 1) {
        label(32, h - 80, w - 64, 21, L"Double-click a writable row to edit. Read errors are shown per setting; transmit meters are valid during TX.", smallFont_, Muted);
    } else if (page_ == 2) {
        label(32, 228, w - 200, 32, L"ACTIVITY & DIRECT CAT", bodyFont_);
        label(32, 260, w - 64, 20, L"UTC timestamps · Read replies and command errors appear here", smallFont_, Muted);
        label(32, h - 229, w - 64, 24, L"FT-710 CAT console  /  select a query or enter one command ending in ;", bodyFont_);
        label(32, h - 159, w - 64, 22, L"Direct USB CAT only. EX menu commands require the menu ID and values from the Yaesu reference manual.", smallFont_, Muted);
    } else if (page_ == 3) {
        label(32, 228, w - 564, 32, L"AUDIO ROUTING  /  RADIO & REMOTE DEVICES", bodyFont_);
        label(32, 264, w - 64, 23, audioError_.empty() ? (audio::remoteSession() ? L"RDP session: radio = Host hardware / direct; phone = RDP / phone. Refresh after connecting devices." : L"Host hardware / direct stays independent of RDP. Windows shared devices are also available.") : audioError_, smallFont_, audioError_.empty() ? Muted : Amber);
        int half = (w - 112) / 2;
        card(dc, r(28, 296, w - 56, 144));
        label(48, 300, half, 20, L"RADIO RECEIVE SOURCE  /  USB recording device", smallFont_, Muted);
        label(64 + half, 300, half, 20, L"LISTENING DESTINATION  /  speakers, RDP or browser", smallFont_, Muted);
        auto receive = receiveAudio_.status();
        label(260, 376, w - 570, 33, receive.text, smallFont_, receive.active ? Accent : Muted);
        label(w - 248, 377, 100, 30, L"Level (%)", smallFont_, Muted);
        auto peak = [&](int y, const audio::Status& status) {
            label(48, y, 66, 18, L"Input", smallFont_, Muted);
            fill(dc, r(116, y + 7, half - 116, 5), Border);
            fill(dc, r(116, y + 7, static_cast<int>((half - 116) * std::clamp(status.inputPeak, 0.0f, 1.0f)), 5), status.inputPeak >= .98f ? Red : Accent);
            label(64 + half, y, 66, 18, L"Output", smallFont_, Muted);
            fill(dc, r(132 + half, y + 7, half - 68, 5), Border);
            fill(dc, r(132 + half, y + 7, static_cast<int>((half - 68) * std::clamp(status.outputPeak, 0.0f, 1.0f)), 5), status.outputPeak >= .98f ? Red : Blue);
        };
        peak(416, receive);
        card(dc, r(28, 484, w - 56, 144));
        label(48, 488, half, 20, L"MICROPHONE SOURCE  /  phone or local microphone", smallFont_, Muted);
        label(64 + half, 488, half, 20, L"RADIO TRANSMIT DESTINATION  /  USB playback device", smallFont_, Muted);
        auto microphone = microphoneAudio_.status();
        label(260, 564, w - 570, 33, microphone.text, smallFont_, microphone.active ? Accent : Muted);
        label(w - 248, 565, 100, 30, L"Level (%)", smallFont_, Muted);
        peak(604, microphone);
        card(dc, r(28, 640, w - 56, 70));
        label(48, 647, w - 304, 24, L"Tap PTT on to transmit; tap PTT off to return to receive. No holding required.", bodyFont_);
        label(48, 674, w - 304, 22, L"Host mic uses desktop PTT; browser mic uses web PTT. Receive audio mutes during TX.", smallFont_, Muted);
        label(w - 228, 683, 180, 18, L"Tap PTT timeout", smallFont_, Muted);
        label(32, 714, w - 64, 18, L"Phone: use Web: 8888 for browser audio, or enable RDP microphone/playback redirection, then Refresh devices.", smallFont_, Muted);
        label(32, 736, w - 64, 18, L"FT-710: RADIO SETTING > MODE SSB > MOD SOURCE = USB. Adjust USB MOD GAIN for microphone audio.", smallFont_, Muted);
    } else if (page_ == 4) {
        label(32, 228, w - 274, 32, L"RECEIVE AUDIO  /  SPECTRUM & WATERFALL", bodyFont_);
        auto receive = receiveAudio_.status();
        bool generated = scopeDemo_ && state_.demo;
        std::wstring status = generated ? L"DEMO / GENERATED AUDIO  ·  No radio audio device is open" :
            !state_.online || state_.demo ? L"Connect the physical radio, select Audio mappings, then enable Receive audio" :
            !receiveWanted_ ? (receive.text != L"Off" && !receive.active && !receive.starting ? L"Receive audio stopped: " + receive.text : L"Receive audio is off  ·  Select Audio mappings, then enable Receive audio") :
            receive.starting ? receive.text : !state_.pttKnown ? L"Paused / waiting for the radio TX status" :
            state_.transmitting ? L"Paused during TX  ·  Waterfall retains earlier receive audio" :
            !receive.active ? receive.text : scope_.fresh() ? L"LIVE RECEIVE AUDIO  ·  " + std::to_wstring(static_cast<int>(scope_.rate())) + L" samples / second" : L"Waiting for receive samples  ·  " + receive.text;
        if (scope_.frozen) status += L"  ·  Display frozen";
        if (!generated && receive.inputPeak >= .98f) status += L"  ·  INPUT NEAR FULL SCALE";
        label(32, 265, w - 64, 22, status, smallFont_, generated || receive.inputPeak >= .98f ? Amber : !scope_.frozen && scope_.fresh() ? Accent : Muted);
        label(32, 287, w - 64, 20, generated ? L"Synthetic tones and noise for display preview" : L"Source: " + (audioSelectedNames_[0].empty() ? L"select the radio USB recording device in Audio mappings" : audioSelectedNames_[0]), smallFont_, Muted);
        card(dc, r(28, 310, w - 56, 68));
        label(48, 315, 134, 20, L"AUDIO SPAN", smallFont_, Muted);
        label(200, 315, 120, 20, L"DISPLAY GAIN", smallFont_, Muted);
        label(338, 315, 104, 20, L"DISPLAY FLOOR", smallFont_, Muted);
        card(dc, r(28, 386, w - 56, 178));
        label(48, 394, 208, 24, L"SPECTRUM  /  Hz / dBFS", bodyFont_, Accent);
        label(270, 395, w - 318, 22, scope_.detail(), smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        card(dc, r(28, 577, w - 56, h - 650));
        label(48, 583, 208, 24, L"WATERFALL  /  AUDIO Hz", bodyFont_, Blue);
        label(270, 584, w - 318, 22, scope_.historyDetail(), smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        scope_.paint(dc, r(94, 429, w - 140, 105), r(94, 614, w - 140, h - 701), smallFont_, uiScale_);
    } else if (page_ == 5) {
        label(32, 228, w - 64, 32, L"FT-710 RF SCOPE  /  FT4222 USB SPI", bodyFont_);
        const auto active = state_.activeVfo == RIG_VFO_B ? state_.b : state_.a;
        const auto center = rfFrame_.frequencyValid ? rfFrame_.centerHz : active.frequencyValid ? static_cast<std::uint64_t>(std::llround(active.frequency)) : 0;
        const auto span = rf_scope::spanHz(rfFrame_.valid ? rfFrame_.spanIndex : static_cast<unsigned>(std::max(0, selected(controls_[RfSpan]))));
        label(32, 265, w - 64, 22, rfUiStatus_, smallFont_, rfFrame_.fresh() && rfCapture_.running() ? Accent : Muted);
        label(32, 287, w - 64, 20, rfApplyStatus_.empty() ? L"Apply a span through CAT-2, then start capture. Tap a live spectrum or waterfall to tune." : rfApplyStatus_, smallFont_, Muted);
        card(dc, r(28, 310, w - 56, 68));
        label(48, 315, 114, 20, L"RF SPAN", smallFont_, Muted);
        label(178, 315, 176, 20, L"STANDARD / CAT-2 COM", smallFont_, Muted);
        label(351, 315, 68, 20, L"CAT BAUD", smallFont_, Muted);
        label(428, 315, 112, 20, L"WATERFALL LEVEL (dB)", smallFont_, Muted);
        card(dc, r(28, 386, w - 56, 178));
        label(48, 394, 210, 24, L"RF SPECTRUM  /  relative level", bodyFont_, Accent);
        std::wstring details = center ? frequencyText(active) + L" center  ·  " + rf_scope::spanLabel(rfFrame_.valid ? rfFrame_.spanIndex : static_cast<unsigned>(std::max(0, selected(controls_[RfSpan])))) : L"Waiting for active VFO frequency";
        label(270, 395, w - 318, 22, details, smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT plot = r(94, 429, w - 140, 105);
        fill(dc, plot, Background);
        HPEN grid = CreatePen(PS_SOLID, 1, Border), trace = CreatePen(PS_SOLID, scale(2), Accent);
        auto oldPen = SelectObject(dc, grid);
        for (int i = 0; i <= 4; ++i) {
            int x = plot.left + (plot.right - plot.left) * i / 4;
            int y = plot.top + (plot.bottom - plot.top) * i / 4;
            MoveToEx(dc, x, plot.top, nullptr); LineTo(dc, x, plot.bottom);
            MoveToEx(dc, plot.left, y, nullptr); LineTo(dc, plot.right, y);
        }
        if (rfFrame_.valid) {
            SelectObject(dc, trace);
            for (std::size_t i = 0; i < rf_scope::BinCount; ++i) {
                int x = plot.left + static_cast<int>((plot.right - plot.left - 1) * i / (rf_scope::BinCount - 1));
                int y = plot.bottom - 1 - static_cast<int>((plot.bottom - plot.top - 2) * rfFrame_.bins[i] / 255);
                if (!i) MoveToEx(dc, x, y, nullptr); else LineTo(dc, x, y);
            }
        } else {
            drawText(dc, smallFont_, Muted, plot, L"No FT4222 RF scope frames", DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        SelectObject(dc, oldPen); DeleteObject(grid); DeleteObject(trace);
        auto tick = [&](int i) {
            if (!center) return std::wstring(L"--");
            auto hz = static_cast<long long>(center) - static_cast<long long>(span / 2) + static_cast<long long>(span) * i / 4;
            radio::Vfo v; v.frequency = static_cast<double>(hz); v.frequencyValid = true;
            return frequencyText(v);
        };
        for (int i = 0; i <= 4; ++i) label(94 + (w - 140) * i / 4 - 48, 535, 96, 18, tick(i), smallFont_, Muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        card(dc, r(28, 577, w - 56, h - 650));
        label(48, 583, 210, 24, L"RF WATERFALL  /  newest at top", bodyFont_, Blue);
        label(270, 584, w - 318, 22, std::to_wstring(rfHistory_.size()) + L" frames retained", smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT waterfall = r(94, 614, w - 140, h - 712);
        fill(dc, waterfall, Background);
        const int rows = static_cast<int>(std::min<std::size_t>(rfHistory_.size(), static_cast<std::size_t>(std::max<LONG>(0, waterfall.bottom - waterfall.top))));
        if (rows > 0) {
            const bool changed = rfPixels_.size() != static_cast<std::size_t>(rows) * rf_scope::BinCount || rfPixelSequence_ != rfRenderedSequence_;
            const bool incremental = changed && rfPixelSequence_ + 1 == rfRenderedSequence_ && rfPixels_.size() == static_cast<std::size_t>(rows) * rf_scope::BinCount;
            if (incremental && rows > 1) std::memmove(rfPixels_.data() + rf_scope::BinCount,rfPixels_.data(),static_cast<std::size_t>(rows-1)*rf_scope::BinCount*sizeof(std::uint32_t));
            rfPixels_.resize(static_cast<std::size_t>(rows) * rf_scope::BinCount);
            constexpr std::array<COLORREF, 8> palette{RGB(5,8,30), RGB(27,25,108), RGB(22,79,184), RGB(0,170,216), RGB(37,220,151), RGB(236,231,74), RGB(255,121,35), RGB(255,246,220)};
            if (changed) for (int row = 0; row < (incremental ? 1 : rows); ++row) for (std::size_t x = 0; x < rf_scope::BinCount; ++x) {
                double level = rfHistory_[row][x] / 255.0 * (palette.size() - 1); int segment = std::min(static_cast<int>(palette.size()) - 2, static_cast<int>(level)); double part = level - segment;
                auto a = palette[segment], b = palette[segment + 1];
                auto channel = [&](int shift) { return static_cast<int>(((a >> shift) & 255) + part * (static_cast<int>((b >> shift) & 255) - static_cast<int>((a >> shift) & 255))); };
                rfPixels_[static_cast<std::size_t>(row) * rf_scope::BinCount + x] = (channel(0) << 16) | (channel(8) << 8) | channel(16);
            }
            rfPixelSequence_ = rfRenderedSequence_;
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = static_cast<LONG>(rf_scope::BinCount);
            info.bmiHeader.biHeight = -rows; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            // One captured RF frame occupies one display row. New frames are
            // inserted at the top, moving older frames down until they leave
            // the visible area at the bottom.
            StretchDIBits(dc, waterfall.left, waterfall.top, waterfall.right-waterfall.left, rows,
                0, 0, static_cast<int>(rf_scope::BinCount), rows, rfPixels_.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        } else drawText(dc, smallFont_, Muted, waterfall, L"Waiting for FT4222 RF scope frames", DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        for (int i = 0; i <= 4; ++i) label(94 + (w - 140) * i / 4 - 48, waterfall.bottom + 1, 96, 18, tick(i), smallFont_, Muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    body = false; h += 48;
    fill(dc, r(0, h - 55, w, 1), Border);
    COLORREF statusColor = state_.demo ? Amber : state_.online ? Accent : (state_.poweredOff || state_.standby) ? Muted : state_.portOpen ? Amber : Muted;
    fill(dc, r(29, h - 38, 7, 7), statusColor);
    std::wstring status = wide(state_.connectionStatus);
    if (state_.portOpen && !state_.demo) status += L"  ·  " + wide(state_.endpoint);
    if (state_.online && state_.pttKnown) status += state_.transmitting ? L"  ·  TX" : L"  ·  RX";
    label(46, h - 47, w - 350, 27, status, bodyFont_, statusColor);
    label(w - 320, h - 47, 292, 27, std::to_wstring(state_.settings.size()) + L" settings  ·  " +
        (state_.online ? L"LIVE" : L"NO LIVE DATA"), smallFont_, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    label(29, h - 22, w - 57, 19, state_.error.empty() ?
        (state_.demo ? L"Demo is connected to Hamlib Dummy. Meter readings are supplied by that backend." :
        L"FT-710 driver capabilities determine available settings. Use the CAT console for additional menu commands.") : wide(state_.error), smallFont_, state_.error.empty() ? Muted : Amber);
}
bool Dashboard::buttonSelected(int id) const {
    bool on = (id == TabOverview && page_ == 0) || (id == TabSettings && page_ == 1) || (id == TabActivity && page_ == 2) || (id == TabAudio && page_ == 3) || (id == TabScope && page_ == 4) || (id == TabRfScope && page_ == 5) ||
        (id == RfCapture && rfCapture_.running()) ||
        ((id == AudioReceive || id == ScopeReceive) && receiveWanted_) || (id == ScopeFreeze && scope_.frozen) || (id == ScopePeak && scope_.peakHold) ||
        (id == AudioMicrophone && microphoneWanted_) || (id == OpenWeb && webServer_ && webServer_->running()) || (id == ShareCat && sharingWanted_) ||
        (id == VfoA && state_.online && state_.activeVfo == RIG_VFO_A) || (id == VfoB && state_.online && state_.activeVfo == RIG_VFO_B);
    if (id == CyclePreamp) {
        auto setting = radio::findSetting(state_, "level.PREAMP");
        on = setting && setting->valid && setting->value != "0";
    } else if (id == CycleDnr || id == CycleNb) {
        auto setting = radio::findSetting(state_, id == CycleDnr ? "func.NR" : "func.NB");
        on = setting && setting->valid && setting->value != "0";
    }
    if (id >= Function && id < Function + static_cast<int>(functionIds.size())) {
        auto s = radio::findSetting(state_, functionIds[id - Function]);
        on = s && s->valid && s->value != "0";
    }
    return on;
}
void Dashboard::refreshButton(int id) {
    bool activePtt = (id == Ptt || id == PttLatch) && (pttLatched_ || pttPending_ || state_.dashboardPtt);
    unsigned visual = (IsWindowEnabled(controls_[id]) ? 1u : 0u) | (buttonSelected(id) ? 2u : 0u) | (activePtt ? 4u : 0u);
    auto old = buttonVisuals_.find(id);
    if (old == buttonVisuals_.end() || old->second != visual) {
        buttonVisuals_[id] = visual;
        InvalidateRect(controls_[id], nullptr, FALSE);
    }
}
void Dashboard::drawButton(const DRAWITEMSTRUCT& item) {
    int id = static_cast<int>(item.CtlID);
    bool enabled = (item.itemState & ODS_DISABLED) == 0;
    if (id == TuningDial) {
        fill(item.hDC, item.rcItem, Panel);
        card(item.hDC, item.rcItem, Input, enabled ? (dialDragging_ ? Accent : Blue) : Border, scale(8));
        auto rect = item.rcItem;
        for (int x = rect.left + scale(18); x < rect.right - scale(8); x += scale(16))
            fill(item.hDC, RECT{x, rect.bottom - scale(6), x + scale(1), rect.bottom - scale(3)}, Border);
        drawText(item.hDC, smallFont_, enabled ? Text : Muted, rect,
            L"Drag left / right  ·  Scroll wheel or arrow keys  ·  " + textOf(controls_[Step]) + L" steps",
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (item.itemState & ODS_FOCUS) { InflateRect(&rect, -scale(4), -scale(4)); DrawFocusRect(item.hDC, &rect); }
        return;
    }
    bool on = buttonSelected(id);
    bool activePtt = (id == Ptt || id == PttLatch) && (pttLatched_ || pttPending_ || state_.dashboardPtt);
    bool red = id == PowerOff || activePtt;
    bool primary = id == Connect || id == Tune || id == PowerOn || on;
    COLORREF edge = !enabled ? Border : red ? Red : primary ? Accent : Border;
    COLORREF bg = !enabled ? Panel : activePtt ? RGB(92, 37, 48) : on || (item.itemState & ODS_SELECTED) ? RGB(30, 73, 76) : Input;
    fill(item.hDC, item.rcItem, Background);
    card(item.hDC, item.rcItem, bg, edge, scale(8));
    auto rect = item.rcItem; InflateRect(&rect, -scale(5), 0);
    drawText(item.hDC, bodyFont_, !enabled ? RGB(90, 108, 129) : red ? Red : primary ? Accent : Text, rect, textOf(item.hwndItem), DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (item.itemState & ODS_FOCUS) { InflateRect(&rect, -scale(3), -scale(4)); DrawFocusRect(item.hDC, &rect); }
}
std::uint64_t Dashboard::send(radio::Command command) {
    command.generation = state_.generation;
    auto index = command.vfo == RIG_VFO_B ? 1u : 0u;
    if (command.action == radio::Action::Frequency || command.action == radio::Action::StepFrequency) {
        command.uiRequest = ++nextUiRequest_; pendingFrequency_[index] = command.uiRequest;
    }
    if (command.action == radio::Action::Mode) {
        command.uiRequest = ++nextUiRequest_; pendingMode_[index] = command.uiRequest; requestedModes_[index] = command.text;
    }
    if (command.action == radio::Action::SelectVfo) { command.uiRequest = ++nextUiRequest_; pendingVfo_ = command.uiRequest; }
    if (command.action == radio::Action::Set) {
        command.uiRequest = ++nextUiRequest_;
        auto quick = std::find(quickIds.begin(), quickIds.end(), command.id);
        if (quick != quickIds.end()) pendingQuick_[std::distance(quickIds.begin(), quick)] = command.uiRequest;
    }
    auto request = command.uiRequest;
    if (worker_) worker_->submit(std::move(command));
    return request;
}
void Dashboard::syncTuningEditors(bool reset) {
    bool wasUpdating = updating_;
    updating_ = true;
    if (reset) {
        frequencyDirty_ = widthDirty_ = false;
        wheelRemainder_ = dialMotion_ = 0;
    }
    auto index = selected(controls_[TuneVfo]) == 1 ? 1u : 0u;
    const auto& vfo = index == 1 ? state_.b : state_.a;
    if (state_.frequencyRevision[index] >= pendingFrequency_[index]) pendingFrequency_[index] = 0;
    if (state_.modeRevision[index] >= pendingMode_[index]) pendingMode_[index] = 0;
    if (!frequencyDirty_ && (reset || !pendingFrequency_[index]))
        textIfChanged(controls_[Frequency], state_.online && vfo.frequencyValid ? frequencyInput(vfo.frequency) : L"");
    auto name = pendingMode_[index] ? requestedModes_[index] : state_.online && vfo.modeValid ? vfo.mode : std::string{};
    auto mode = std::find(state_.modes.begin(), state_.modes.end(), name);
    if (!SendMessageW(controls_[Mode], CB_GETDROPPEDSTATE, 0, 0))
        selectionIfChanged(controls_[Mode], mode == state_.modes.end() ? -1 : static_cast<int>(std::distance(state_.modes.begin(), mode)));
    if (!widthDirty_ && (reset || !pendingMode_[index]))
        textIfChanged(controls_[Width], state_.online && vfo.modeValid ? std::to_wstring(vfo.width) : L"");
    updating_ = wasUpdating;
}
void Dashboard::selectVfo(vfo_t vfo) {
    if (!state_.online || state_.busy) return;
    SendMessageW(controls_[TuneVfo], CB_SETCURSEL, vfo == RIG_VFO_B ? 1 : 0, 0);
    syncTuningEditors(true);
    radio::Command cmd; cmd.action = radio::Action::SelectVfo; cmd.vfo = vfo; send(cmd);
    InvalidateRect(controls_[TuningDial], nullptr, FALSE);
}
void Dashboard::tuneBySteps(int steps) {
    const auto& vfo = selected(controls_[TuneVfo]) == 1 ? state_.b : state_.a;
    if (!steps || !state_.online || state_.busy || !vfo.frequencyValid) return;
    radio::Command cmd;
    cmd.action = radio::Action::StepFrequency;
    cmd.vfo = selected(controls_[TuneVfo]) == 1 ? RIG_VFO_B : RIG_VFO_A;
    cmd.number = steps * tuningSteps[std::clamp(selected(controls_[Step]), 0, static_cast<int>(tuningSteps.size()) - 1)];
    frequencyDirty_ = false;
    send(cmd);
}
bool Dashboard::tuningWheel(WPARAM wp, LPARAM lp) {
    if (page_ != 0) return false;
    POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    HWND hit = WindowFromPoint(point);
    ScreenToClient(window_, &point);
    point.x += scale(scrollX_); point.y += scale(scrollY_);
    bool background = hit == window_ && point.x >= scale(28) && point.x < scale(canvasWidth_ - 28) &&
        point.y >= scale(224 + 48) && point.y < scale(450 + 48);
    if (!background && hit != controls_[TuningDial] && hit != controls_[Frequency] && hit != controls_[Down] && hit != controls_[Up]) return false;
    wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wp);
    int steps = wheelRemainder_ / WHEEL_DELTA;
    wheelRemainder_ %= WHEEL_DELTA;
    tuneBySteps(steps);
    return true;
}
void Dashboard::update(const radio::Snapshot& state) {
    bool generationChanged = renderedGeneration_ != state.generation;
    if (generationChanged || !state.online || state.busy) cancelTunerClick();
    bool modesChanged = state_.modes != state.modes;
    bool becameOnline = !state_.online && state.online;
    bool settingsVfoChanged = state_.settingsVfo != state.settingsVfo;
    bool pttChanged = state_.pttKnown != state.pttKnown || state_.transmitting != state.transmitting ||
        state_.dashboardPtt != state.dashboardPtt || state_.releasePending != state.releasePending;
    state_ = state;
    if (generationChanged) { pendingFrequency_ = {}; pendingMode_ = {}; pendingQuick_ = {}; pendingVfo_ = 0; }
    if (state.vfoRevision >= pendingVfo_) pendingVfo_ = 0;
    if (state.dashboardPtt) pttPending_ = false;
    if (!state.dashboardPtt && !pttPending_) pttLatched_ = false;
    if (pttPending_ && std::chrono::steady_clock::now() - pttRequested_ > std::chrono::seconds(15)) {
        releasePtt(); log(L"PTT did not confirm; release requested.");
    }
    if ((!state.portOpen || state.poweredOff || state.standby || generationChanged) && (receiveWanted_ || microphoneWanted_)) stopAudio();
    receiveAudio_.gate(receiveWanted_ && state.online && state.pttKnown && !state.transmitting);
    microphoneAudio_.gate(microphoneWanted_ && state.online && state.pttKnown && state.transmitting && state.dashboardPtt && pttLatched_ &&
        (audioSelectedIds_[2] == audio::BrowserMicrophone ? browserPttOwner_ != 0 : browserPttOwner_ == 0));
    if (catServer_) {
        if (catServer_->running() && (!state.portOpen || generationChanged || selected(controls_[ConnectionKind]) == 1)) catServer_->stop();
        if (generationChanged && state.portOpen && sharingWanted_ && selected(controls_[ConnectionKind]) != 1) startSharing();
        // A connect publishes the new generation before the port is opened.
        if (state.portOpen && !state.busy && sharingWanted_ && !catServer_->running() && sharingError_.empty() && selected(controls_[ConnectionKind]) != 1) startSharing();
    }
    updating_ = true;
    textIfChanged(controls_[ShareCat], sharingWanted_ ? (sharingError_.empty() ? L"CAT sharing: On" : L"Retry CAT sharing") : L"CAT sharing: Off");
    EnableWindow(controls_[ShareCat], !state.busy && selected(controls_[ConnectionKind]) != 1);
    EnableWindow(controls_[SharePort], !catServer_ || !catServer_->running());
    textIfChanged(controls_[Connect], state.portOpen ? L"Disconnect" : state.busy ? L"Opening..." : L"Connect");
    EnableWindow(controls_[Connect], !state.busy);
    EnableWindow(controls_[PowerOn], state.portOpen && state.canPower && !state.busy && !state.transmitting && !state.releasePending);
    EnableWindow(controls_[PowerOff], state.portOpen && state.canPower && !state.busy && !state.poweredOff && !state.standby);
    for (int id : {ConnectionKind, Endpoint, Baud, StopBits, Flow, Poll}) EnableWindow(controls_[id], !state.portOpen && !state.busy);
    bool serial = selected(controls_[ConnectionKind]) == 0;
    for (int id : {Baud, StopBits, Flow}) EnableWindow(controls_[id], !state.portOpen && !state.busy && serial);
    EnableWindow(controls_[Endpoint], !state.portOpen && !state.busy && selected(controls_[ConnectionKind]) != 2);
    bool live = state.online && !state.busy;
    for (int id : {TuneVfo, Frequency, Tune, Mode, Width, SetMode, Step, VfoA, VfoB, Swap, Copy}) EnableWindow(controls_[id], live);
    for (int id : {Ptt, PttLatch}) {
        EnableWindow(controls_[id], state.releasePending || state.dashboardPtt || pttPending_ || (live && state.canPtt && state.pttKnown && !state.transmitting));
        textIfChanged(controls_[id], state.releasePending ? L"Retry release" : state.dashboardPtt || pttLatched_ ? L"PTT off" : pttPending_ ? L"Starting TX..." : L"PTT on");
    }
    EnableWindow(controls_[Refresh], state.portOpen && !state.busy);
    EnableWindow(controls_[Export], !state.settings.empty());
    EnableWindow(controls_[SettingsVfo], state.portOpen && !state.busy);
    EnableWindow(controls_[CatQuery], live && !state.demo && selected(controls_[ConnectionKind]) == 0);
    EnableWindow(controls_[CatWrite], live && !state.demo && selected(controls_[ConnectionKind]) == 0);
    if (modesChanged) {
        std::vector<std::wstring> names;
        for (const auto& mode : state.modes) names.push_back(wide(mode));
        combo(controls_[Mode], names);
    }
    bool targetChanged = false;
    if (!pendingVfo_ && state.online) {
        int actual = state.activeVfo == RIG_VFO_B ? 1 : 0;
        targetChanged = selected(controls_[TuneVfo]) != actual;
        selectionIfChanged(controls_[TuneVfo], actual);
    }
    syncTuningEditors(generationChanged || becameOnline || targetChanged);
    const auto& tuneVfo = selected(controls_[TuneVfo]) == 1 ? state.b : state.a;
    for (int id : {Down, Up, TuningDial}) EnableWindow(controls_[id], live && tuneVfo.frequencyValid);
    textIfChanged(controls_[VfoA], state.online && state.activeVfo == RIG_VFO_A ? L"VFO A active" : L"Use VFO A");
    textIfChanged(controls_[VfoB], state.online && state.activeVfo == RIG_VFO_B ? L"VFO B active" : L"Use VFO B");
    for (int i = 0; i < static_cast<int>(quickIds.size()); ++i) {
        if (generationChanged || settingsVfoChanged) {
            quickDirty_[i] = false;
            quickLoaded_[i] = false;
            textIfChanged(controls_[QuickEdit + i], L"");
        }
        auto revision = state.settingRevision.find(quickIds[i]);
        if (revision != state.settingRevision.end() && revision->second >= pendingQuick_[i]) pendingQuick_[i] = 0;
        const auto* setting = radio::findSetting(state, quickIds[i]);
        bool available = live && setting && setting->writable && setting->valid && quickLoaded_[i];
        if (live && setting && setting->valid && !pendingQuick_[i] && (!quickLoaded_[i] || !quickDirty_[i])) {
            double value = std::stod(setting->value);
            if (i != 5) value = std::round(value * 100);
            textIfChanged(controls_[QuickEdit + i], wide(radio::formatNumber(value)));
            quickLoaded_[i] = true;
            available = setting->writable;
        }
        if (!live || !setting || !setting->valid) {
            quickDirty_[i] = false; quickLoaded_[i] = false;
            textIfChanged(controls_[QuickEdit + i], L"");
        }
        EnableWindow(controls_[QuickEdit + i], available); EnableWindow(controls_[QuickSet + i], available && !pendingQuick_[i]);
    }
    const auto* preamp = radio::findSetting(state, "level.PREAMP");
    const auto* dnrFunction = radio::findSetting(state, "func.NR");
    const auto* dnrLevel = radio::findSetting(state, "level.NR");
    const auto* nbFunction = radio::findSetting(state, "func.NB");
    const auto* nbLevel = radio::findSetting(state, "level.NB");
    textIfChanged(controls_[CyclePreamp], L"Preamp: " + preampName(preamp));
    textIfChanged(controls_[CycleDnr], L"DNR: " + noiseReductionName(dnrFunction, dnrLevel, 15, true));
    textIfChanged(controls_[CycleNb], L"NB: " + noiseReductionName(nbFunction, nbLevel, 10, false));
    EnableWindow(controls_[CyclePreamp], live && preamp && preamp->valid && preamp->writable);
    EnableWindow(controls_[CycleDnr], live && dnrFunction && dnrFunction->valid && dnrFunction->writable &&
        dnrLevel && dnrLevel->valid && dnrLevel->writable);
    EnableWindow(controls_[CycleNb], live && nbFunction && nbFunction->valid && nbFunction->writable &&
        nbLevel && nbLevel->valid && nbLevel->writable);
    for (int id : {CyclePreamp, CycleDnr, CycleNb}) refreshButton(id);
    for (int i = 0; i < static_cast<int>(functionIds.size()); ++i) {
        auto s = radio::findSetting(state, functionIds[i]);
        EnableWindow(controls_[Function + i], live && s && s->writable && s->valid);
        refreshButton(Function + i);
    }
    bool settingsChanged = renderedSettingIds_.size() != state.settings.size();
    if (!settingsChanged) {
        for (std::size_t i = 0; i < state.settings.size(); ++i)
            if (renderedSettingIds_[i] != state.settings[i].id) { settingsChanged = true; break; }
    }
    if (generationChanged || settingsChanged) { renderedGeneration_ = state.generation; rebuildRows(); }
    else updateRows();
    int row = ListView_GetNextItem(controls_[SettingsList], -1, LVNI_SELECTED);
    const auto* editable = row >= 0 && static_cast<std::size_t>(row) < visibleRows_.size() ? radio::findSetting(state, visibleRows_[row]) : nullptr;
    EnableWindow(controls_[EditSetting], live && editable && editable->writable);
    updating_ = false;
    updateAudio();
    invalidateLive();
    for (int id : {PowerOn, PowerOff, Connect, VfoA, VfoB, Ptt, PttLatch, TuningDial, ShareCat}) refreshButton(id);
    if (pttChanged) publishWeb(true);
}
void Dashboard::rebuildRows() {
    renderedSettingIds_.clear();
    for (const auto& setting : state_.settings) renderedSettingIds_.push_back(setting.id);
    auto filter = narrow(textOf(controls_[Filter]));
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto list = controls_[SettingsList];
    int selectedRow = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    std::string previous = selectedRow >= 0 && static_cast<std::size_t>(selectedRow) < visibleRows_.size() ? visibleRows_[selectedRow] : "";
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list); visibleRows_.clear();
    for (const auto& setting : state_.settings) {
        std::string searchable = setting.name + " " + setting.id + " " + setting.group;
        std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!filter.empty() && searchable.find(filter) == std::string::npos) continue;
        int row = static_cast<int>(visibleRows_.size());
        auto name = wide(setting.name);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = name.data();
        ListView_InsertItem(list, &item); visibleRows_.push_back(setting.id);
        if (setting.id == previous) ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    updateRows(); SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, FALSE);
}
void Dashboard::updateRows() {
    auto list = controls_[SettingsList];
    auto set = [&](int row, int column, const std::string& text) {
        wchar_t current[2048]{}; ListView_GetItemText(list, row, column, current, 2048);
        auto value = wide(text);
        if (value != current) ListView_SetItemText(list, row, column, value.data());
    };
    for (std::size_t i = 0; i < visibleRows_.size(); ++i) {
        const auto* s = radio::findSetting(state_, visibleRows_[i]); if (!s) continue;
        set(static_cast<int>(i), 1, s->group);
        set(static_cast<int>(i), 2, radio::displayValue(*s));
        set(static_cast<int>(i), 3, s->writable ? s->readable ? "Read / write" : "Write only" : "Read only");
        set(static_cast<int>(i), 4, radio::rangeText(*s));
        std::string status = s->status;
        if (s->valid && s->kind != radio::Kind::Configuration) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s->updated).count();
            status += " / " + std::to_string(age) + "s ago";
        }
        set(static_cast<int>(i), 5, status);
    }
}
void Dashboard::log(const std::wstring& text) {
    activity_.push_back(utc() + L"  " + text);
    if (activity_.size() > 500) activity_.erase(activity_.begin(), activity_.begin() + 100);
    std::wstring all;
    for (const auto& entry : activity_) all += entry + L"\r\n";
    SetWindowTextW(controls_[Activity], all.c_str());
    SendMessageW(controls_[Activity], EM_SETSEL, all.size(), all.size()); SendMessageW(controls_[Activity], EM_SCROLLCARET, 0, 0);
}
void Dashboard::connect() {
    radio::Command cmd;
    if (state_.portOpen) { releasePtt(); stopAudio(); if (catServer_) catServer_->stop(); cmd.action = radio::Action::Disconnect; send(cmd); return; }
    sharingError_.clear();
    cmd.action = radio::Action::Connect;
    cmd.connection = station_settings::connection({{"kind",std::to_string(selected(controls_[ConnectionKind]))},{"endpoint",narrow(textOf(controls_[Endpoint]))},
        {"baud",narrow(textOf(controls_[Baud]))},{"stopBits",narrow(textOf(controls_[StopBits]))},{"flow",narrow(textOf(controls_[Flow]))},{"poll",narrow(textOf(controls_[Poll]))}});
    savePreferences();
    state_.busy = true;
    EnableWindow(controls_[Connect], FALSE);
    send(cmd);
}
void Dashboard::startSharing() {
    if (!catServer_ || !state_.portOpen) return;
    try {
        double port = numberOf(controls_[SharePort]);
        if (port != std::trunc(port) || port < 1 || port > 65535) throw std::invalid_argument("CAT TCP port must be 1 to 65535.");
        catServer_->start(static_cast<unsigned>(port), state_.generation);
        sharingError_.clear();
    } catch (const std::exception& error) {
        sharingError_ = wide(error.what()); log(sharingError_);
    }
}
void Dashboard::cancelTunerClick() {
    KillTimer(window_, TunerClickTimer);
    tunerClickPending_ = false;
}
void Dashboard::toggleTuner() {
    const auto* setting = radio::findSetting(state_, "func.TUNER");
    if (!state_.online || state_.busy || !setting || !setting->valid || !setting->writable) return;
    radio::Command command; command.action = radio::Action::Set; command.id = setting->id;
    command.text = setting->value == "0" ? "1" : "0";
    send(std::move(command));
}
std::uint64_t Dashboard::startAntennaTune() {
    cancelTunerClick();
    if (!state_.online || state_.busy || !state_.canTune)
        throw std::runtime_error("Connect to a radio with antenna tuning support first.");
    if (!state_.pttKnown || state_.transmitting || state_.dashboardPtt || pttLatched_ || pttPending_ || state_.releasePending)
        throw std::runtime_error("Wait for RX and release PTT before antenna tuning.");
    radio::Command command; command.action = radio::Action::TuneAntenna; command.vfo = RIG_VFO_CURR;
    return send(std::move(command));
}
void Dashboard::command(int id, int notification) {
    if (updating_) return;
    try {
        radio::Command cmd;
        cmd.vfo = selected(controls_[TuneVfo]) == 1 ? RIG_VFO_B : RIG_VFO_A;
        if (id == Frequency && notification == EN_CHANGE) { frequencyDirty_ = true; return; }
        if (id == Width && notification == EN_CHANGE) { widthDirty_ = true; return; }
        if (id == Mode && notification == CBN_SELCHANGE) {
            cmd.action = radio::Action::Mode; cmd.text = narrow(textOf(controls_[Mode]));
            if (cmd.text.empty()) return;
            // Use the new mode's normal filter instead of the previous mode's width.
            cmd.width = 0;
            widthDirty_ = false;
            send(cmd); return;
        }
        if (id == Step && notification == CBN_SELCHANGE) {
            wheelRemainder_ = dialMotion_ = 0;
            InvalidateRect(controls_[TuningDial], nullptr, FALSE);
            savePreferences(); return;
        }
        if (id >= QuickEdit && id < QuickEdit + 6 && notification == EN_CHANGE) { quickDirty_[id - QuickEdit] = true; return; }
        if (id >= AudioDevice && id < AudioDevice + 4 && notification == CBN_SELCHANGE) {
            int slot = id - AudioDevice, choice = selected(controls_[id]);
            if (slot < 2) { receiveWanted_ = false; stopRoute(receiveAudio_); receiveAudio_.spectrum().discard(); scope_.clear(); }
            else { microphoneWanted_ = false; stopRoute(microphoneAudio_); releasePtt(); }
            audioSelectedIds_[slot] = choice > 0 && static_cast<std::size_t>(choice) < audioDeviceIds_[slot].size() ? audioDeviceIds_[slot][choice] : L"";
            auto device = std::find_if(audioDevices_.begin(), audioDevices_.end(), [&](const auto& d) { return d.id == audioSelectedIds_[slot] && d.input == (slot == 0 || slot == 2); });
            if (device != audioDevices_.end()) audioSelectedNames_[slot] = device->name;
            else if (audioSelectedIds_[slot].empty()) audioSelectedNames_[slot].clear();
            savePreferences(); updateAudio(); return;
        }
        if (id == AudioPttLimit && notification == CBN_SELCHANGE) { savePreferences(); return; }
        if ((id == Endpoint || id == SharePort || id == RfCat2Port || id == RfWaterfallLevel) && notification == EN_KILLFOCUS) { savePreferences(); return; }
        if ((id == ScopeSpan || id == ScopeGain || id == ScopeFloor) && notification == CBN_SELCHANGE) {
            scope_.spanHz = scopeSpans[std::clamp(selected(controls_[ScopeSpan]), 0, 3)];
            scope_.gainDb = scopeGains[std::clamp(selected(controls_[ScopeGain]), 0, 6)];
            scope_.floorDb = scopeFloors[std::clamp(selected(controls_[ScopeFloor]), 0, 3)];
            savePreferences(); invalidateArea(28, 386 + 48, canvasWidth_ - 56, canvasHeight_ - 386 - 55 - 48); return;
        }
        if ((id == RfSpan || id == RfCat2Baud) && notification == CBN_SELCHANGE) { savePreferences(); invalidateArea(28, 228 + 48, canvasWidth_ - 56, canvasHeight_ - 228); return; }
        if ((id == Baud || id == StopBits || id == Flow || id == Poll) && notification == CBN_SELCHANGE) { savePreferences(); return; }
        if ((id == AudioReceiveGain || id == AudioMicrophoneGain) && notification == EN_KILLFOCUS) {
            double value = numberOf(controls_[id]);
            if (value < 0 || value > 100 || value != std::trunc(value)) throw std::invalid_argument("Audio level must be a whole percent from 0 to 100.");
            (id == AudioReceiveGain ? receiveAudio_ : microphoneAudio_).gain(static_cast<float>(value / 100));
            savePreferences(); return;
        }
        if (id == ConnectionKind && notification == CBN_SELCHANGE) {
            int kind = selected(controls_[ConnectionKind]);
            if (kind == 1 && narrow(textOf(controls_[Endpoint])).find(':') == std::string::npos) SetWindowTextW(controls_[Endpoint], L"127.0.0.1:4532");
            if (kind == 0 && narrow(textOf(controls_[Endpoint])).find(':') != std::string::npos) SetWindowTextW(controls_[Endpoint], L"COM3");
            savePreferences();
            update(state_); return;
        }
        if (id == TuneVfo && notification == CBN_SELCHANGE) {
            selectVfo(cmd.vfo);
            return;
        }
        if (id == Filter && notification == EN_CHANGE) { rebuildRows(); return; }
        if (id == SettingsVfo && notification == CBN_SELCHANGE) {
            cmd.action = radio::Action::SettingsVfo;
            int choice = selected(controls_[SettingsVfo]); cmd.vfo = choice == 0 ? RIG_VFO_CURR : choice == 1 ? RIG_VFO_A : RIG_VFO_B;
            send(cmd); return;
        }
        if (id == CatPreset && notification == CBN_SELCHANGE) {
            const std::vector<std::wstring> commands{L"ID;", L"VE;", L"RI0;", L"DA;", L"AS1;", L"SS00;", L"EX"};
            int index = selected(controls_[CatPreset]);
            if (index >= 0 && static_cast<std::size_t>(index) < commands.size()) SetWindowTextW(controls_[CatCommand], commands[index].c_str());
            return;
        }
        if (notification != BN_CLICKED) return;
        if (id == ShareCat) {
            if (sharingWanted_ && !sharingError_.empty()) {
                sharingError_.clear(); startSharing();
            } else {
                sharingWanted_ = !sharingWanted_; sharingError_.clear();
                if (sharingWanted_) startSharing(); else if (catServer_) catServer_->stop();
            }
            savePreferences(); update(state_); return;
        }
        if (id >= QuickSet && id < QuickSet + static_cast<int>(quickIds.size())) {
            int index = id - QuickSet;
            double value = numberOf(controls_[QuickEdit + index]);
            if (index == 2 && (value < 5 || value > 100 || value != std::trunc(value)))
                throw std::invalid_argument("Enter RF power from 5 to 100 W, in whole watts. Readback shows the setting accepted by the radio.");
            cmd.action = radio::Action::Set; cmd.id = quickIds[index];
            cmd.text = radio::formatNumber(index == 5 ? value : value / 100);
            quickDirty_[index] = false;
            send(cmd); return;
        }
        auto sendSetting = [&](const std::string& settingId, const std::string& value) {
            radio::Command settingCommand;
            settingCommand.action = radio::Action::Set; settingCommand.id = settingId; settingCommand.text = value;
            send(std::move(settingCommand));
        };
        if (id == CyclePreamp) {
            const auto* setting = radio::findSetting(state_, "level.PREAMP");
            if (setting && setting->valid && setting->writable) {
                const std::string next = setting->value == "0" ? "10" : setting->value == "10" ? "20" : "0";
                sendSetting(setting->id, next);
            }
            return;
        }
        if (id == CycleDnr || id == CycleNb) {
            const bool dnr = id == CycleDnr;
            const auto* function = radio::findSetting(state_, dnr ? "func.NR" : "func.NB");
            const auto* level = radio::findSetting(state_, dnr ? "level.NR" : "level.NB");
            const int maximum = dnr ? 15 : 10;
            const int amount = discreteLevel(level, maximum, dnr);
            if (function && function->valid && function->writable && level && level->valid && level->writable && amount >= 0) {
                const bool enabled = function->value != "0";
                if (!enabled) {
                    sendSetting(level->id, radio::formatNumber(dnr ? 1.0 / 15.0 : 1.0));
                    sendSetting(function->id, "1");
                } else if (amount >= maximum) {
                    sendSetting(function->id, "0");
                } else {
                    sendSetting(level->id, radio::formatNumber(dnr ? static_cast<double>(amount + 1) / maximum : amount + 1));
                }
            }
            return;
        }
        if (id >= Function && id < Function + static_cast<int>(functionIds.size())) {
            if (id == TunerButton) {
                if (notification == BN_DOUBLECLICKED) startAntennaTune();
                else if (notification == BN_CLICKED) {
                    cancelTunerClick();
                    tunerClickGeneration_ = state_.generation;
                    tunerClickPending_ = SetTimer(window_, TunerClickTimer, GetDoubleClickTime() + 1, nullptr) != 0;
                    if (!tunerClickPending_) toggleTuner();
                }
                return;
            }
            const auto* setting = radio::findSetting(state_, functionIds[id - Function]);
            if (setting && setting->valid) {
                cmd.action = radio::Action::Set; cmd.id = setting->id; cmd.text = setting->value == "0" ? "1" : "0";
                send(cmd);
            }
            return;
        }
        switch (id) {
        case Connect: connect(); return;
        case OpenWeb: openWeb(); return;
        case WebPasscode: editWebPasscode(); return;
        case PowerOn: cmd.action = radio::Action::PowerOn; break;
        case PowerOff: releasePtt(); cmd.action = radio::Action::PowerOff; break;
        case TabOverview: page_ = 0; layout(); return;
        case TabSettings: page_ = 1; layout(); updateRows(); return;
        case TabActivity: page_ = 2; layout(); return;
        case TabAudio: page_ = 3; layout(); updateAudio(); return;
        case TabScope: page_ = 4; layout(); updateAudio(); return;
        case TabRfScope: page_ = 5; layout(); updateRfScope(); return;
        case RfCapture:
            setRfCapture(!rfCaptureWanted_); return;
        case RfApplySpan: {
            static constexpr int baudRates[]{4800, 9600, 19200, 38400, 115200};
            auto baud = baudRates[std::clamp(selected(controls_[RfCat2Baud]), 0, 4)];
            const auto port = textOf(controls_[RfCat2Port]);
            const auto levelText = textOf(controls_[RfWaterfallLevel]);
            double level = 0; std::size_t consumed = 0;
            try { level = std::stod(levelText, &consumed); }
            catch (...) { rfUiStatus_ = L"Enter a valid waterfall level from -30 to +30 dB in 0.5 dB steps"; log(rfUiStatus_); invalidateArea(28, 228 + 48, canvasWidth_ - 56, canvasHeight_ - 228); return; }
            if (consumed != levelText.size()) { rfUiStatus_ = L"Enter a valid waterfall level from -30 to +30 dB in 0.5 dB steps"; log(rfUiStatus_); invalidateArea(28, 228 + 48, canvasWidth_ - 56, canvasHeight_ - 228); return; }
            applyRfSettings(port, static_cast<unsigned>(std::max(0, selected(controls_[RfSpan]))), static_cast<unsigned>(baud), level); return;
        }
        case RfClear: rfHistory_.clear(); rfPixels_.clear(); invalidateArea(28, 577 + 48, canvasWidth_ - 56, canvasHeight_ - 577); return;
        case AudioUnattended: unattended_ = !unattended_; savePreferences(); updateAudio(); return;
        case ScopeRouting: page_ = 3; layout(); updateAudio(); return;
        case ScopeFreeze:
            scope_.frozen = !scope_.frozen;
            if (!scope_.frozen) scope_.pause();
            textIfChanged(controls_[ScopeFreeze], scope_.frozen ? L"Resume" : L"Freeze");
            refreshButton(ScopeFreeze); layout(); return;
        case ScopePeak:
            scope_.peakHold = !scope_.peakHold; scope_.resetPeaks();
            textIfChanged(controls_[ScopePeak], scope_.peakHold ? L"Peak hold: On" : L"Peak hold: Off");
            refreshButton(ScopePeak); layout(); return;
        case ScopeClear: receiveAudio_.spectrum().discard(); scope_.clear(); layout(); return;
        case AudioRefresh: releasePtt(); stopAudio(); refreshAudioDevices(); return;
        case AudioReceive: case ScopeReceive: toggleAudio(false); return;
        case AudioMicrophone: toggleAudio(true); return;
        case Ptt: case PttLatch:
            if (pttLatched_ || pttPending_ || state_.dashboardPtt || state_.releasePending) { releasePtt(); return; }
            {
                browserPttOwner_ = 0;
                const long limits[]{30, 60, 180, 300, 600};
                cmd.action = radio::Action::PttOn; cmd.width = limits[std::clamp(selected(controls_[AudioPttLimit]), 0, 4)];
                pttLatched_ = pttPending_ = true; pttRequested_ = std::chrono::steady_clock::now();
                send(cmd); update(state_); return;
            }
        case Tune:
            cmd.action = radio::Action::Frequency; cmd.number = frequencyHzOf(controls_[Frequency]);
            frequencyDirty_ = false;
            break;
        case SetMode:
            cmd.action = radio::Action::Mode; cmd.text = narrow(textOf(controls_[Mode]));
            { double width = numberOf(controls_[Width]); if (std::trunc(width) != width || width < 0 || width > 100000) throw std::invalid_argument("Passband must be a whole number from 0 to 100000 Hz."); cmd.width = static_cast<long>(width); }
            widthDirty_ = false;
            break;
        case Down: case Up: tuneBySteps(id == Up ? 1 : -1); return;
        case VfoA: case VfoB: selectVfo(id == VfoA ? RIG_VFO_A : RIG_VFO_B); return;
        case Swap: cmd.action = radio::Action::Swap; break;
        case Copy: cmd.action = radio::Action::CopyAtoB; break;
        case Refresh: cmd.action = radio::Action::Refresh; break;
        case EditSetting: editSelected(); return;
        case Export: exportCsv(); return;
        case CatQuery: case CatWrite:
            cmd.action = id == CatQuery ? radio::Action::RawQuery : radio::Action::RawWrite;
            cmd.text = narrow(textOf(controls_[CatCommand])); break;
        case ClearLog: activity_.clear(); SetWindowTextW(controls_[Activity], L""); return;
        case Manual:
            ShellExecuteW(window_, L"open", L"https://www.yaesu.com/Files/4CB893D7-1018-01AF-FA97E9E9AD48B50C/FT-710_CAT_OM_ENG_2306-C.pdf", nullptr, nullptr, SW_SHOWNORMAL); return;
        default: return;
        }
        send(cmd);
    } catch (const std::exception& error) {
        log(wide(error.what()));
        MessageBoxW(window_, wide(error.what()).c_str(), L"Radio control", MB_OK | MB_ICONINFORMATION);
    }
}
void Dashboard::editSelected() {
    int row = ListView_GetNextItem(controls_[SettingsList], -1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= visibleRows_.size()) return;
    auto* setting = radio::findSetting(state_, visibleRows_[row]);
    if (!setting || !setting->writable || !state_.online) return;
    releasePtt();
    Editor editor; editor.setting = *setting;
    auto generation = state_.generation;
    if (editor.show(window_, instance_)) {
        radio::Command command; command.action = radio::Action::Set;
        command.id = editor.setting.id; command.text = editor.result; command.generation = generation;
        worker_->submit(command);
    }
}
void Dashboard::releasePtt() {
    browserPttOwner_ = 0;
    audio::browserAudio().microphoneOwner(0, false);
    if (!pttLatched_ && !pttPending_ && !state_.dashboardPtt && !state_.releasePending) return;
    pttLatched_ = pttPending_ = false;
    microphoneAudio_.gate(false);
    radio::Command cmd; cmd.action = radio::Action::PttOff; send(cmd);
    InvalidateRect(controls_[Ptt], nullptr, FALSE);
    InvalidateRect(controls_[PttLatch], nullptr, FALSE);
}
void Dashboard::postUi(std::function<void()> completion) {
    auto* job = new std::function<void()>(std::move(completion));
    if (!PostMessageW(window_,dashboard_id::CompletionMessage,0,reinterpret_cast<LPARAM>(job))) delete job;
}
void Dashboard::stopRoute(audio::Route& route) {
    route.requestStop();
    if (closing_) route.stop();
    else audioJobs_.submit([&route] { route.stop(); });
}
void Dashboard::applyRfSettings(std::wstring port, unsigned span, unsigned baud, double level, std::shared_ptr<web::Request> request) {
    station_settings::scope({{"rfCat2Port",narrow(port)},{"rfSpan",std::to_string(span)},{"rfCat2Baud",std::to_string(baud)},{"rfWaterfallLevel",std::to_string(level)}},true);
    if (rfApplying_) throw std::invalid_argument("A scope settings change is already in progress");
    rfApplying_ = true; rfApplyStatus_ = L"Applying scope settings via CAT-2…";
    EnableWindow(controls_[RfApplySpan],FALSE);
    const auto access = request ? request->accessRevision : 0;
    scopeJobs_.submit([this,port,span,baud,level,request,access] {
        std::wstring result;
        try {
            if (request && (!webServer_ || access != webServer_->accessRevision())) result = L"Web passcode changed; scope request cancelled";
            else result = scopeApply_(port,span,baud,level);
        }
        catch (const std::exception& e) { result = wide(e.what()); }
        postUi([this,span,level,request,access,result=std::move(result)] {
            rfApplying_ = false; EnableWindow(controls_[RfApplySpan],TRUE);
            std::wostringstream label; label << std::fixed << std::setprecision(1) << level;
            rfApplyStatus_ = result.empty() ? L"Radio confirmed " + rf_scope::spanLabel(span) + L" span / " + label.str() + L" dB" : result;
            savePreferences(); log(rfApplyStatus_); publishWeb(true);
            if (request) {
                const auto error = !result.empty() ? result : preferencesError_;
                if (!webServer_ || access != webServer_->accessRevision()) request->reply.set_value("\"ok\":false,\"error\":\"Web passcode changed; sign in again\"");
                else request->reply.set_value(error.empty() ? "\"ok\":true,\"queued\":false" : "\"ok\":false,\"error\":" + web::json(narrow(error)));
            }
        });
    });
    publishWeb(true);
}
void Dashboard::browserMicrophone(std::uint64_t client, bool armed) {
    if (armed) {
        browserMicClients_.insert(client);
        try { if (!microphoneWanted_) toggleAudio(true); }
        catch (...) { browserMicClients_.erase(client); throw; }
    } else {
        const bool registered = browserMicClients_.erase(client) != 0;
        if (browserPttOwner_ == client) releasePtt();
        if (registered && browserMicClients_.empty() && audioSelectedIds_[2] == audio::BrowserMicrophone && microphoneWanted_)
            toggleAudio(true);
    }
    updateAudio();
}
void Dashboard::refreshAudioDevices() {
    if (audioRefreshing_) return;
    audioRefreshing_ = true; const auto revision = ++audioRefreshRevision_;
    audioJobs_.submit([this,revision] {
        std::vector<audio::Device> devices; std::wstring error;
        try { devices = audio::devices(); }
        catch (const std::exception& e) { error = L"Cannot enumerate Windows audio devices: " + wide(e.what()); }
        postUi([this,revision,devices=std::move(devices),error=std::move(error)]() mutable {
            if (revision != audioRefreshRevision_) return;
            audioRefreshing_ = false; applyAudioDevices(std::move(devices),std::move(error)); publishWeb(true);
        });
    });
}
void Dashboard::applyAudioDevices(std::vector<audio::Device> devices, std::wstring error) {
    audioDevices_ = std::move(devices); audioError_ = std::move(error);
    if (!audioError_.empty()) log(audioError_);
    if (!audio::directDeviceIssue().empty()) { audioError_ = L"Direct hardware audio: " + audio::directDeviceIssue(); log(audioError_); }
    unsigned hardwareInputs = 0, hardwareOutputs = 0, remoteInputs = 0, remoteOutputs = 0;
    for (const auto& device : audioDevices_) {
        if (!device.remote) (device.input ? hardwareInputs : hardwareOutputs)++;
        else (device.input ? remoteInputs : remoteOutputs)++;
    }
    if (audioError_.empty() && audio::remoteSession() && (!hardwareInputs || !hardwareOutputs))
        audioError_ = L"RDP is active, but a radio hardware recording/playback device is missing. Connect its USB audio and Refresh devices.";
    log(L"Audio inventory: " + std::to_wstring(hardwareInputs) + L" host recording / " + std::to_wstring(hardwareOutputs) +
        L" host playback / " + std::to_wstring(remoteInputs) + L" remote recording / " + std::to_wstring(remoteOutputs) + L" remote playback.");
    bool previousUpdating = updating_; updating_ = true;
    for (int slot = 0; slot < 4; ++slot) {
        std::vector<std::wstring> names{L"Select a device..."}; audioDeviceIds_[slot] = {L""};
        for (const auto& device : audioDevices_) if (device.input == (slot == 0 || slot == 2) && !((slot == 0 || slot == 3) && device.remote)) {
            names.push_back(device.name); audioDeviceIds_[slot].push_back(device.id);
            if (audioSelectedIds_[slot] == device.id) audioSelectedNames_[slot] = device.name;
        }
        if (!audioSelectedIds_[slot].empty() && std::find(audioDeviceIds_[slot].begin(), audioDeviceIds_[slot].end(), audioSelectedIds_[slot]) == audioDeviceIds_[slot].end()) {
            if (audioSelectedNames_[slot].empty()) audioSelectedNames_[slot] = audio::savedDeviceName(audioSelectedIds_[slot], slot == 0 || slot == 2);
            std::wstring saved = audioSelectedNames_[slot].empty() ? L"Saved audio device" : audioSelectedNames_[slot];
            names.push_back(saved + L" [Unavailable / reconnect]"); audioDeviceIds_[slot].push_back(audioSelectedIds_[slot]);
        }
        auto it = std::find(audioDeviceIds_[slot].begin(), audioDeviceIds_[slot].end(), audioSelectedIds_[slot]);
        int choice = it == audioDeviceIds_[slot].end() ? 0 : static_cast<int>(it - audioDeviceIds_[slot].begin());
        combo(controls_[AudioDevice + slot], names, choice);
    }
    updating_ = previousUpdating; updateAudio();
}
void Dashboard::stopAudio() {
    browserMicClients_.clear();
    receiveWanted_ = microphoneWanted_ = false;
    stopRoute(receiveAudio_); stopRoute(microphoneAudio_);
    audio::browserAudio().microphoneOwner(0, false);
}
void Dashboard::toggleAudio(bool microphone) {
    bool& wanted = microphone ? microphoneWanted_ : receiveWanted_;
    auto& route = microphone ? microphoneAudio_ : receiveAudio_;
    if (wanted) {
        wanted = false; stopRoute(route); if (microphone) releasePtt();
    } else {
        if (!state_.online || state_.demo) throw std::invalid_argument("Connect to the physical radio before enabling audio routing.");
        int slot = microphone ? 2 : 0;
        for (int i = slot; i < slot + 2; ++i) {
            int choice = selected(controls_[AudioDevice + i]);
            if (choice <= 0 || static_cast<std::size_t>(choice) >= audioDeviceIds_[i].size())
                throw std::invalid_argument("Select both the source and destination for this audio route.");
            auto device = std::find_if(audioDevices_.begin(), audioDevices_.end(), [&](const auto& d) { return d.id == audioSelectedIds_[i] && d.input == (i == 0 || i == 2); });
            if (device == audioDevices_.end()) throw std::invalid_argument("A saved audio device is unavailable. For the radio in RDP, select its Host hardware / direct entry; for the phone, select RDP / phone.");
            if ((i == 0 || i == 3) && device->remote) throw std::invalid_argument("Remote Audio belongs to the RDP client and cannot be the radio source/destination. Select the radio's Host hardware / direct device.");
        }
        double value = numberOf(controls_[microphone ? AudioMicrophoneGain : AudioReceiveGain]);
        if (value < 0 || value > 100 || value != std::trunc(value)) throw std::invalid_argument("Audio level must be a whole percent from 0 to 100.");
        route.gain(static_cast<float>(value / 100));
        const auto source = audioSelectedIds_[slot], destination = audioSelectedIds_[slot + 1]; wanted = true;
        audioJobs_.submit([this,&route,source,destination] {
            try { route.start(source,destination); }
            catch (const std::exception& e) { postUi([this,message=wide(e.what())] { audioError_ = message; log(message); }); }
        });
        log(microphone ? L"Microphone route enabled; radio output is gated by dashboard PTT." : L"Receive audio route enabled; it mutes during TX.");
    }
    savePreferences(); updateAudio();
}
void Dashboard::updateAudio() {
    textIfChanged(controls_[AudioUnattended],unattended_ ? L"Unattended browser audio: On" : L"Unattended browser audio: Off");
    auto receive = receiveAudio_.status(), microphone = microphoneAudio_.status();
    if (receiveWanted_ && !receive.active && !receive.starting && receive.text != L"Off") { receiveWanted_ = false; log(L"Receive audio stopped: " + receive.text); }
    if (microphoneWanted_ && !microphone.active && !microphone.starting && microphone.text != L"Off") {
        microphoneWanted_ = false; microphoneAudio_.gate(false); releasePtt(); log(L"Microphone audio stopped: " + microphone.text);
    }
    receiveAudio_.gate(receiveWanted_ && state_.online && state_.pttKnown && !state_.transmitting);
    bool browserMicrophone = audioSelectedIds_[2] == audio::BrowserMicrophone;
    bool microphoneGate = microphoneWanted_ && state_.online && state_.pttKnown && state_.transmitting && state_.dashboardPtt && pttLatched_ &&
        (browserMicrophone ? browserPttOwner_ != 0 : browserPttOwner_ == 0);
    microphoneAudio_.gate(microphoneGate);
    audio::browserAudio().microphoneOwner(browserPttOwner_, browserMicrophone && microphoneGate && microphone.active);
    bool ready = state_.online && !state_.demo && !state_.busy;
    for (auto [id, wanted] : {std::pair{AudioReceive, receiveWanted_}, std::pair{ScopeReceive, receiveWanted_}, std::pair{AudioMicrophone, microphoneWanted_}}) {
        EnableWindow(controls_[id], wanted || ready);
        textIfChanged(controls_[id], id != AudioMicrophone ? (wanted ? L"Receive audio: On" : L"Receive audio: Off") : (wanted ? L"Microphone: On" : L"Microphone: Off"));
        refreshButton(id);
    }
    if (page_ == 3) invalidateArea(28, 296 + 48, canvasWidth_ - 56, 332);
    if (page_ == 4) invalidateArea(32, 265 + 48, canvasWidth_ - 64, 42);
}
void Dashboard::updateScope() {
    auto& analyzer = receiveAudio_.spectrum();
    if (scopeDemo_ && state_.demo) {
        if (!demoSample_) analyzer.configure(48000);
        // Explicit --scope-demo only: no input device, playback, CAT PTT or RF.
        for (unsigned i = 0; i < 2400; ++i, ++demoSample_) {
            double t = demoSample_ / 48000.0;
            demoNoise_ = demoNoise_ * 1664525u + 1013904223u;
            float noise = (static_cast<float>(demoNoise_ >> 8) / 16777216 - .5f) * .003f;
            float tone = static_cast<float>(.025 * std::sin(6.283185307179586 * 700 * t) +
                .012 * std::sin(6.283185307179586 * (1600 * t + 70 * std::sin(t))) + .006 * std::sin(6.283185307179586 * 2400 * t));
            analyzer.push(tone + noise);
        }
    }
    bool wasFresh = scope_.fresh();
    audio::SpectrumFrame frame;
    bool changed = analyzer.poll(frame);
    if (changed) { scope_.update(frame); webSpectrumTick_ = frame.valid ? GetTickCount64() : 0; if (webServer_) webServer_->spectrum(frame); }
    if (!(scopeDemo_ && state_.demo) && (!receiveWanted_ || !receiveAudio_.status().active || !state_.online || !state_.pttKnown || state_.transmitting)) scope_.pause();
    if (page_ == 4 && (changed || wasFresh != scope_.fresh()))
        invalidateArea(28, 386 + 48, canvasWidth_ - 56, canvasHeight_ - 386 - 55 - 48);
}
void Dashboard::updateRfScope() {
    const auto status = rfCapture_.status();
    bool changed = status != rfUiStatus_;
    rfUiStatus_ = rfCaptureChanging_ ? (rfCaptureWanted_ ? L"Starting RF capture…" : L"Stopping RF capture…") : status;
    if (!rfCaptureChanging_) rfCaptureWanted_ = rfCapture_.running();
    if (rfCapture_.running() && rfFrame_.valid && !rfFrame_.fresh()) rfUiStatus_ = L"RF capture stalled · frozen history";
    textIfChanged(controls_[RfCapture], rfCapture_.running() ? L"Stop RF capture" : L"Start RF capture");
    refreshButton(RfCapture);
    rf_scope::Frame next;
    if (rfCaptureWanted_ && rfCapture_.running() && rfCapture_.latest(next) && next.fresh() && next.sequence != rfRenderedSequence_) {
        rfRenderedSequence_ = next.sequence;
        const auto& active = state_.activeVfo == RIG_VFO_B ? state_.b : state_.a;
        if (!next.frequencyValid && active.frequencyValid) { next.centerHz = static_cast<std::uint64_t>(std::llround(active.frequency)); next.frequencyValid = true; }
        if (rfFrame_.generation != next.generation || rfFrame_.centerHz != next.centerHz || rfFrame_.spanIndex != next.spanIndex) {
            rfHistory_.clear(); rfPixels_.clear();
        }
        rfFrame_ = next;
        rfHistory_.push_front(next.bins);
        if (rfHistory_.size() > 2048) rfHistory_.pop_back();
        if (webServer_) webServer_->rfSpectrum(rfFrame_, rfFrame_.centerHz, rf_scope::spanHz(rfFrame_.spanIndex));
        // The span editor is a draft; the frame and its axis remain radio readback.
        changed = true;
    }
    const auto& active = state_.activeVfo == RIG_VFO_B ? state_.b : state_.a;
    auto center = rfFrame_.frequencyValid ? rfFrame_.centerHz : active.frequencyValid ? static_cast<std::uint64_t>(std::llround(active.frequency)) : 0;
    if (center != rfRenderedCenterHz_) { rfRenderedCenterHz_ = center; changed = true; }
    if (page_ == 5 && changed) invalidateArea(28, 228 + 48, canvasWidth_ - 56, canvasHeight_ - 228);
    EnableWindow(controls_[RfApplySpan],!rfApplying_);
}
bool Dashboard::setRfCapture(bool enabled) {
    rfCaptureWanted_ = enabled;
    rfCaptureChanging_ = true; const auto revision = ++rfOperationRevision_;
    if (!enabled) rfCapture_.requestStop();
    rfFrame_ = {}; rfHistory_.clear(); rfPixels_.clear(); rfRenderedSequence_ = 0;
    if (webServer_) webServer_->rfSpectrum({},0,0);
    rfUiStatus_ = enabled ? L"Starting RF capture…" : L"Stopping RF capture…";
    scopeJobs_.submit([this,enabled,revision] {
        if (enabled) rfCapture_.start(); else rfCapture_.stop();
        postUi([this,revision] { if (revision == rfOperationRevision_) rfCaptureChanging_ = false; updateRfScope(); publishWeb(true); });
    });
    publishWeb(true); return true;
}
void Dashboard::exportCsv() {
    releasePtt();
    wchar_t path[MAX_PATH] = L"FT710-settings.csv";
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof dialog; dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"CSV files\0*.csv\0All files\0*.*\0"; dialog.lpstrFile = path; dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; dialog.lpstrDefExt = L"csv";
    if (!GetSaveFileNameW(&dialog)) return;
    std::ofstream file{std::filesystem::path(path)};
    if (!file) { MessageBoxW(window_, L"Could not write the CSV file.", L"Export", MB_OK | MB_ICONERROR); return; }
    file << "\xEF\xBB\xBF";
    file << "Name,Group,Hamlib ID,Value,Raw value,Access,Range,Status,VFO,UTC\r\n";
    auto writeRow = [&](std::string name, std::string id, std::string value, std::string status) {
        file << csv(name) << ",\"Radio / VFO\"," << csv(id) << ',' << csv(value) << ',' << csv(value) << ",\"Read / write\",\"\"," << csv(status) << ",\"\"," << csv(narrow(utc())) << "\r\n";
    };
    writeRow("VFO A frequency (Hz)", "VFOA.frequency", state_.a.frequencyValid ? radio::formatNumber(state_.a.frequency) : "", state_.a.frequencyValid ? "Live" : "Unavailable");
    writeRow("VFO B frequency (Hz)", "VFOB.frequency", state_.b.frequencyValid ? radio::formatNumber(state_.b.frequency) : "", state_.b.frequencyValid ? "Live" : "Unavailable");
    writeRow("VFO A mode / width", "VFOA.mode", state_.a.modeValid ? state_.a.mode + " / " + std::to_string(state_.a.width) : "", state_.a.modeValid ? "Live" : "Unavailable");
    writeRow("VFO B mode / width", "VFOB.mode", state_.b.modeValid ? state_.b.mode + " / " + std::to_string(state_.b.width) : "", state_.b.modeValid ? "Live" : "Unavailable");
    for (const auto& s : state_.settings) {
        file << csv(s.name) << ',' << csv(s.group) << ',' << csv(s.id) << ',' << csv(radio::displayValue(s)) << ',' << csv(s.valid ? s.value : "") << ',' <<
            csv(s.writable ? "Read / write" : "Read only") << ',' << csv(radio::rangeText(s)) << ',' << csv(s.status) << ',' << csv(rig_strvfo(state_.settingsVfo)) << ',' << csv(narrow(utc())) << "\r\n";
    }
    if (!file) log(L"Export failed while writing file."); else log(L"Settings exported to " + std::wstring(path));
}
void Dashboard::savePreferences() {
    if (iniPath_.empty() || !capturePath_.empty() || preview_) return;
    const auto previousError = preferencesError_; preferencesError_.clear();
    auto writeIni = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* path) {
        if (!WritePrivateProfileStringW(section,key,value,path) && preferencesError_.empty())
            preferencesError_ = L"Cannot save settings (Windows error " + std::to_wstring(GetLastError()) + L"). Check access to " + iniPath_;
    };
    auto save = [&](const wchar_t* key, const std::wstring& value) { writeIni(L"Connection", key, value.c_str(), iniPath_.c_str()); };
    save(L"Kind", std::to_wstring(selected(controls_[ConnectionKind])));
    save(L"Endpoint", textOf(controls_[Endpoint]));
    save(L"ShareCAT", sharingWanted_ ? L"1" : L"0");
    save(L"SharePort", textOf(controls_[SharePort]));
    for (auto [id, name] : std::vector<std::pair<int, const wchar_t*>>{{Baud,L"Baud"},{StopBits,L"StopBits"},{Flow,L"Flow"},{Poll,L"Poll"}})
        save(name, std::to_wstring(selected(controls_[id])));
    const wchar_t* audioKeys[]{L"ReceiveSource", L"ReceiveDestination", L"MicrophoneSource", L"MicrophoneDestination"};
    for (int i = 0; i < 4; ++i) {
        writeIni(L"Audio", audioKeys[i], audioSelectedIds_[i].c_str(), iniPath_.c_str());
        writeIni(L"Audio", (std::wstring(audioKeys[i]) + L"Name").c_str(), audioSelectedNames_[i].c_str(), iniPath_.c_str());
    }
    writeIni(L"Audio", L"ReceiveLevel", textOf(controls_[AudioReceiveGain]).c_str(), iniPath_.c_str());
    writeIni(L"Audio", L"MicrophoneLevel", textOf(controls_[AudioMicrophoneGain]).c_str(), iniPath_.c_str());
    writeIni(L"Audio", L"PttLimit", std::to_wstring(selected(controls_[AudioPttLimit])).c_str(), iniPath_.c_str());
    writeIni(L"Audio", L"Unattended", unattended_ ? L"1" : L"0", iniPath_.c_str());
    writeIni(L"Tuning", L"Step", std::to_wstring(selected(controls_[Step])).c_str(), iniPath_.c_str());
    for (auto [id, key] : {std::pair{ScopeSpan, L"Span"}, std::pair{ScopeGain, L"Gain"}, std::pair{ScopeFloor, L"Floor"}})
        writeIni(L"Scope", key, std::to_wstring(selected(controls_[id])).c_str(), iniPath_.c_str());
    writeIni(L"RfScope", L"Span", std::to_wstring(selected(controls_[RfSpan])).c_str(), iniPath_.c_str());
    writeIni(L"RfScope", L"Cat2Port", textOf(controls_[RfCat2Port]).c_str(), iniPath_.c_str());
    writeIni(L"RfScope", L"Cat2Baud", std::to_wstring(selected(controls_[RfCat2Baud])).c_str(), iniPath_.c_str());
    writeIni(L"RfScope", L"WaterfallLevelDb", textOf(controls_[RfWaterfallLevel]).c_str(), iniPath_.c_str());
    if (!preferencesError_.empty() && preferencesError_ != previousError) log(preferencesError_);
}
void Dashboard::loadPreferences() {
    if (iniPath_.empty()) return;
    unattended_ = GetPrivateProfileIntW(L"Audio",L"Unattended",0,iniPath_.c_str()) != 0;
    auto read = [&](const wchar_t* key, int fallback, int max) { return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Connection", key, fallback, iniPath_.c_str())), 0, max); };
    SendMessageW(controls_[ConnectionKind], CB_SETCURSEL, read(L"Kind", 0, 2), 0);
    SendMessageW(controls_[Baud], CB_SETCURSEL, read(L"Baud", 3, 4), 0);
    SendMessageW(controls_[StopBits], CB_SETCURSEL, read(L"StopBits", 0, 1), 0);
    SendMessageW(controls_[Flow], CB_SETCURSEL, read(L"Flow", 0, 2), 0);
    SendMessageW(controls_[Poll], CB_SETCURSEL, read(L"Poll", 1, 4), 0);
    wchar_t endpoint[256]{}; GetPrivateProfileStringW(L"Connection", L"Endpoint", L"COM3", endpoint, 256, iniPath_.c_str());
    SetWindowTextW(controls_[Endpoint], endpoint);
    sharingWanted_ = read(L"ShareCAT", 1, 1) != 0;
    int sharePort = static_cast<int>(GetPrivateProfileIntW(L"Connection", L"SharePort", 4532, iniPath_.c_str()));
    SetWindowTextW(controls_[SharePort], std::to_wstring(std::clamp(sharePort, 1, 65535)).c_str());
    const wchar_t* audioKeys[]{L"ReceiveSource", L"ReceiveDestination", L"MicrophoneSource", L"MicrophoneDestination"};
    for (int i = 0; i < 4; ++i) {
        wchar_t id[1024]{}; GetPrivateProfileStringW(L"Audio", audioKeys[i], L"", id, 1024, iniPath_.c_str()); audioSelectedIds_[i] = id;
        wchar_t name[1024]{}; GetPrivateProfileStringW(L"Audio", (std::wstring(audioKeys[i]) + L"Name").c_str(), L"", name, 1024, iniPath_.c_str()); audioSelectedNames_[i] = name;
    }
    for (auto [id, key] : {std::pair{AudioReceiveGain, L"ReceiveLevel"}, std::pair{AudioMicrophoneGain, L"MicrophoneLevel"}}) {
        int value = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Audio", key, 100, iniPath_.c_str())), 0, 100);
        SetWindowTextW(controls_[id], std::to_wstring(value).c_str());
    }
    SendMessageW(controls_[AudioPttLimit], CB_SETCURSEL, std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Audio", L"PttLimit", 2, iniPath_.c_str())), 0, 4), 0);
    SendMessageW(controls_[Step], CB_SETCURSEL, std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Tuning", L"Step", 1, iniPath_.c_str())), 0, static_cast<int>(tuningSteps.size()) - 1), 0);
    for (auto [id, key, fallback, maximum] : std::vector<std::tuple<int, const wchar_t*, int, int>>{{ScopeSpan,L"Span",1,3},{ScopeGain,L"Gain",1,6},{ScopeFloor,L"Floor",2,3}})
        SendMessageW(controls_[id], CB_SETCURSEL, std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Scope", key, fallback, iniPath_.c_str())), 0, maximum), 0);
    scope_.spanHz = scopeSpans[std::clamp(selected(controls_[ScopeSpan]), 0, 3)];
    scope_.gainDb = scopeGains[std::clamp(selected(controls_[ScopeGain]), 0, 6)];
    scope_.floorDb = scopeFloors[std::clamp(selected(controls_[ScopeFloor]), 0, 3)];
    SendMessageW(controls_[RfSpan], CB_SETCURSEL, std::clamp(static_cast<int>(GetPrivateProfileIntW(L"RfScope", L"Span", 7, iniPath_.c_str())), 0, 9), 0);
    SendMessageW(controls_[RfCat2Baud], CB_SETCURSEL, std::clamp(static_cast<int>(GetPrivateProfileIntW(L"RfScope", L"Cat2Baud", 0, iniPath_.c_str())), 0, 4), 0);
    wchar_t cat2Port[64]{}; GetPrivateProfileStringW(L"RfScope", L"Cat2Port", L"", cat2Port, 64, iniPath_.c_str());
    SetWindowTextW(controls_[RfCat2Port], cat2Port);
    wchar_t waterfallLevel[24]{}; GetPrivateProfileStringW(L"RfScope", L"WaterfallLevelDb", L"-5.0", waterfallLevel, 24, iniPath_.c_str());
    SetWindowTextW(controls_[RfWaterfallLevel], waterfallLevel);
}
void Dashboard::capture() {
    // Owner-drawn controls must finish their pending repaint before the
    // screenshot's WM_PRINT traversal copies their cached surfaces.
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RECT rect{}; GetClientRect(window_, &rect);
    std::ofstream metadata{std::filesystem::path(capturePath_ + L".txt")};
    metadata << "DPI: " << dpi_ << "\nClient: " << rect.right << 'x' << rect.bottom
        << "\nUI scale: " << uiScale_
        << "\nCanvas: " << canvasWidth_ << 'x' << canvasHeight_
        << "\nScroll: " << scrollX_ << ',' << scrollY_
        << "\nCAT sharing requested: " << sharingWanted_ << "\nCAT sharing button: " << narrow(textOf(controls_[ShareCat]))
        << "\nSettings discovered: " << state_.settings.size() << "\nSettings rows: " << visibleRows_.size() << '\n';
    HDC dc = GetDC(window_), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    auto old = SelectObject(memory, bitmap);
    PrintWindow(window_, memory, PW_CLIENTONLY);
    // PrintWindow may reuse themed button surfaces in an occluded RDP
    // session. Ask our owner-drawn widgets to print their current state.
    for (const auto& [id, child] : controls_) {
        (void)id;
        wchar_t className[32]{}; GetClassNameW(child, className, 32);
        if (CompareStringOrdinal(className, -1, L"BUTTON", -1, TRUE) != CSTR_EQUAL || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT childRect{}; GetWindowRect(child, &childRect);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&childRect), 2);
        int saved = SaveDC(memory);
        IntersectClipRect(memory, childRect.left, childRect.top, childRect.right, childRect.bottom);
        SetViewportOrgEx(memory, childRect.left, childRect.top, nullptr);
        SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
        RestoreDC(memory, saved);
    }
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0; Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        CLSID encoder{}; CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}", &encoder);
        image.Save(capturePath_.c_str(), &encoder, nullptr);
    }
    Gdiplus::GdiplusShutdown(token);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window_, dc);
    PostMessageW(window_, WM_CLOSE, 0, 0);
}
LRESULT CALLBACK Dashboard::buttonProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    if (msg == WM_LBUTTONDBLCLK && GetDlgCtrlID(window) == TunerButton && IsWindowEnabled(window)) {
        // Consume the second mouse press so its release cannot queue another
        // single-click toggle after the full tuning request.
        auto* self = reinterpret_cast<Dashboard*>(data);
        self->command(TunerButton, BN_DOUBLECLICKED);
        return 0;
    }
    if (msg == WM_PRINTCLIENT || (msg == WM_PRINT && (lp & PRF_CLIENT))) {
        // Use the same renderer as normal WM_DRAWITEM. Default button
        // printing can otherwise copy a cached, out-of-date caption.
        auto* self = reinterpret_cast<Dashboard*>(data);
        DRAWITEMSTRUCT item{};
        item.CtlType = ODT_BUTTON; item.CtlID = GetDlgCtrlID(window); item.itemAction = ODA_DRAWENTIRE;
        item.hwndItem = window; item.hDC = reinterpret_cast<HDC>(wp);
        if (!IsWindowEnabled(window)) item.itemState |= ODS_DISABLED;
        if (GetFocus() == window) item.itemState |= ODS_FOCUS;
        if (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) item.itemState |= ODS_SELECTED;
        GetClientRect(window, &item.rcItem); self->drawButton(item);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, buttonProc, 2);
    return DefSubclassProc(window, msg, wp, lp);
}
LRESULT CALLBACK Dashboard::tuningProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<Dashboard*>(data);
    int id = GetDlgCtrlID(window);
    if (msg == WM_GETDLGCODE) {
        LRESULT code = DefSubclassProc(window, msg, wp, lp);
        if (id == TuningDial) code |= DLGC_WANTARROWS;
        auto* key = reinterpret_cast<MSG*>(lp);
        if ((id == Frequency || id == Width) && key && key->message == WM_KEYDOWN && key->wParam == VK_RETURN) code |= DLGC_WANTMESSAGE;
        return code;
    }
    if (msg == WM_KEYDOWN && IsWindowEnabled(window)) {
        if (wp == VK_RETURN && (id == Frequency || id == Width)) {
            self->command(id == Frequency ? Tune : SetMode, BN_CLICKED); return 0;
        }
        if (id == TuningDial) {
            if (wp == VK_LEFT || wp == VK_DOWN || wp == VK_RIGHT || wp == VK_UP) {
                self->tuneBySteps(wp == VK_LEFT || wp == VK_DOWN ? -1 : 1); return 0;
            }
            if (wp == VK_PRIOR || wp == VK_NEXT) { self->tuneBySteps(wp == VK_PRIOR ? 10 : -10); return 0; }
        }
    }
    if (msg == WM_CHAR && wp == VK_RETURN && (id == Frequency || id == Width)) return 0;
    if (id == TuningDial) {
        if (msg == WM_SETCURSOR && IsWindowEnabled(window)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
        if (msg == WM_LBUTTONDOWN && IsWindowEnabled(window)) {
            SetFocus(window); SetCapture(window);
            self->dialDragging_ = true; self->dialLastX_ = GET_X_LPARAM(lp); self->dialMotion_ = 0;
            InvalidateRect(window, nullptr, FALSE); return 0;
        }
        if (msg == WM_MOUSEMOVE && self->dialDragging_ && GetCapture() == window) {
            self->dialMotion_ += GET_X_LPARAM(lp) - self->dialLastX_;
            self->dialLastX_ = GET_X_LPARAM(lp);
            int pixelsPerStep = std::max(1, self->scale(8));
            int steps = self->dialMotion_ / pixelsPerStep;
            self->dialMotion_ %= pixelsPerStep;
            self->tuneBySteps(steps); return 0;
        }
        if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE || msg == WM_KILLFOCUS) {
            self->dialDragging_ = false; self->dialMotion_ = 0;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (msg == WM_LBUTTONUP) return 0;
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, tuningProc, 1);
    return DefSubclassProc(window, msg, wp, lp);
}
LRESULT CALLBACK Dashboard::windowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Dashboard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Dashboard*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
}
LRESULT Dashboard::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        dpi_ = GetDpiForWindow(window_);
        uiScale_ = dpi_ / 96.0;
        BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof dark);
        WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
        createControls(); layout(); update(state_); return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_HSCROLL: case WM_VSCROLL:
        if (!lp) { scrollCanvas(msg == WM_HSCROLL ? SB_HORZ : SB_VERT, LOWORD(wp)); return 0; }
        break;
    case WM_MOUSEWHEEL: {
        if (tuningWheel(wp, lp)) return 0;
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL, 0, 0, 0, 0, 0};
        if (GetScrollInfo(window_, SB_VERT, &info) && info.nMax >= static_cast<int>(info.nPage)) {
            int delta = MulDiv(GET_WHEEL_DELTA_WPARAM(wp), 96, WHEEL_DELTA);
            int position = std::clamp(info.nPos - delta, 0, info.nMax - static_cast<int>(info.nPage) + 1);
            if (position != info.nPos) { scrollY_ = position; SetScrollPos(window_, SB_VERT, position, TRUE); layout(); }
            return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        fitWindowToWorkArea(reinterpret_cast<RECT*>(lp));
        layout(); return 0;
    }
    case WM_DISPLAYCHANGE:
        fitWindowToWorkArea(); layout(); return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) { fitWindowToWorkArea(); layout(); }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window_, &ps);
        RECT rect{}; GetClientRect(window_, &rect);
        HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
        auto old = SelectObject(memory, bitmap); paint(memory);
        BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); EndPaint(window_, &ps); return 0;
    }
    case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(wp)); return 0;
    case WM_DRAWITEM: drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp); SetTextColor(dc, Text); SetBkColor(dc, Input); return reinterpret_cast<LRESULT>(inputBrush_);
    }
    case WM_COMMAND: command(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_LBUTTONUP: {
        if (page_ == 5 && rfCapture_.running() && rfFrame_.fresh()) {
            const double x = unscale(GET_X_LPARAM(lp)) + scrollX_;
            const double y = unscale(GET_Y_LPARAM(lp)) + scrollY_;
            const int plotX = 94, plotY = 429 + 48, plotWidth = canvasWidth_ - 140, plotHeight = 105;
            const auto& active = state_.activeVfo == RIG_VFO_B ? state_.b : state_.a;
            const auto center = rfFrame_.frequencyValid ? rfFrame_.centerHz : active.frequencyValid ? static_cast<std::uint64_t>(std::llround(active.frequency)) : 0;
            const auto span = rf_scope::spanHz(rfFrame_.spanIndex);
            if (center && span && x >= plotX && x < plotX + plotWidth && y >= plotY && y <= plotY + plotHeight) {
                const double fraction = std::clamp(static_cast<double>(x - plotX) / plotWidth, 0.0, 1.0);
                radio::Command cmd; cmd.action = radio::Action::Frequency; cmd.vfo = state_.activeVfo;
                cmd.number = static_cast<double>(center) - span / 2.0 + fraction * span;
                send(cmd); return 0;
            }
        }
        break;
    }
    case WM_NOTIFY: {
        auto* notification = reinterpret_cast<NMHDR*>(lp);
        if (notification->idFrom == SettingsList && notification->code == NM_DBLCLK) editSelected();
        if (notification->idFrom == SettingsList && notification->code == LVN_ITEMCHANGED) {
            int row = ListView_GetNextItem(controls_[SettingsList], -1, LVNI_SELECTED);
            const auto* s = row >= 0 && static_cast<std::size_t>(row) < visibleRows_.size() ? radio::findSetting(state_, visibleRows_[row]) : nullptr;
            EnableWindow(controls_[EditSetting], state_.online && !state_.busy && s && s->writable);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TunerClickTimer) {
            const bool apply = tunerClickPending_ && tunerClickGeneration_ == state_.generation;
            cancelTunerClick();
            if (apply) toggleTuner();
            return 0;
        }
        if (wp == 1) {
            updateAudio();
            if (!state_.dashboardPtt && !pttPending_ && !pttLatched_) browserPttOwner_ = 0;
            publishWeb();
            if (page_ == 0) invalidateArea(28, 460 + 48, canvasWidth_ - 56, 86);
            auto second = GetTickCount64() / 1000;
            if (second != clockTick_) {
                clockTick_ = second;
                invalidateArea(canvasWidth_ - 492, 33, 138, 24);
                invalidateArea(388, 177, canvasWidth_ - 432, 31);
                if (page_ == 1) updateRows();
            }
        }
        if (wp == 2) { KillTimer(window_, 2); capture(); }
        if (wp == 3) { updateScope(); updateRfScope(); }
        return 0;
    case UpdateMessage: {
        std::unique_ptr<radio::Snapshot> state(reinterpret_cast<radio::Snapshot*>(lp));
        MSG newer{};
        while (PeekMessageW(&newer, window_, UpdateMessage, UpdateMessage, PM_REMOVE))
            state.reset(reinterpret_cast<radio::Snapshot*>(newer.lParam));
        update(*state); return 0;
    }
    case LogMessage: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp)); log(*text); return 0;
    }
    case WebMessage: {
        std::unique_ptr<std::shared_ptr<web::Request>> request(reinterpret_cast<std::shared_ptr<web::Request>*>(lp));
        webCommand(*request); return 0;
    }
    case CompletionMessage: {
        std::unique_ptr<std::function<void()>> job(reinterpret_cast<std::function<void()>*>(lp));
        if (!closing_) { try { (*job)(); } catch (const std::exception& e) { log(wide(e.what())); } }
        return 0;
    }
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_REMOTE_DISCONNECT || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_SESSION_LOCK || wp == WTS_SESSION_LOGOFF) {
            releasePtt();
            if (!unattended_) stopAudio();
            else if (audioSelectedIds_[2] != audio::BrowserMicrophone) { microphoneWanted_ = false; stopRoute(microphoneAudio_); }
            updateAudio(); log(unattended_ ? L"Session ended: PTT released; unattended browser receive audio remains enabled." : L"Session ended: PTT and audio stopped.");
        }
        if ((wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_UNLOCK) && !unattended_) { stopAudio(); refreshAudioDevices(); }
        return 0;
    case WM_CLOSE: {
        closing_ = true;
        cancelTunerClick();
        releasePtt(); stopAudio(); savePreferences(); WTSUnRegisterSessionNotification(window_);
        audioJobs_.stop(); scopeJobs_.stop();
        if (webServer_) webServer_->stop();
        if (catServer_) catServer_->stop();
        if (worker_) worker_->stop();
        // Drain worker-owned payloads before destroying the receiving window.
        MSG pending{};
        while (PeekMessageW(&pending, window_, UpdateMessage, LogMessage, PM_REMOVE)) {
            if (pending.message == UpdateMessage) delete reinterpret_cast<radio::Snapshot*>(pending.lParam);
            if (pending.message == LogMessage) delete reinterpret_cast<std::wstring*>(pending.lParam);
        }
        while (PeekMessageW(&pending, window_, WebMessage, WebMessage, PM_REMOVE)) delete reinterpret_cast<std::shared_ptr<web::Request>*>(pending.lParam);
        while (PeekMessageW(&pending, window_, CompletionMessage, CompletionMessage, PM_REMOVE)) delete reinterpret_cast<std::function<void()>*>(pending.lParam);
        DestroyWindow(window_); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window_, msg, wp, lp);
}
