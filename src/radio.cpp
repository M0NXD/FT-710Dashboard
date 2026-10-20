#include "radio.hpp"
#include <algorithm>
#include <array>
#include <hamlib/rig_state.h>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace radio {
using Clock = std::chrono::steady_clock;
namespace {
std::string safe(const char* text) { return text ? text : ""; }
std::string briefError(int result) {
    auto text = safe(rigerror2(result));
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}
double numeric(const std::string& text) {
    std::size_t consumed = 0;
    double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value))
        throw std::invalid_argument("Enter a finite number without units.");
    return value;
}
bool changesRadio(const Command& command) {
    if (command.cat) {
        const auto& name = command.cat->request.name;
        return name.starts_with("set_") || name == "vfo_op" || name == "__release_client";
    }
    // Keep UI connection/selection changes as barriers as well as writes.
    return command.action != Action::Refresh && command.action != Action::RawQuery;
}
bool fast(const Setting& setting) {
    if (setting.kind == Kind::Function) return (setting.mask &
        (RIG_FUNC_NB | RIG_FUNC_NR | RIG_FUNC_ANF | RIG_FUNC_APF | RIG_FUNC_MON |
         RIG_FUNC_COMP | RIG_FUNC_VOX | RIG_FUNC_LOCK | RIG_FUNC_TUNER)) != 0;
    return setting.kind == Kind::Level && (setting.mask &
        (RIG_LEVEL_STRENGTH | RIG_LEVEL_RFPOWER | RIG_LEVEL_RFPOWER_METER |
         RIG_LEVEL_RFPOWER_METER_WATTS | RIG_LEVEL_SWR | RIG_LEVEL_ALC |
         RIG_LEVEL_COMP_METER |
         RIG_LEVEL_VD_METER | RIG_LEVEL_ID_METER | RIG_LEVEL_AF | RIG_LEVEL_RF |
         RIG_LEVEL_MICGAIN | RIG_LEVEL_SQL | RIG_LEVEL_IF | RIG_LEVEL_NR | RIG_LEVEL_NB | RIG_LEVEL_PREAMP));
}
std::string nameFor(const std::string& name) {
    static const std::map<std::string, std::string> names{
        {"AF", "AF / audio gain"}, {"RF", "RF gain"}, {"RFPOWER", "RF power setting"},
        {"STRENGTH", "Signal strength"}, {"RFPOWER_METER", "RF output (relative)"},
        {"ALC", "ALC meter"}, {"SWR", "SWR meter"}, {"COMP_METER", "Compression meter"},
        {"RFPOWER_METER_WATTS", "RF output"}, {"VD_METER", "Supply voltage"},
        {"ID_METER", "Drain current"}, {"MICGAIN", "Microphone gain"},
        {"SQL", "Squelch"}, {"AGC", "AGC"}, {"IF", "IF shift"},
        {"NR", "Noise reduction"}, {"NB", "Noise blanker"}, {"ANF", "Auto notch"},
        {"NOTCHF", "Notch frequency"}, {"KEYSPD", "CW key speed"},
        {"CWPITCH", "CW pitch"}, {"PREAMP", "Preamp"}, {"ATT", "Attenuator"},
        {"COMP", "Speech processor"}, {"MON", "Monitor"}, {"LOCK", "Dial lock"},
        {"TUNER", "Antenna tuner"}, {"MONITOR_GAIN", "Monitor gain"},
        {"BKINDL", "CW break-in delay"}, {"VOXDELAY", "VOX delay"},
        {"VOXGAIN", "VOX gain"}, {"ANTIVOX", "Anti VOX"}, {"APF", "Audio peak filter"}
    };
    auto it = names.find(name);
    return it == names.end() ? name : it->second;
}
void addChoices(Setting& setting, const std::vector<std::pair<int, std::string>>& choices) {
    setting.type = ValueType::Choice;
    for (const auto& [value, label] : choices) {
        setting.choiceValues.push_back(std::to_string(value));
        setting.choices.push_back(label);
    }
}
Setting fromConf(const confparams& conf, Kind kind, bool get, bool set) {
    Setting s;
    s.kind = kind;
    s.token = conf.token;
    s.name = safe(conf.label);
    if (s.name.empty()) s.name = safe(conf.name);
    s.description = safe(conf.tooltip);
    s.id = (kind == Kind::Configuration ? "config." :
            kind == Kind::ExtLevel ? "extlevel." :
            kind == Kind::ExtFunction ? "extfunc." : "extparm.") + safe(conf.name);
    s.group = kind == Kind::Configuration ? "Hamlib configuration" : "Extended controls";
    s.readable = get;
    s.writable = set;
    switch (conf.type) {
    case RIG_CONF_NUMERIC:
        s.type = ValueType::Real;
        s.minimum = conf.u.n.min;
        s.maximum = conf.u.n.max;
        s.step = conf.u.n.step;
        s.bounded = s.maximum > s.minimum;
        break;
    case RIG_CONF_INT: s.type = ValueType::Integer; break;
    case RIG_CONF_CHECKBUTTON: s.type = ValueType::Boolean; break;
    case RIG_CONF_COMBO:
        s.type = ValueType::Choice;
        for (int i = 0; i < RIG_COMBO_MAX && conf.u.c.combostr[i]; ++i) {
            s.choices.emplace_back(conf.u.c.combostr[i]);
            s.choiceValues.push_back(kind == Kind::Configuration ? conf.u.c.combostr[i] : std::to_string(i));
        }
        break;
    case RIG_CONF_STRING: s.type = ValueType::Text; break;
    default:
        s.type = ValueType::Text;
        s.readable = s.writable = false;
        s.status = "Binary/action token; use CAT console";
        break;
    }
    return s;
}
int collectConfig(const confparams* conf, rig_ptr_t data) {
    auto* settings = static_cast<std::vector<Setting>*>(data);
    settings->push_back(fromConf(*conf, Kind::Configuration, true, false));
    return 1;
}
}

