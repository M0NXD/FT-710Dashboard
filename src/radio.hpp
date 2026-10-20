#pragma once
#include <hamlib/rig.h>
#include "cat.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {
enum class ConnectionType { Serial, Network, Demo };
struct Connection {
    ConnectionType type = ConnectionType::Serial;
    std::string endpoint = "COM3";
    int baud = 38400;
    int stopBits = 1;
    std::string handshake = "None";
    int pollMs = 750;
};
enum class Kind { Level, Function, Parameter, ExtLevel, ExtFunction, ExtParameter, Configuration, Special };
enum class ValueType { Integer, Real, Boolean, Text, Choice };
struct Setting {
    std::string id, name, group, description, units;
    Kind kind = Kind::Level;
    ValueType type = ValueType::Integer;
    setting_t mask = 0;
    hamlib_token_t token = 0;
    bool readable = false, writable = false, bounded = false, txOnly = false;
    double minimum = 0, maximum = 0, step = 0;
    std::vector<std::string> choices, choiceValues;
    std::string value, status = "Waiting";
    bool valid = false;
    std::chrono::steady_clock::time_point updated{};
};
struct Vfo {
    double frequency = 0;
    std::string mode;
    long width = 0;
    bool frequencyValid = false, modeValid = false;
};
struct Snapshot {
    bool portOpen = false, online = false, demo = false, busy = false;
    bool poweredOff = false, powerKnown = false, transmitting = false, pttKnown = false;
    // A silent direct CAT connection uses the standby display without
    // inventing a confirmed power state or discarding PTT release ownership.
    bool standby = false;
    bool canPower = false, canPtt = false;
    bool dashboardPtt = false;
    bool releasePending = false;
    std::string connectionStatus = "Disconnected", model = "Yaesu FT-710", endpoint, error;
    Vfo a, b;
    vfo_t activeVfo = RIG_VFO_A, settingsVfo = RIG_VFO_CURR;
    std::vector<std::string> modes;
    std::vector<Setting> settings;
    std::chrono::steady_clock::time_point lastContact{};
    std::uint64_t generation = 0;
    std::array<std::uint64_t, 2> frequencyRevision{}, modeRevision{};
    std::uint64_t vfoRevision = 0;
    std::map<std::string, std::uint64_t> settingRevision;
};
enum class Action { Connect, Disconnect, PowerOn, PowerOff, Set, Frequency, StepFrequency, Mode, SelectVfo, Swap, CopyAtoB, PttOn, PttOff, Refresh, SettingsVfo, RawQuery, RawWrite, Cat };
struct Command {
    Action action = Action::Refresh;
    Connection connection;
    std::string id, text;
    double number = 0;
    long width = 0;
    vfo_t vfo = RIG_VFO_CURR;
    std::uint64_t generation = 0;
    std::uint64_t uiRequest = 0;
    std::chrono::steady_clock::time_point queuedAt{};
    std::shared_ptr<CatJob> cat;
};
class Worker {
public:
    using Update = std::function<void(const Snapshot&)>;
    using Log = std::function<void(const std::string&)>;
    Worker(Update update, Log log);
    ~Worker();
    void submit(Command command);
    std::future<CatReply> request(std::shared_ptr<CatJob> job);
    void stop();
private:
    Update update_;
    Log log_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    bool stopping_ = false;
    RIG* rig_ = nullptr;
    Snapshot state_;
    Connection connection_;
    std::size_t slowIndex_ = 0;
    std::size_t meterIndex_ = 0;
    std::size_t controlIndex_ = 0, fastIndex_ = 0;
    enum class PollStage { FrequencyA, ActiveVfo, Meters, Controls, FrequencyB, ModeA, ModeB, Functions, Slow };
    PollStage pollStage_ = PollStage::FrequencyA;
    int missed_ = 0;
    bool heldPtt_ = false;
    std::uint64_t catPttOwner_ = 0;
    std::chrono::steady_clock::time_point pttDeadline_{};
    std::chrono::steady_clock::time_point releaseDeadline_{};
    int releaseTransmit(const std::string& reason);
    void queueInteractive(Command command); // mutex_ held; preserve writes and this client's command order.
    void run();
    void execute(const Command& command);
    void connect(const Connection& connection);
    void disconnect();
    void discover();
    bool poll(); // False means a queued command interrupted the scan; resume it next time.
    void pollMeters(bool publishUpdate = true);
    bool pollControls();
    bool urgentCommandPending();
    void resetPoll();
    void read(Setting& setting);
    void write(Setting& setting, const std::string& value);
    void publish();
    void report(const std::string& operation, int result);
    CatReply executeCat(const CatRequest& request);
    int power(bool on);
    void markPoweredOff();
    int rfPowerMaximum();
};
std::string formatNumber(double value);
std::string rangeText(const Setting& setting);
std::string displayValue(const Setting& setting);
const Setting* findSetting(const Snapshot& state, const std::string& id);
bool isMeter(setting_t level);
bool isTransmitMeter(setting_t level);
}
