#pragma once
namespace dashboard_id {
enum Id {
    ConnectionKind = 101, Endpoint, Baud, StopBits, Flow, Poll, Connect,
    PowerOn, PowerOff, TabOverview, TabSettings, TabActivity, TabAudio, Export, Refresh,
    ShareCat, SharePort, TabScope = 118, OpenWeb, WebPasscode, TabRfScope,
    TuneVfo = 201, Frequency, Tune, Mode, Width, SetMode, Step, Down, Up, VfoA, VfoB, Swap, Copy, Ptt, PttLatch, TuningDial,
    QuickEdit = 300, QuickSet = 310, CyclePreamp = 400, CycleDnr, CycleNb, Function = 410,
    Filter = 501, SettingsVfo, SettingsList, EditSetting,
    Activity = 601, CatCommand, CatQuery, CatWrite, CatPreset, ClearLog, Manual,
    AudioDevice = 801, AudioRefresh = 811, AudioReceive, AudioMicrophone, AudioReceiveGain, AudioMicrophoneGain, AudioPttLimit,
    AudioUnattended = 818,
    ScopeSpan = 901, ScopeGain, ScopeFloor, ScopeFreeze, ScopePeak, ScopeClear, ScopeReceive, ScopeRouting,
    RfSpan = 920, RfCat2Port, RfCat2Baud, RfWaterfallLevel, RfCapture, RfApplySpan, RfClear
};
inline constexpr unsigned WebMessage = WM_APP + 3;
inline constexpr unsigned CompletionMessage = WM_APP + 4;
}