std::string formatNumber(double value) {
    std::ostringstream result;
    result << std::fixed << std::setprecision(4) << value;
    auto text = result.str();
    while (text.size() > 1 && text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}
std::string rangeText(const Setting& setting) {
    if (setting.type == ValueType::Boolean) return "Off / On";
    if (!setting.choices.empty()) {
        std::string text;
        for (const auto& choice : setting.choices) {
            if (!text.empty()) text += ", ";
            text += choice;
        }
        return text;
    }
    if (!setting.bounded) return setting.units;
    return formatNumber(setting.minimum) + " to " + formatNumber(setting.maximum) +
        (setting.units.empty() ? "" : " " + setting.units);
}
std::string displayValue(const Setting& setting) {
    if (!setting.valid) return "--";
    if (setting.type == ValueType::Boolean) return setting.value == "0" ? "Off" : "On";
    for (std::size_t i = 0; i < setting.choiceValues.size(); ++i)
        if (setting.value == setting.choiceValues[i]) return setting.choices[i];
    return setting.value + (setting.units.empty() ? "" : " " + setting.units);
}
const Setting* findSetting(const Snapshot& state, const std::string& id) {
    auto it = std::find_if(state.settings.begin(), state.settings.end(), [&](const Setting& s) { return s.id == id; });
    return it == state.settings.end() ? nullptr : &*it;
}
bool isTransmitMeter(setting_t level) {
    return (level & (RIG_LEVEL_RFPOWER_METER | RIG_LEVEL_RFPOWER_METER_WATTS |
        RIG_LEVEL_SWR | RIG_LEVEL_ALC | RIG_LEVEL_COMP_METER | RIG_LEVEL_ID_METER)) != 0;
}
bool isMeter(setting_t level) {
    return isTransmitMeter(level) || (level & (RIG_LEVEL_STRENGTH | RIG_LEVEL_RAWSTR | RIG_LEVEL_VD_METER)) != 0;
}

Worker::Worker(Update update, Log log) : update_(std::move(update)), log_(std::move(log)) {
    thread_ = std::thread([this] { run(); });
}
Worker::~Worker() { stop(); }
void Worker::stop() {
    {
        std::lock_guard lock(mutex_); stopping_ = true;
        for (auto& command : commands_) if (command.cat) {
            CatReply reply; reply.result = -RIG_EIO; command.cat->reply.set_value(std::move(reply));
        }
        commands_.clear();
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}
void Worker::submit(Command command) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        command.queuedAt = Clock::now();
        // Merge only adjacent UI tuning requests. CAT jobs and other controls
        // keep their queue order; relative steps are never based on a stale UI read.
        if (!commands_.empty() && (command.action == Action::StepFrequency || command.action == Action::Frequency)) {
            auto& last = commands_.back();
            if (last.action == command.action && last.vfo == command.vfo && last.generation == command.generation) {
                command.number = command.action == Action::StepFrequency ? last.number + command.number : command.number;
                commands_.back() = std::move(command);
                wake_.notify_one();
                return;
            }
        }
        if (command.action == Action::PttOff)
            commands_.erase(std::remove_if(commands_.begin(), commands_.end(), [](const Command& pending) {
                return pending.action == Action::PttOn;
            }), commands_.end());
        // Release requests have priority over queued UI operations.
        if (command.action == Action::PttOff || command.action == Action::Disconnect)
            commands_.push_front(std::move(command));
        else if (changesRadio(command)) queueInteractive(std::move(command));
        else commands_.push_back(std::move(command));
    }
    wake_.notify_one();
}
void Worker::queueInteractive(Command command) {
    // Tuning, mode and PTT need the same priority for GUI and NET clients.
    // Do not overtake another write or a read belonging to this client.
    auto position = commands_.begin();
    for (auto it = commands_.begin(); it != commands_.end(); ++it)
        if (changesRadio(*it) || (command.cat && it->cat && command.cat->request.client == it->cat->request.client))
            position = std::next(it);
    commands_.insert(position, std::move(command));
}
std::future<CatReply> Worker::request(std::shared_ptr<CatJob> job) {
    auto future = job->reply.get_future();
    {
        std::lock_guard lock(mutex_);
        bool release = job->request.name == "__release_client";
        if (stopping_ || (!release && commands_.size() >= 128)) {
            CatReply reply; reply.result = stopping_ ? -RIG_EIO : -RIG_ELIMIT;
            job->reply.set_value(std::move(reply)); return future;
        }
        Command command; command.action = Action::Cat; command.cat = std::move(job); command.queuedAt = Clock::now();
        if (release) {
            for (auto& pending : commands_) if (pending.cat && pending.cat->request.client == command.cat->request.client)
                pending.cat->cancelled = true;
            commands_.push_front(std::move(command));
        } else if (changesRadio(command)) queueInteractive(std::move(command));
        else commands_.push_back(std::move(command));
    }
    wake_.notify_one();
    return future;
}
void Worker::publish() { state_.dashboardPtt = heldPtt_; update_(state_); }
int Worker::releaseTransmit(const std::string& reason) {
    const int result = rig_set_ptt(rig_, RIG_VFO_CURR, RIG_PTT_OFF);
    report(reason, result);
    state_.releasePending = result != RIG_OK;
    if (state_.releasePending) releaseDeadline_ = Clock::now() + std::chrono::seconds(1);
    else {
        heldPtt_ = false; catPttOwner_ = 0;
        state_.pttKnown = true; state_.transmitting = false;
        for (auto& setting : state_.settings) if (setting.txOnly) { setting.valid = false; setting.status = "TX only"; }
    }
    return result;
}
void Worker::report(const std::string& operation, int result) {
    if (result == RIG_OK) {
        log_(operation + " succeeded");
        state_.error.clear();
    } else {
        state_.error = operation + ": " + briefError(result);
        log_(operation + ": " + safe(rigerror(result)));
    }
}
void Worker::run() {
    rig_set_debug(RIG_DEBUG_NONE);
    rig_load_all_backends();
    auto nextPoll = Clock::now();
    auto nextMeters = Clock::now();
    bool catBurst = false;
    auto catQuietUntil = Clock::time_point{}, catBurstDeadline = Clock::time_point{};
    while (true) {
        Command command;
        bool hasCommand = false;
        {
            std::unique_lock lock(mutex_);
            auto deadline = std::min(nextPoll, nextMeters);
            // Let the client send its next setup/readback command before a
            // background read starts. Never hold a continuous burst over 200 ms.
            if (catQuietUntil > Clock::now()) deadline = std::max(deadline, catQuietUntil);
            if (heldPtt_ && !state_.releasePending) deadline = std::min(deadline, pttDeadline_);
            if (state_.releasePending) deadline = std::min(deadline, releaseDeadline_);
            wake_.wait_until(lock, deadline, [&] { return stopping_ || !commands_.empty(); });
            if (stopping_) break;
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                hasCommand = true;
            }
        }
        try {
            if ((state_.releasePending && Clock::now() >= releaseDeadline_) ||
                (!state_.releasePending && heldPtt_ && Clock::now() >= pttDeadline_)) {
                releaseTransmit(state_.releasePending ? "Retry PTT release" : "Dashboard PTT timeout / release");
                publish();
            }
            if (hasCommand && command.cat) {
                CatReply reply;
                const auto started = Clock::now();
                try {
                    if (command.cat->cancelled) reply.result = -RIG_ETIMEOUT;
                    else reply = executeCat(command.cat->request);
                } catch (const std::invalid_argument& error) {
                    reply.result = -RIG_EINVAL;
                    log_("CAT client " + std::to_string(command.cat->request.client) + ": " + command.cat->request.name + ": " + error.what());
                } catch (const std::out_of_range& error) {
                    reply.result = -RIG_EINVAL;
                    log_("CAT client " + std::to_string(command.cat->request.client) + ": " + command.cat->request.name + ": " + error.what());
                } catch (...) { reply.result = -RIG_EINTERNAL; }
                bool changed = reply.changed;
                command.cat->reply.set_value(std::move(reply));
                const auto finished = Clock::now();
                const auto queuedMs = std::chrono::duration_cast<std::chrono::milliseconds>(started - command.queuedAt).count();
                const auto radioMs = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
                if (queuedMs + radioMs >= 250 && command.cat->request.name != "__release_client")
                    log_("CAT client " + std::to_string(command.cat->request.client) + ": " + command.cat->request.name +
                        " took " + std::to_string(queuedMs + radioMs) + " ms (queue " + std::to_string(queuedMs) +
                        " ms, radio " + std::to_string(radioMs) + " ms).");
                if (!catBurst) { catBurst = true; catBurstDeadline = finished + std::chrono::milliseconds(200); }
                catQuietUntil = std::min(finished + std::chrono::milliseconds(20), catBurstDeadline);
                if (changed) { nextPoll = Clock::now(); publish(); }
            } else if (hasCommand) {
                catBurst = false; catQuietUntil = Clock::time_point{};
                execute(command);
            }
            // Keep the PTT acknowledgement path short. The radio command has
            // already returned successfully; defer the next full settings scan
            // until after an initial meter refresh.
            if (hasCommand && command.action == Action::PttOn && heldPtt_) {
                nextPoll = Clock::now() + std::chrono::milliseconds(connection_.pollMs);
                nextMeters = Clock::now() + std::chrono::milliseconds(250);
            }
            if (Clock::now() < catQuietUntil) continue;
            catBurst = false;
            if (Clock::now() >= nextPoll) {
                const bool complete = !rig_ || !state_.portOpen || poll();
                // Resume interrupted scans after servicing the queued command.
                // Restarting at frequency A starves later controls under CAT traffic.
                nextPoll = complete ? Clock::now() + std::chrono::milliseconds(state_.online ? connection_.pollMs : 2000) : Clock::now();
                if (complete) nextMeters = Clock::now() + std::chrono::milliseconds(state_.transmitting ? 250 : 500);
            } else if (Clock::now() >= nextMeters) {
                if (rig_ && state_.online && !state_.poweredOff) pollMeters();
                nextMeters = Clock::now() + std::chrono::milliseconds(state_.transmitting ? 250 : 500);
            }
        } catch (const std::exception& e) {
            state_.busy = false;
            state_.error = e.what();
            log_(state_.error);
            publish();
        }
    }
    disconnect();
}
void Worker::disconnect() {
    if (rig_) {
        if (heldPtt_ || catPttOwner_ || state_.releasePending) releaseTransmit("Disconnect PTT release");
        if (state_.portOpen) rig_close(rig_);
        rig_cleanup(rig_);
        rig_ = nullptr;
    }
    heldPtt_ = false;
    catPttOwner_ = 0;
    auto generation = state_.generation + 1;
    state_ = Snapshot{};
    state_.generation = generation;
    resetPoll();
    publish();
}
void Worker::connect(const Connection& connection) {
    disconnect();
    connection_ = connection;
    state_.busy = true;
    state_.connectionStatus = "Opening connection...";
    state_.endpoint = connection.endpoint;
    state_.demo = connection.type == ConnectionType::Demo;
    publish();
    auto model = connection.type == ConnectionType::Serial ? RIG_MODEL_FT710 :
        connection.type == ConnectionType::Network ? RIG_MODEL_NETRIGCTL : RIG_MODEL_DUMMY;
    rig_ = rig_init(model);
    if (!rig_) throw std::runtime_error("Hamlib could not initialize the selected radio driver.");
    auto config = [&](const char* key, const std::string& value, bool required = true) {
        auto token = rig_token_lookup(rig_, key);
        if (token == RIG_CONF_END) {
            if (required) throw std::runtime_error(std::string("Hamlib configuration unavailable: ") + key);
            return;
        }
        auto result = rig_set_conf(rig_, token, value.c_str());
        if (result != RIG_OK && required) {
            log_(std::string(key) + ": " + safe(rigerror(result)));
            throw std::runtime_error(std::string(key) + ": " + briefError(result));
        }
    };
    config("auto_power_on", "0", false);
    config("auto_power_off", "0", false);
    if (connection.type != ConnectionType::Demo) {
        if (connection.endpoint.empty()) throw std::runtime_error("Enter the Enhanced CAT COM port or rigctld host:port.");
        config("rig_pathname", connection.endpoint);
        config("timeout", "400");
        config("retry", "0");
    }
    if (connection.type == ConnectionType::Serial) {
        config("serial_speed", std::to_string(connection.baud));
        config("stop_bits", std::to_string(connection.stopBits));
        config("serial_handshake", connection.handshake);
        config("rts_state", connection.handshake == "Hardware" ? "Unset" : "OFF", false);
        config("dtr_state", "OFF", false);
    }
    int result = rig_open(rig_);
    if (result != RIG_OK) {
        state_.busy = false;
        state_.connectionStatus = "Connection failed";
        report("Open radio", result);
        rig_cleanup(rig_);
        rig_ = nullptr;
        publish();
        return;
    }
    state_.portOpen = true;
    if (connection.type == ConnectionType::Serial) {
        // The 4.7.2 FT-710 descriptor inherited 200 W / 75 W ranges from
        // another Yaesu. Correct this instance's advertised power ranges.
        auto& ranges = HAMLIB_STATE(rig_)->tx_range_list;
        for (auto& range : ranges) {
            if (RIG_IS_FRNG_END(range)) break;
            bool amOnly = (range.modes & ~(RIG_MODE_AM | RIG_MODE_AMN)) == 0;
            if (!amOnly) range.modes &= ~(RIG_MODE_AM | RIG_MODE_AMN);
            range.low_power = 5000;
            range.high_power = amOnly ? 25000 : 100000;
        }
    }
    state_.canPower = rig_->caps->set_powerstat != nullptr;
    state_.canPtt = rig_->caps->set_ptt != nullptr;
    state_.canTune = rig_->caps->vfo_op && (rig_->caps->vfo_ops & RIG_OP_TUNE);
    state_.model = state_.demo ? "Hamlib Dummy / demo" : "Yaesu FT-710";
    if (state_.demo) {
        rig_set_freq(rig_, RIG_VFO_A, 14200000);
        rig_set_freq(rig_, RIG_VFO_B, 7074000);
        rig_set_mode(rig_, RIG_VFO_A, RIG_MODE_USB, 2400);
        rig_set_mode(rig_, RIG_VFO_B, RIG_MODE_PKTUSB, 3000);
        value_t value{};
        value.f = 0.5f;
        rig_set_level(rig_, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, value);
        value.f = 0.35f;
        rig_set_level(rig_, RIG_VFO_CURR, RIG_LEVEL_AF, value);
    }
    discover();
    slowIndex_ = 0;
    meterIndex_ = 0;
    missed_ = 0;
    if (state_.error.starts_with("Frequency read:")) state_.error.clear();
    state_.busy = false;
    log_(state_.demo ? "Connected to Hamlib Dummy. No physical radio is controlled." : "CAT connection opened: " + connection.endpoint);
    poll();
}
void Worker::discover() {
    state_.settings.clear();
    state_.modes.clear();
    // NET rigctl uses the known FT-710 descriptor for labels/ranges and extension tokens.
    const rig_caps* caps = connection_.type == ConnectionType::Network ? rig_get_caps(RIG_MODEL_FT710) : rig_->caps;
    if (!caps) caps = rig_->caps;
    rmode_t modes = 0;
    for (const auto& range : caps->rx_range_list1) modes |= range.modes;
    for (int i = 0; i < 64; ++i) {
        setting_t bit = setting_t(1) << i;
        if (modes & bit) {
            auto name = safe(rig_strrmode(bit));
            if (!name.empty()) state_.modes.push_back(name);
        }
        for (Kind kind : {Kind::Level, Kind::Function, Kind::Parameter}) {
            bool get = kind == Kind::Level ? (caps->has_get_level & bit) != 0 :
                kind == Kind::Function ? (caps->has_get_func & bit) != 0 : (caps->has_get_parm & bit) != 0;
            bool set = kind == Kind::Level ? (caps->has_set_level & bit) != 0 :
                kind == Kind::Function ? (caps->has_set_func & bit) != 0 : (caps->has_set_parm & bit) != 0;
            if (!get && !set) continue;
            auto key = safe(kind == Kind::Level ? rig_strlevel(bit) : kind == Kind::Function ? rig_strfunc(bit) : rig_strparm(bit));
            if (key.empty()) continue;
            Setting s;
            s.id = (kind == Kind::Level ? "level." : kind == Kind::Function ? "func." : "parm.") + key;
            s.name = nameFor(key);
            s.description = key;
            s.kind = kind;
            s.mask = bit;
            s.group = kind == Kind::Level ? "Levels / meters" : kind == Kind::Function ? "Functions" : "Parameters";
            s.readable = get;
            s.writable = set;
            s.type = kind == Kind::Function ? ValueType::Boolean :
                kind == Kind::Level ? (RIG_LEVEL_IS_FLOAT(bit) ? ValueType::Real : ValueType::Integer) :
                RIG_PARM_IS_STRING(bit) ? ValueType::Text : RIG_PARM_IS_FLOAT(bit) ? ValueType::Real : ValueType::Integer;
            if (kind != Kind::Function && s.type != ValueType::Text) {
                const gran_t& gran = kind == Kind::Level ? caps->level_gran[i] : caps->parm_gran[i];
                bool real = s.type == ValueType::Real;
                s.minimum = real ? gran.min.f : gran.min.i;
                s.maximum = real ? gran.max.f : gran.max.i;
                s.step = real ? gran.step.f : gran.step.i;
                s.bounded = std::isfinite(s.minimum) && std::isfinite(s.maximum) && s.maximum > s.minimum;
                if (real && !s.bounded && set) { s.minimum = 0; s.maximum = 1; s.bounded = true; }
            }
            if (kind == Kind::Level) {
                s.txOnly = isTransmitMeter(bit);
                if (bit == RIG_LEVEL_PREAMP || bit == RIG_LEVEL_ATT) {
                    std::vector<std::pair<int, std::string>> values{{0, "Off"}};
                    const int* list = bit == RIG_LEVEL_PREAMP ? caps->preamp : caps->attenuator;
                    for (int n = 0; n < 8 && list[n]; ++n) values.emplace_back(list[n], std::to_string(list[n]) + " dB");
                    addChoices(s, values);
                }
                if (bit == RIG_LEVEL_AGC) {
                    std::vector<std::pair<int, std::string>> values;
                    for (int n = 0; n < caps->agc_level_count; ++n) {
                        int code = caps->agc_levels[n];
                        values.emplace_back(code, safe(rig_stragclevel(static_cast<agc_level_e>(code))));
                    }
                    if (!values.empty()) addChoices(s, values);
                }
                if (bit == RIG_LEVEL_STRENGTH) s.units = "dB rel. S9";
                if (bit == RIG_LEVEL_VD_METER) s.units = "V";
                if (bit == RIG_LEVEL_ID_METER) s.units = "A";
                if (bit == RIG_LEVEL_RFPOWER_METER_WATTS) s.units = "W";
                if (bit == RIG_LEVEL_COMP_METER) s.units = "dB";
                if (bit == RIG_LEVEL_RFPOWER) s.description = "Configured RF power limit, normalized 0–1 in Hamlib. The main dashboard uses watts (5–100 W). This is separate from measured RF output.";
                if (bit == RIG_LEVEL_ALC) s.description = "Relative ALC indication from Hamlib. The dashboard shows percent of the driver's reference scale; it is not RF power or a percentage of watts.";
                if (bit == RIG_LEVEL_KEYSPD) s.units = "WPM";
                if (bit == RIG_LEVEL_IF || bit == RIG_LEVEL_CWPITCH || bit == RIG_LEVEL_NOTCHF) s.units = "Hz";
            }
            if (kind == Kind::Parameter && bit == RIG_PARM_BANDSELECT && caps->parm_gran[i].step.s) {
                s.type = ValueType::Choice;
                std::stringstream choices(caps->parm_gran[i].step.s);
                std::string value;
                while (std::getline(choices, value, ',')) { s.choices.push_back(value); s.choiceValues.push_back(value); }
            }
            state_.settings.push_back(std::move(s));
        }
    }
    auto addExt = [&](const confparams* list, Kind kind, bool get, bool set) {
        if (!list) return;
        for (int i = 0; list[i].token != RIG_CONF_END; ++i)
            state_.settings.push_back(fromConf(list[i], kind, get, set));
    };
    addExt(caps->extlevels, Kind::ExtLevel, caps->get_ext_level, caps->set_ext_level);
    addExt(caps->extfuncs, Kind::ExtFunction, caps->get_ext_func, caps->set_ext_func);
    addExt(caps->extparms, Kind::ExtParameter, caps->get_ext_parm, caps->set_ext_parm);
    auto special = [&](std::string id, std::string name, ValueType type, bool get, bool set, double min = 0, double max = 0) -> Setting& {
        Setting s;
        s.id = "radio." + id; s.name = std::move(name); s.group = "Radio / VFO";
        s.kind = Kind::Special; s.type = type; s.readable = get; s.writable = set;
        s.minimum = min; s.maximum = max; s.bounded = max > min;
        state_.settings.push_back(s);
        return state_.settings.back();
    };
    special("RIT", "RIT offset (Hz)", ValueType::Integer, caps->get_rit, caps->set_rit, -caps->max_rit, caps->max_rit);
    special("XIT", "XIT offset (Hz)", ValueType::Integer, caps->get_xit, caps->set_xit, -caps->max_xit, caps->max_xit);
    special("SPLIT", "Split RX/TX", ValueType::Boolean, caps->get_split_vfo, caps->set_split_vfo);
    special("STEP", "Radio tuning step (Hz)", ValueType::Integer, caps->get_ts, caps->set_ts, 1, 1000000);
    special("MEM", "Memory channel", ValueType::Integer, caps->get_mem, caps->set_mem, 1, 99);
    special("CTCSS", "CTCSS tone (tenths of Hz)", ValueType::Integer, caps->get_ctcss_tone, caps->set_ctcss_tone, 0, 2541);
    special("CTCSS_SQL", "CTCSS squelch (tenths of Hz)", ValueType::Integer, caps->get_ctcss_sql, caps->set_ctcss_sql, 0, 2541);
    special("INFO", "Radio / firmware information", ValueType::Text, caps->get_info, false);
    special("CLOCK", "Radio date and time", ValueType::Text, caps->get_clock, false);
    special("RPTR_OFFSET", "Repeater offset (Hz)", ValueType::Integer, caps->get_rptr_offs, caps->set_rptr_offs, 0, 10000000);
    auto& shift = special("RPTR_SHIFT", "Repeater shift", ValueType::Choice, caps->get_rptr_shift, caps->set_rptr_shift);
    addChoices(shift, {{RIG_RPT_SHIFT_NONE, "Simplex"}, {RIG_RPT_SHIFT_MINUS, "Minus"}, {RIG_RPT_SHIFT_PLUS, "Plus"}});
    state_.settings.erase(std::remove_if(state_.settings.begin(), state_.settings.end(), [](const Setting& s) {
        return s.kind == Kind::Special && !s.readable && !s.writable;
    }), state_.settings.end());
    rig_token_foreach(rig_, collectConfig, &state_.settings);
    for (auto& s : state_.settings) if (s.kind == Kind::Configuration) read(s);
    log_("Discovered " + std::to_string(state_.settings.size()) + " Hamlib settings. Slow settings refresh in rotation.");
}
void Worker::read(Setting& s) {
    if (!s.readable) return;
    if (s.txOnly && (!state_.pttKnown || !state_.transmitting)) {
        s.valid = false; s.status = state_.pttKnown ? "TX only" : "TX status unavailable"; return;
    }
    value_t value{};
    char buffer[1024]{};
    if (s.type == ValueType::Text || (s.kind == Kind::Parameter && RIG_PARM_IS_STRING(s.mask))) value.s = buffer;
    int result = -RIG_ENAVAIL;
    switch (s.kind) {
    case Kind::Level:
        // Meters and PC power control are global FT-710 values. Targeting the
        // All settings VFO could cause Hamlib to switch VFO during a TX read.
        result = rig_get_level(rig_, isMeter(s.mask) || s.mask == RIG_LEVEL_RFPOWER ? RIG_VFO_CURR : state_.settingsVfo, s.mask, &value);
        break;
    case Kind::Function: result = rig_get_func(rig_, state_.settingsVfo, s.mask, &value.i); break;
    case Kind::Parameter: result = rig_get_parm(rig_, s.mask, &value); break;
    case Kind::ExtLevel: result = rig_get_ext_level(rig_, state_.settingsVfo, s.token, &value); break;
    case Kind::ExtFunction: result = rig_get_ext_func(rig_, state_.settingsVfo, s.token, &value.i); break;
    case Kind::ExtParameter: result = rig_get_ext_parm(rig_, s.token, &value); break;
    case Kind::Configuration:
        result = rig_get_conf2(rig_, s.token, buffer, sizeof buffer);
        if (result == RIG_OK) s.value = buffer;
        break;
    case Kind::Special: {
        shortfreq_t offset = 0;
        tone_t tone = 0;
        split_t split = RIG_SPLIT_OFF;
        vfo_t tx = RIG_VFO_B;
        rptr_shift_t shift = RIG_RPT_SHIFT_NONE;
        if (s.id == "radio.RIT") { result = rig_get_rit(rig_, state_.settingsVfo, &offset); value.i = static_cast<int>(offset); }
        if (s.id == "radio.XIT") { result = rig_get_xit(rig_, state_.settingsVfo, &offset); value.i = static_cast<int>(offset); }
        if (s.id == "radio.SPLIT") { result = rig_get_split_vfo(rig_, RIG_VFO_CURR, &split, &tx); value.i = split; }
        if (s.id == "radio.STEP") { result = rig_get_ts(rig_, state_.settingsVfo, &offset); value.i = static_cast<int>(offset); }
        if (s.id == "radio.MEM") result = rig_get_mem(rig_, state_.settingsVfo, &value.i);
        if (s.id == "radio.CTCSS") { result = rig_get_ctcss_tone(rig_, state_.settingsVfo, &tone); value.i = static_cast<int>(tone); }
        if (s.id == "radio.CTCSS_SQL") { result = rig_get_ctcss_sql(rig_, state_.settingsVfo, &tone); value.i = static_cast<int>(tone); }
        if (s.id == "radio.RPTR_OFFSET") { result = rig_get_rptr_offs(rig_, state_.settingsVfo, &offset); value.i = static_cast<int>(offset); }
        if (s.id == "radio.RPTR_SHIFT") { result = rig_get_rptr_shift(rig_, state_.settingsVfo, &shift); value.i = shift; }
        if (s.id == "radio.INFO") {
            const char* info = rig_get_info(rig_);
            result = info ? RIG_OK : -RIG_ENAVAIL;
            if (info) { s.value = info; }
        }
        if (s.id == "radio.CLOCK") {
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, zone = 0;
            double millisecond = 0;
            result = rig_get_clock(rig_, &year, &month, &day, &hour, &minute, &second, &millisecond, &zone);
            if (result == RIG_OK) {
                std::ostringstream text;
                text << year << '-' << std::setw(2) << std::setfill('0') << month << '-' << std::setw(2) << day << ' '
                    << std::setw(2) << hour << ':' << std::setw(2) << minute << ':' << std::setw(2) << second;
                s.value = text.str();
            }
        }
        break;
    }
    }
    s.valid = result == RIG_OK;
    if (s.valid && s.type == ValueType::Real && !std::isfinite(value.f)) {
        s.valid = false; s.status = "Invalid numeric response"; return;
    }
    s.status = result == RIG_OK ? "Live" : briefError(result);
    if (!s.valid) return;
    s.updated = Clock::now();
    if (s.kind == Kind::Configuration || s.id == "radio.INFO" || s.id == "radio.CLOCK") return;
    if (s.type == ValueType::Text || (s.kind == Kind::Parameter && RIG_PARM_IS_STRING(s.mask)))
        s.value = value.s ? value.s : "";
    else if (s.type == ValueType::Real) s.value = formatNumber(value.f);
    else s.value = std::to_string(value.i);
}
void Worker::write(Setting& s, const std::string& text) {
    if (!s.writable) throw std::invalid_argument("This setting is read only.");
    value_t value{};
    bool string = s.type == ValueType::Text || (s.kind == Kind::Parameter && RIG_PARM_IS_STRING(s.mask));
    if (!s.choiceValues.empty() && std::find(s.choiceValues.begin(), s.choiceValues.end(), text) == s.choiceValues.end())
        throw std::invalid_argument("Choose one of the supported values.");
    if (string) value.cs = text.c_str();
    else {
        double number = numeric(text);
        if (s.bounded && (number < s.minimum - 0.000001 || number > s.maximum + 0.000001))
            throw std::invalid_argument("Value is outside the range: " + rangeText(s));
        if (s.type == ValueType::Boolean && number != 0 && number != 1)
            throw std::invalid_argument("A switch value must be 0 or 1.");
        if (s.type == ValueType::Real) value.f = static_cast<float>(number);
        else {
            if (std::trunc(number) != number || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
                throw std::invalid_argument("Enter a whole number within the supported range.");
            value.i = static_cast<int>(number);
        }
    }
    int result = -RIG_ENAVAIL;
    if (s.kind == Kind::Level && s.mask == RIG_LEVEL_RFPOWER && value.f * 100 > rfPowerMaximum() + .001)
        throw std::invalid_argument("AM transmit power is limited to 25 W. Enter 5 to 25 W.");
    switch (s.kind) {
    case Kind::Level: result = rig_set_level(rig_, s.mask == RIG_LEVEL_RFPOWER ? RIG_VFO_CURR : state_.settingsVfo, s.mask, value); break;
    case Kind::Function: result = rig_set_func(rig_, state_.settingsVfo, s.mask, value.i); break;
    case Kind::Parameter: result = rig_set_parm(rig_, s.mask, value); break;
    case Kind::ExtLevel: result = rig_set_ext_level(rig_, state_.settingsVfo, s.token, value); break;
    case Kind::ExtFunction: result = rig_set_ext_func(rig_, state_.settingsVfo, s.token, value.i); break;
    case Kind::ExtParameter: result = rig_set_ext_parm(rig_, s.token, value); break;
    case Kind::Configuration: break;
    case Kind::Special:
        if (s.id == "radio.RIT") result = rig_set_rit(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.XIT") result = rig_set_xit(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.SPLIT") result = rig_set_split_vfo(rig_, state_.activeVfo, value.i ? RIG_SPLIT_ON : RIG_SPLIT_OFF,
            state_.activeVfo == RIG_VFO_A ? RIG_VFO_B : RIG_VFO_A);
        if (s.id == "radio.STEP") result = rig_set_ts(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.MEM") result = rig_set_mem(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.CTCSS") result = rig_set_ctcss_tone(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.CTCSS_SQL") result = rig_set_ctcss_sql(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.RPTR_OFFSET") result = rig_set_rptr_offs(rig_, state_.settingsVfo, value.i);
        if (s.id == "radio.RPTR_SHIFT") result = rig_set_rptr_shift(rig_, state_.settingsVfo, static_cast<rptr_shift_t>(value.i));
        break;
    }
    report("Set " + s.name + " = " + text, result);
    if (result == RIG_OK) { if (s.readable) read(s); else { s.value = text; s.valid = true; s.status = "Sent; no readback"; } }
    if (s.kind == Kind::Level && s.mask == RIG_LEVEL_RFPOWER && result == RIG_OK) {
        if (s.valid) log_("RF power readback: " + formatNumber(numeric(s.value) * 100) + " W configured (actual RF output is a separate meter).");
        else { state_.error = "RF power command sent, but readback failed: " + s.status; log_(state_.error); }
    }
}
int Worker::rfPowerMaximum() {
    split_t split = RIG_SPLIT_OFF;
    vfo_t tx = RIG_VFO_CURR;
    if (rig_get_split_vfo(rig_, RIG_VFO_CURR, &split, &tx) != RIG_OK || split == RIG_SPLIT_OFF) tx = RIG_VFO_CURR;
    rmode_t mode = RIG_MODE_NONE;
    pbwidth_t width{};
    if (rig_get_mode(rig_, tx, &mode, &width) == RIG_OK && (mode & (RIG_MODE_AM | RIG_MODE_AMN))) return 25;
    return 100;
}
void Worker::pollMeters(bool publishUpdate) {
    auto commandPending = [&] {
        std::lock_guard lock(mutex_);
        return stopping_ || !commands_.empty();
    };
    ptt_t ptt{};
    state_.pttKnown = rig_get_ptt(rig_, RIG_VFO_CURR, &ptt) == RIG_OK;
    state_.transmitting = state_.pttKnown && ptt != RIG_PTT_OFF;
    if (state_.pttKnown && !state_.transmitting) { heldPtt_ = false; catPttOwner_ = 0; state_.releasePending = false; }
    // Clear TX readings immediately on RX/unknown PTT, even if a queued
    // client request interrupts the remainder of this batch.
    for (auto& s : state_.settings) if (s.txOnly && !state_.transmitting) {
        s.valid = false; s.status = state_.pttKnown ? "TX only" : "TX status unavailable";
    }
    for (auto& s : state_.settings) if (s.kind == Kind::Level && state_.transmitting &&
        (s.mask == RIG_LEVEL_STRENGTH || s.mask == RIG_LEVEL_RAWSTR)) { s.valid = false; s.status = "RX only"; }
    if (urgentCommandPending()) { if (publishUpdate) publish(); return; }
    // Prioritize the transmitting meters, avoiding duplicate RM5 requests
    // when the calibrated watts meter is available.
    const std::array<setting_t, 9> levels{RIG_LEVEL_RFPOWER_METER_WATTS, RIG_LEVEL_ALC, RIG_LEVEL_SWR,
            RIG_LEVEL_COMP_METER, RIG_LEVEL_ID_METER, RIG_LEVEL_VD_METER,
            RIG_LEVEL_RFPOWER, RIG_LEVEL_STRENGTH, RIG_LEVEL_RAWSTR};
    for (std::size_t count = 0; count < levels.size(); ++count) {
        auto mask = levels[meterIndex_++ % levels.size()];
        auto it = std::find_if(state_.settings.begin(), state_.settings.end(), [&](const Setting& s) { return s.kind == Kind::Level && s.mask == mask; });
        if (mask == RIG_LEVEL_RFPOWER_METER_WATTS && it == state_.settings.end())
            it = std::find_if(state_.settings.begin(), state_.settings.end(), [](const Setting& s) { return s.kind == Kind::Level && s.mask == RIG_LEVEL_RFPOWER_METER; });
        if (it != state_.settings.end()) {
            if ((it->txOnly && !state_.transmitting) ||
                (state_.transmitting && (mask == RIG_LEVEL_STRENGTH || mask == RIG_LEVEL_RAWSTR))) continue;
            read(*it);
            // Derive the relative output from the same calibrated RM5 read.
            if (it->mask == RIG_LEVEL_RFPOWER_METER_WATTS) for (auto& relative : state_.settings)
                if (relative.kind == Kind::Level && relative.mask == RIG_LEVEL_RFPOWER_METER) {
                    relative.valid = it->valid; relative.status = it->status; relative.updated = it->updated;
                    if (it->valid) relative.value = formatNumber(std::clamp(numeric(it->value) / 100, 0.0, 1.0));
                }
        }
        if (commandPending()) break;
    }
    if (publishUpdate) publish();
}
bool Worker::urgentCommandPending() {
    std::lock_guard lock(mutex_);
    return stopping_ || (heldPtt_ && !state_.releasePending && Clock::now() >= pttDeadline_) ||
        (state_.releasePending && Clock::now() >= releaseDeadline_) ||
        std::any_of(commands_.begin(), commands_.end(), [](const Command& command) {
            if (!command.cat) return true;
            const auto& name = command.cat->request.name;
            return name.starts_with("set_") || name == "vfo_op" || name == "__release_client";
        });
}
void Worker::resetPoll() {
    pollStage_ = PollStage::FrequencyA;
    controlIndex_ = fastIndex_ = 0;
}
bool Worker::pollControls() {
    auto commandPending = [&] {
        std::lock_guard lock(mutex_);
        return stopping_ || !commands_.empty();
    };
    // Read the switch and its amount together, before the capability scan.
    // Keep the cursor when a CAT client interrupts so NR cannot stay at Off.
    static constexpr std::array<std::pair<Kind, setting_t>, 11> controls{{
        {Kind::Function, RIG_FUNC_NR}, {Kind::Level, RIG_LEVEL_NR},
        {Kind::Function, RIG_FUNC_NB}, {Kind::Level, RIG_LEVEL_NB},
        {Kind::Level, RIG_LEVEL_PREAMP}, {Kind::Level, RIG_LEVEL_AF},
        {Kind::Level, RIG_LEVEL_RF}, {Kind::Level, RIG_LEVEL_RFPOWER},
        {Kind::Level, RIG_LEVEL_MICGAIN}, {Kind::Level, RIG_LEVEL_SQL}, {Kind::Level, RIG_LEVEL_IF}
    }};
    while (controlIndex_ < controls.size()) {
        if (urgentCommandPending()) return false;
        const auto [kind, mask] = controls[controlIndex_++];
        auto it = std::find_if(state_.settings.begin(), state_.settings.end(), [&](const Setting& s) {
            return s.kind == kind && s.mask == mask;
        });
        if (it != state_.settings.end() && it->readable &&
            (!state_.transmitting || state_.settingsVfo == RIG_VFO_CURR || mask == RIG_LEVEL_RFPOWER)) read(*it);
        if (commandPending() && controlIndex_ < controls.size()) return false;
    }
    controlIndex_ = 0;
    return true;
}
void Worker::markPoweredOff() {
    resetPoll();
    const bool changed = !state_.poweredOff;
    state_.poweredOff = true; state_.powerKnown = true; state_.online = false; state_.standby = false;
    state_.connectionStatus = "Powered off"; state_.error.clear(); missed_ = 0;
    state_.transmitting = false; state_.pttKnown = true; heldPtt_ = false; catPttOwner_ = 0;
    state_.releasePending = false;
    state_.a.frequencyValid = state_.b.frequencyValid = false;
    state_.a.modeValid = state_.b.modeValid = false;
    for (auto& s : state_.settings) if (s.kind != Kind::Configuration) { s.valid = false; s.status = "Powered off"; }
    if (changed) log_("Radio powered off. CAT connection retained; Power on is available.");
}
bool Worker::poll() {
    auto pending = [&] {
        std::lock_guard lock(mutex_);
        return stopping_ || !commands_.empty() ||
            (heldPtt_ && !state_.releasePending && Clock::now() >= pttDeadline_) ||
            (state_.releasePending && Clock::now() >= releaseDeadline_);
    };
    if (urgentCommandPending()) return false;
    // Resume at the next transaction instead of repeating the first queries.
    // Each yield still gives queued PTT, tuning and other writes priority.
    for (;;) {
        // FT-710 frequency/mode queries target A/B directly and must stay live
        // during TX. Only skip receive controls that can make Hamlib switch VFO.
        if (state_.transmitting && pollStage_ == PollStage::Controls) {
            controlIndex_ = 0;
            pollStage_ = PollStage::FrequencyB;
        }
        if (state_.transmitting && pollStage_ >= PollStage::Functions) { resetPoll(); publish(); return true; }
        switch (pollStage_) {
        case PollStage::FrequencyA: {
            const bool checkedPower = state_.poweredOff || (!state_.online && !state_.powerKnown);
            if (checkedPower) {
                powerstat_t power{};
                const auto result = rig_get_powerstat(rig_, &power);
                if (result == RIG_OK && power == RIG_POWER_OFF) {
                    if (!state_.poweredOff) markPoweredOff();
                    state_.connectionStatus = "Powered off";
                    resetPoll(); publish(); return true;
                }
                if (state_.poweredOff && (result != RIG_OK || power != RIG_POWER_ON)) { resetPoll(); publish(); return true; }
                if (result == RIG_OK && power == RIG_POWER_ON) { state_.poweredOff = false; state_.powerKnown = true; }
            }
            freq_t frequency = 0;
            auto result = rig_get_freq(rig_, RIG_VFO_A, &frequency);
            if (result != RIG_OK) {
                const auto details = "Frequency read: " + safe(rigerror(result));
                // A frequency request does not work in standby. Check PS before
                // publishing an error, including when the front-panel power changed.
                powerstat_t power{};
                if (result == -RIG_EPOWER || (!checkedPower && rig_get_powerstat(rig_, &power) == RIG_OK && power == RIG_POWER_OFF)) {
                    markPoweredOff(); publish(); return true;
                }
                ++missed_;
                state_.online = false;
                state_.standby = connection_.type == ConnectionType::Serial && result == -RIG_ETIMEOUT;
                state_.connectionStatus = state_.standby ? "Powered off" : "No CAT response";
                if (state_.standby) {
                    state_.powerKnown = false;
                    if (state_.error.starts_with("Frequency read:")) state_.error.clear();
                } else state_.error = "Frequency read: " + briefError(result);
                state_.a.frequencyValid = state_.b.frequencyValid = false;
                state_.a.modeValid = state_.b.modeValid = false;
                state_.pttKnown = false;
                for (auto& s : state_.settings) if (s.kind != Kind::Configuration) { s.valid = false; s.status = state_.standby ? "Powered off" : "Stale / no response"; }
                if (missed_ == 1) {
                    log_(details);
                    log_(state_.standby ? "Powered off / silent CAT standby. Connection retained; polling continues and Power on is available." : "Power on remains available while the CAT port is open.");
                }
                resetPoll(); publish();
                return true;
            }
            missed_ = 0;
            if (state_.error.starts_with("Frequency read:")) state_.error.clear();
            state_.online = true;
            state_.standby = false; state_.poweredOff = false;
            state_.powerKnown = true;
            state_.connectionStatus = state_.demo ? "DEMO / Hamlib Dummy" : "Connected / live CAT";
            state_.lastContact = Clock::now();
            state_.a.frequency = frequency; state_.a.frequencyValid = true;
            pollStage_ = PollStage::ActiveVfo;
            break;
        }
        case PollStage::ActiveVfo: {
            vfo_t active{};
            if (rig_get_vfo(rig_, &active) == RIG_OK) state_.activeVfo = active;
            pollStage_ = PollStage::Meters;
            break;
        }
        case PollStage::Meters:
            pollMeters(false);
            pollStage_ = PollStage::Controls;
            break;
        case PollStage::Controls:
            if (!pollControls()) { publish(); return false; }
            pollStage_ = PollStage::FrequencyB;
            break;
        case PollStage::FrequencyB:
            state_.b.frequencyValid = rig_get_freq(rig_, RIG_VFO_B, &state_.b.frequency) == RIG_OK;
            pollStage_ = PollStage::ModeA;
            break;
        case PollStage::ModeA: {
            rmode_t mode{}; pbwidth_t width{};
            state_.a.modeValid = rig_get_mode(rig_, RIG_VFO_A, &mode, &width) == RIG_OK;
            if (state_.a.modeValid) { state_.a.mode = safe(rig_strrmode(mode)); state_.a.width = width; }
            pollStage_ = PollStage::ModeB;
            break;
        }
        case PollStage::ModeB: {
            rmode_t mode{}; pbwidth_t width{};
            state_.b.modeValid = rig_get_mode(rig_, RIG_VFO_B, &mode, &width) == RIG_OK;
            if (state_.b.modeValid) { state_.b.mode = safe(rig_strrmode(mode)); state_.b.width = width; }
            pollStage_ = PollStage::Functions;
            break;
        }
        case PollStage::Functions:
            while (fastIndex_ < state_.settings.size()) {
                if (urgentCommandPending()) { publish(); return false; }
                auto& s = state_.settings[fastIndex_++];
                if (!fast(s) || (s.kind == Kind::Function && (s.mask == RIG_FUNC_NR || s.mask == RIG_FUNC_NB)) ||
                    (s.kind == Kind::Level &&
                     (isMeter(s.mask) || s.mask == RIG_LEVEL_RFPOWER || s.mask == RIG_LEVEL_AF || s.mask == RIG_LEVEL_RF ||
                      s.mask == RIG_LEVEL_MICGAIN || s.mask == RIG_LEVEL_SQL || s.mask == RIG_LEVEL_IF ||
                      s.mask == RIG_LEVEL_NR || s.mask == RIG_LEVEL_NB || s.mask == RIG_LEVEL_PREAMP))) continue;
                read(s);
                if (pending()) { publish(); return false; }
            }
            fastIndex_ = 0;
            pollStage_ = PollStage::Slow;
            break;
        case PollStage::Slow: {
            // Keep slow scans bounded so one large capability table cannot block UI commands.
            auto deadline = Clock::now() + std::chrono::milliseconds(160);
            for (int count = 0; count < 4 && !state_.settings.empty(); ++count) {
                if (urgentCommandPending()) { publish(); return false; }
                auto& s = state_.settings[slowIndex_++ % state_.settings.size()];
                if (!fast(s) && s.kind != Kind::Configuration && !(s.kind == Kind::Level && isMeter(s.mask))) read(s);
                { std::lock_guard lock(mutex_); if (!commands_.empty() || stopping_) break; }
                if (Clock::now() > deadline) break;
            }
            resetPoll(); publish();
            return true;
        }
        }
        if (pending()) { publish(); return false; }
    }
}
int Worker::power(bool on) {
    if (on && (heldPtt_ || catPttOwner_ || state_.releasePending || state_.transmitting)) {
        report("Power on is unavailable during TX or a pending PTT release",-RIG_ERJCTED);publish();return -RIG_ERJCTED;
    }
    state_.busy = true;
    state_.connectionStatus = on ? "Waking radio..." : "Powering radio off...";
    publish();
    if (!on && (heldPtt_ || catPttOwner_ || state_.releasePending)) releaseTransmit("Power-off PTT release");
    int result = rig_set_powerstat(rig_, on ? RIG_POWER_ON : RIG_POWER_OFF);
    report(on ? "Power on" : "Power off", result);
    state_.busy = false;
    if (result == RIG_OK) {
        resetPoll();
        state_.poweredOff = !on;
        state_.standby = false;
        state_.powerKnown = true;
        state_.online = false;
        if (!on) markPoweredOff();
    }
    publish();
    return result;
}
void Worker::execute(const Command& command) {
    if (command.action == Action::Connect) { connect(command.connection); return; }
    if (command.action == Action::Disconnect) { disconnect(); log_("Disconnected"); return; }
    if (command.generation && command.generation != state_.generation) return;
    // No snapshots are published during these synchronous calls. Recording
    // the revision here acknowledges both success and the exception path.
    if (command.uiRequest) {
        auto index = command.vfo == RIG_VFO_B ? 1u : 0u;
        if (command.action == Action::Frequency || command.action == Action::StepFrequency) state_.frequencyRevision[index] = command.uiRequest;
        if (command.action == Action::Mode) state_.modeRevision[index] = command.uiRequest;
        if (command.action == Action::SelectVfo) state_.vfoRevision = command.uiRequest;
        if (command.action == Action::Set) state_.settingRevision[command.id] = command.uiRequest;
    }
    if (!rig_ || !state_.portOpen) throw std::runtime_error("Connect to the radio first.");
    if (command.action == Action::PttOff) {
        if (heldPtt_ || state_.releasePending) releaseTransmit("Release PTT");
        publish(); return;
    }
    if (command.action == Action::PowerOn || command.action == Action::PowerOff) {
        if (power(command.action == Action::PowerOn) == RIG_OK && !state_.poweredOff) poll();
        return;
    }
    if (command.action == Action::SettingsVfo) {
        state_.settingsVfo = command.vfo;
        resetPoll();
        for (auto& s : state_.settings) if (s.kind != Kind::Configuration) { s.valid = false; s.status = "Waiting for selected VFO"; }
        slowIndex_ = 0; publish(); return;
    }
    if (command.action == Action::Refresh) { slowIndex_ = 0; resetPoll(); poll(); return; }
    if (!state_.online || state_.busy) throw std::runtime_error("The radio is not responding. Use Power on or reconnect.");
    int result = -RIG_ENAVAIL;
    switch (command.action) {
    case Action::Set: {
        auto it = std::find_if(state_.settings.begin(), state_.settings.end(), [&](const Setting& s) { return s.id == command.id; });
        if (it == state_.settings.end()) throw std::invalid_argument("The selected setting is no longer available.");
        write(*it, command.text); publish(); return;
    }
    case Action::Frequency: case Action::StepFrequency: {
        double frequency = command.number;
        if (command.action == Action::StepFrequency) {
            freq_t current = 0;
            result = rig_get_freq(rig_, command.vfo, &current);
            if (result != RIG_OK) break;
            frequency = std::clamp(current + command.number, 30000.0, 60000000.0);
        }
        if (!std::isfinite(frequency) || frequency < 30000 || frequency > 60000000)
            throw std::invalid_argument("FT-710 receive frequency must be 0.030 to 60.000 MHz.");
        result = rig_set_freq(rig_, command.vfo, frequency);
        if (result == RIG_OK) {
            auto& vfo = command.vfo == RIG_VFO_B ? state_.b : state_.a;
            vfo.frequencyValid = rig_get_freq(rig_, command.vfo, &vfo.frequency) == RIG_OK;
        }
        break;
    }
    case Action::Mode: {
        rmode_t mode = rig_parse_mode(command.text.c_str());
        if (mode == RIG_MODE_NONE || command.width < 0 || command.width > 100000)
            throw std::invalid_argument("Select a supported mode and a passband of 0 (normal) to 100000 Hz.");
        result = rig_set_mode(rig_, command.vfo, mode, command.width);
        if (result == RIG_OK) {
            auto& vfo = command.vfo == RIG_VFO_B ? state_.b : state_.a;
            rmode_t actualMode{}; pbwidth_t actualWidth{};
            vfo.modeValid = rig_get_mode(rig_, command.vfo, &actualMode, &actualWidth) == RIG_OK;
            if (vfo.modeValid) { vfo.mode = safe(rig_strrmode(actualMode)); vfo.width = actualWidth; }
        }
        break;
    }
    case Action::SelectVfo:
        result = rig_set_vfo(rig_, command.vfo);
        if (result == RIG_OK) rig_get_vfo(rig_, &state_.activeVfo);
        break;
    case Action::Swap: result = rig_vfo_op(rig_, RIG_VFO_CURR, RIG_OP_XCHG); break;
    case Action::CopyAtoB: result = rig_vfo_op(rig_, RIG_VFO_CURR, RIG_OP_CPY); break;
    case Action::TuneAntenna: {
        if (!state_.canTune) throw std::runtime_error("This radio driver does not support an antenna tuning cycle.");
        if (heldPtt_ || catPttOwner_ || state_.releasePending)
            throw std::runtime_error("Release PTT before starting the antenna tuning cycle.");
        ptt_t ptt{};
        const int status = rig_get_ptt(rig_, RIG_VFO_CURR, &ptt);
        if (status != RIG_OK) throw std::runtime_error("Cannot read TX status; antenna tuning was not started.");
        if (ptt != RIG_PTT_OFF) throw std::runtime_error("The radio is transmitting; release PTT before antenna tuning.");
        // FT-710's Hamlib TUNE operation sends AC003;. The radio controls
        // its own tuning carrier and ends the cycle; dashboard PTT stays off.
        result = rig_vfo_op(rig_, RIG_VFO_CURR, RIG_OP_TUNE);
        resetPoll(); // Read actual TX/meters before any receive-control scan.
        break;
    }
    case Action::PttOn:
        {
            auto queueMs = command.queuedAt == Clock::time_point{} ? 0LL :
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - command.queuedAt).count();
            auto started = Clock::now();
        if (state_.releasePending) throw std::runtime_error("PTT release is pending; wait for RX confirmation.");
        if (catPttOwner_) throw std::runtime_error("PTT is already held by a CAT client.");
        {
            ptt_t current{};
            int pttRead = rig_get_ptt(rig_, RIG_VFO_CURR, &current);
            auto readMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
            if (pttRead != RIG_OK) {
                if (queueMs > 100 || readMs > 100) log_("Dashboard PTT timing: queue " + std::to_string(queueMs) + " ms, status read " + std::to_string(readMs) + " ms.");
                throw std::runtime_error("Cannot read PTT state; transmit was not started.");
            }
            if (current != RIG_PTT_OFF && !heldPtt_)
                throw std::runtime_error("The radio is already transmitting; its PTT is not owned by the dashboard.");
        }
        started = Clock::now();
        result = rig_set_ptt(rig_, RIG_VFO_CURR, RIG_PTT_ON);
        auto setMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        if (queueMs > 100 || setMs > 100) log_("Dashboard PTT timing: queue " + std::to_string(queueMs) + " ms, key command " + std::to_string(setMs) + " ms.");
        if (result == RIG_OK) { heldPtt_ = true; pttDeadline_ = Clock::now() + std::chrono::seconds(command.width > 0 ? std::clamp(command.width, 30L, 600L) : 30); }
        break;
        }
    case Action::RawQuery:
    case Action::RawWrite: {
        if (connection_.type != ConnectionType::Serial)
            throw std::invalid_argument("The raw FT-710 CAT console requires a direct serial connection.");
        const auto& text = command.text;
        if (text.size() < 3 || text.size() > 200 || text.back() != ';' || text.find(';') != text.size() - 1 ||
            std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c > 126; }))
            throw std::invalid_argument("Enter one ASCII CAT command terminated by a semicolon.");
        unsigned char reply[1024]{};
        unsigned char terminator[] = {';', 0};
        int received = rig_send_raw(rig_, reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()),
            command.action == Action::RawQuery ? reply : nullptr,
            command.action == Action::RawQuery ? sizeof reply - 1 : 0, terminator);
        if (received < 0) report("CAT " + text, received);
        else log_("CAT " + text + (command.action == Action::RawQuery ? " -> " + std::string(reinterpret_cast<char*>(reply), received) : " sent"));
        publish(); return;
    }
    default: return;
    }
    report(command.action == Action::Frequency ? "Tune " + std::string(rig_strvfo(command.vfo)) + " to " + formatNumber(command.number / 1e6) + " MHz" :
        command.action == Action::StepFrequency ? "Tune " + std::string(rig_strvfo(command.vfo)) + " by " + formatNumber(command.number) + " Hz" :
        command.action == Action::Mode ? "Set mode " + command.text : command.action == Action::TuneAntenna ? "Start antenna tuning cycle" :
        command.action == Action::PttOn ? "Dashboard PTT on" : "Radio command", result);
    if (command.action == Action::PttOn) {
        if (result == RIG_OK) {
            state_.pttKnown = true;
            state_.transmitting = true;
        }
        publish();
        return;
    }
    if (command.action == Action::Frequency || command.action == Action::StepFrequency || command.action == Action::Mode || command.action == Action::SelectVfo)
        publish(); // Show the direct readback before polling the other settings.
    poll();
}
}
