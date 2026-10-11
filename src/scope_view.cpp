#include "scope_view.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {
constexpr COLORREF Grid = RGB(31, 48, 64), Muted = RGB(141, 162, 184);
constexpr COLORREF Trace = RGB(80, 211, 194), Peak = RGB(255, 193, 101);
void text(HDC dc, HFONT font, RECT rect, const std::wstring& value, UINT flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, Muted);
    DrawTextW(dc, value.c_str(), -1, &rect, flags); SelectObject(dc, old);
}
std::wstring frequency(double hz) {
    std::wostringstream value;
    if (hz >= 1000) value << std::fixed << std::setprecision(hz < 10000 ? 1 : 0) << hz / 1000 << L" k";
    else value << static_cast<int>(hz);
    return value.str();
}
std::uint32_t color(float fraction) {
    constexpr std::array<std::array<int, 3>, 6> stops{{{8,16,29}, {24,45,116}, {24,128,189}, {52,213,181}, {255,202,83}, {255,92,75}}};
    float scaled = std::clamp(fraction, 0.0f, 1.0f) * (stops.size() - 1);
    int index = std::min(static_cast<int>(scaled), static_cast<int>(stops.size()) - 2);
    float part = scaled - index;
    auto channel = [&](int i) { return static_cast<unsigned>(stops[index][i] + part * (stops[index + 1][i] - stops[index][i])); };
    return (channel(0) << 16) | (channel(1) << 8) | channel(2); // 32-bit DIB BGR bytes.
}
}
ScopeView::ScopeView() : history_(History), pixels_(ImageWidth * History) { clear(); }
void ScopeView::resetPeaks() { peaks_.fill(-140); }
void ScopeView::clear() {
    next_ = rows_ = 0; valid_ = false; latestTick_ = 0;
    smooth_.fill(-140); resetPeaks(); ++revision_;
}
double ScopeView::span() const { return std::min<double>(spanHz, rate_ > 0 ? rate_ / 2 : spanHz); }
void ScopeView::update(const audio::SpectrumFrame& frame) {
    if (generation_ != frame.generation) { clear(); generation_ = frame.generation; }
    rate_ = frame.sampleRate;
    if (!frame.valid) { pause(); return; }
    if (frozen) return;
    bool previous = fresh();
    for (unsigned i = 0; i < frame.db.size(); ++i) {
        // Smooth power rather than averaging logarithmic values.
        float power = std::pow(10.0f, frame.db[i] / 10);
        if (previous) power = .55f * std::pow(10.0f, smooth_[i] / 10) + .45f * power;
        smooth_[i] = 10 * std::log10(std::max(power, 1e-14f));
        if (peakHold) peaks_[i] = std::max(peaks_[i], frame.db[i]);
    }
    latestTick_ = GetTickCount64(); valid_ = true;
    auto& row = history_[next_]; row.db = frame.db; row.tick = latestTick_;
    next_ = (next_ + 1) % History; rows_ = std::min(rows_ + 1, History); ++revision_;
}
std::wstring ScopeView::detail() const {
    if (!fresh()) return L"4096-point Hann FFT  ·  Waiting for receive samples";
    unsigned last = std::min(static_cast<unsigned>(audio::SpectrumFrame::Bins - 1), static_cast<unsigned>(span() * audio::SpectrumFrame::Size / rate_));
    unsigned strongest = 1;
    for (unsigned i = 2; i <= last; ++i) if (smooth_[i] > smooth_[strongest]) strongest = i;
    std::wostringstream value;
    value << L"Peak " << frequency(strongest * rate_ / audio::SpectrumFrame::Size) << L"Hz  ·  "
          << std::fixed << std::setprecision(1) << smooth_[strongest] << L" dBFS  ·  " << rate_ / audio::SpectrumFrame::Size << L" Hz / bin";
    return value.str();
}
std::wstring ScopeView::historyDetail() const {
    if (!rows_) return L"Newest at top  ·  Up to 300 receive frames";
    auto oldest = history_[(next_ + History - rows_) % History].tick;
    std::wostringstream value;
    value << (frozen ? L"Frozen  ·  " : L"Newest at top  ·  ") << rows_ << L" frames  ·  "
          << std::fixed << std::setprecision(1) << (latestTick_ - oldest) / 1000.0 << L" s between first / last";
    return value.str();
}
float ScopeView::column(const std::array<float, audio::SpectrumFrame::Bins>& data, unsigned x, unsigned width) const {
    double bins = span() * audio::SpectrumFrame::Size / std::max(1.0, rate_);
    unsigned begin = std::min(static_cast<unsigned>(data.size() - 1), static_cast<unsigned>(x * bins / width));
    unsigned end = std::min(static_cast<unsigned>(data.size()), std::max(begin + 1, static_cast<unsigned>((x + 1) * bins / width)));
    return *std::max_element(data.begin() + begin, data.begin() + end);
}
void ScopeView::rasterize() {
    if (imageRevision_ == revision_ && imageSpan_ == spanHz && imageGain_ == gainDb && imageFloor_ == floorDb) return;
    for (unsigned y = 0; y < History; ++y) {
        const Row* row = y < rows_ ? &history_[(next_ + History - 1 - y) % History] : nullptr;
        for (unsigned x = 0; x < ImageWidth; ++x) {
            float db = row ? column(row->db, x, ImageWidth) : -140;
            pixels_[y * ImageWidth + x] = row ? color((db + gainDb - floorDb) / -static_cast<float>(floorDb)) : color(0);
        }
    }
    imageRevision_ = revision_; imageSpan_ = spanHz; imageGain_ = gainDb; imageFloor_ = floorDb;
}
void ScopeView::paint(HDC dc, RECT spectrum, RECT waterfall, HFONT font, unsigned dpi) {
    auto px = [&](int value) { return MulDiv(value, dpi, 96); };
    auto oldPen = SelectObject(dc, GetStockObject(DC_PEN)); SetDCPenColor(dc, Grid);
    auto background = CreateSolidBrush(RGB(8, 16, 29)); FillRect(dc, &spectrum, background); FillRect(dc, &waterfall, background); DeleteObject(background);
    for (int i = 0; i <= 4; ++i) {
        int y = spectrum.top + (spectrum.bottom - spectrum.top - 1) * i / 4;
        MoveToEx(dc, spectrum.left, y, nullptr); LineTo(dc, spectrum.right, y);
        text(dc, font, RECT{spectrum.left - px(55), y - px(9), spectrum.left - px(7), y + px(9)}, std::to_wstring(floorDb * i / 4 - gainDb), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    for (int i = 0; i <= 6; ++i) {
        int x = spectrum.left + (spectrum.right - spectrum.left - 1) * i / 6;
        MoveToEx(dc, x, spectrum.top, nullptr); LineTo(dc, x, spectrum.bottom);
        std::wstring hz = frequency(span() * i / 6);
        text(dc, font, RECT{x - px(30), spectrum.bottom + px(3), x + px(30), spectrum.bottom + px(23)}, hz);
        text(dc, font, RECT{x - px(30), waterfall.bottom + px(1), x + px(30), waterfall.bottom + px(16)}, hz);
    }
    if (fresh()) {
        int width = spectrum.right - spectrum.left, height = spectrum.bottom - spectrum.top - 1;
        std::vector<POINT> points(width);
        auto trace = [&](const auto& bins, COLORREF colorValue, int penWidth) {
            for (int x = 0; x < width; ++x) {
                float fraction = std::clamp((column(bins, x, width) + gainDb - floorDb) / -static_cast<float>(floorDb), 0.0f, 1.0f);
                points[x] = POINT{spectrum.left + x, spectrum.bottom - 1 - static_cast<int>(fraction * height)};
            }
            auto pen = CreatePen(PS_SOLID, penWidth, colorValue); auto old = SelectObject(dc, pen);
            Polyline(dc, points.data(), width); SelectObject(dc, old); DeleteObject(pen);
        };
        if (peakHold) trace(peaks_, Peak, 1);
        trace(smooth_, Trace, std::max(1, px(2)));
    } else text(dc, font, spectrum, L"Waiting for fresh receive audio");
    if (rows_) {
        rasterize();
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = ImageWidth;
        info.bmiHeader.biHeight = -static_cast<LONG>(History); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, waterfall.left, waterfall.top, waterfall.right - waterfall.left, waterfall.bottom - waterfall.top,
            0, 0, ImageWidth, History, pixels_.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else text(dc, font, waterfall, L"Waterfall history appears here as receive audio arrives");
    // Digital colour scale uses the same gain/floor as the spectrum axis.
    for (int y = waterfall.top; y < waterfall.bottom; ++y) {
        auto rgb = color(1 - static_cast<float>(y - waterfall.top) / std::max(1L, waterfall.bottom - waterfall.top - 1));
        SetDCPenColor(dc, RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
        MoveToEx(dc, waterfall.left - px(13), y, nullptr); LineTo(dc, waterfall.left - px(5), y);
    }
    text(dc, font, RECT{waterfall.left - px(59), waterfall.top - px(5), waterfall.left - px(17), waterfall.top + px(13)}, std::to_wstring(-gainDb), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    text(dc, font, RECT{waterfall.left - px(59), waterfall.bottom - px(15), waterfall.left - px(17), waterfall.bottom + px(3)}, std::to_wstring(floorDb - gainDb), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, oldPen);
}
