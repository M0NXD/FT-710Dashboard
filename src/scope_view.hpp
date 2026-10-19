#pragma once
#include <windows.h>
#include "audio_scope.hpp"
#include <array>
#include <string>
#include <vector>

// UI-thread state and GDI drawing for the received-audio scope.
class ScopeView {
public:
    ScopeView();
    int spanHz = 6000, gainDb = 0, floorDb = -100;
    bool frozen = false, peakHold = false;
    void update(const audio::SpectrumFrame& frame);
    void clear();
    void resetPeaks();
    void pause() { if (!frozen) valid_ = false; }
    bool fresh() const { return valid_ && (frozen || GetTickCount64() - latestTick_ < 500); }
    double rate() const { return rate_; }
    double span() const;
    std::wstring detail() const;
    std::wstring historyDetail() const;
    void paint(HDC dc, RECT spectrum, RECT waterfall, HFONT font, double uiScale);
private:
    static constexpr unsigned History = 300, ImageWidth = 1024;
    struct Row { std::array<float, audio::SpectrumFrame::Bins> db{}; ULONGLONG tick = 0; };
    std::vector<Row> history_;
    std::array<float, audio::SpectrumFrame::Bins> smooth_{}, peaks_{};
    std::vector<std::uint32_t> pixels_;
    unsigned next_ = 0, rows_ = 0;
    double rate_ = 0;
    bool valid_ = false;
    std::uint64_t generation_ = 0, revision_ = 1, imageRevision_ = 0;
    int imageSpan_ = 0, imageGain_ = 0, imageFloor_ = 0;
    ULONGLONG latestTick_ = 0;
    float column(const std::array<float, audio::SpectrumFrame::Bins>& data, unsigned x, unsigned width) const;
    void rasterize();
};
