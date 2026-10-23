#include "radio.hpp"
#include <hamlib/rig_state.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace radio {
namespace {
std::string clean(const char* text) {
    std::string value = text ? text : "";
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}
double real(const std::string& text) {
    std::size_t used = 0;
    double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) throw std::invalid_argument("Invalid number");
    return value;
}
int integer(const std::string& text) {
    double value = real(text);
    if (std::trunc(value) != value || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::invalid_argument("Invalid integer");
    return static_cast<int>(value);
}
std::string precise(double value) {
    std::ostringstream text;
    text << std::setprecision(12) << value;
    return text.str();
}
template<class Parse> std::string names(setting_t mask, Parse parse) {
    std::string result;
    for (int i = 0; i < 64; ++i) if (mask & (setting_t(1) << i)) {
        auto name = clean(parse(setting_t(1) << i));
        if (!name.empty()) { if (!result.empty()) result += ' '; result += name; }
    }
    return result;
}
std::string dumpState(RIG* rig, bool modern) {
    // Protocol layout is specified by Hamlib's NET rigctl handshake. Values
    // come from this open rig, including the region-specific frequency ranges.
    auto& state = *HAMLIB_STATE(rig);
    auto& caps = *rig->caps;
    std::ostringstream out;
    out << std::setprecision(15) << "1\n" << caps.rig_model << "\n0\n";
    for (const auto* ranges : {state.rx_range_list, state.tx_range_list}) {
        for (int i = 0; i < HAMLIB_FRQRANGESIZ && !RIG_IS_FRNG_END(ranges[i]); ++i) {
            const auto& r = ranges[i];
            out << r.startf << ' ' << r.endf << " 0x" << std::hex << r.modes << std::dec << ' '
                << r.low_power << ' ' << r.high_power << " 0x" << std::hex << r.vfo << " 0x" << r.ant << std::dec << '\n';
        }
        out << "0 0 0 0 0 0 0\n";
    }
    for (const auto& step : state.tuning_steps) {
        if (RIG_IS_TS_END(step)) break;
        out << "0x" << std::hex << step.modes << std::dec << ' ' << step.ts << '\n';
    }
    out << "0 0\n";
    for (const auto& filter : state.filters) {
        if (RIG_IS_FLT_END(filter)) break;
        out << "0x" << std::hex << filter.modes << std::dec << ' ' << filter.width << '\n';
    }
    out << "0 0\n" << state.max_rit << '\n' << state.max_xit << '\n' << state.max_ifshift << '\n' << state.announces << '\n';
    for (const auto* list : {state.preamp, state.attenuator}) {
        for (int i = 0; i < HAMLIB_MAXDBLSTSIZ && list[i]; ++i) out << list[i] << ' ';
        out << '\n';
    }
    for (auto mask : {state.has_get_func, state.has_set_func, state.has_get_level, state.has_set_level, state.has_get_parm, state.has_set_parm})
        out << "0x" << std::hex << mask << std::dec << '\n';
    if (modern) {
        out << "vfo_ops=0x" << std::hex << caps.vfo_ops << "\nptt_type=0x" << caps.ptt_type
            << "\ntargetable_vfo=0x" << caps.targetable_vfo << std::dec << '\n';
        out << "has_set_vfo=" << (caps.set_vfo != nullptr) << "\nhas_get_vfo=" << (caps.get_vfo != nullptr)
            << "\nhas_set_freq=" << (caps.set_freq != nullptr) << "\nhas_get_freq=" << (caps.get_freq != nullptr)
            << "\nhas_set_conf=0\nhas_get_conf=" << (caps.get_conf != nullptr)
            << "\nhas_power2mW=" << (caps.power2mW != nullptr) << "\nhas_mW2power=" << (caps.mW2power != nullptr)
            << "\nhas_get_ant=" << (caps.get_ant != nullptr) << "\nhas_set_ant=" << (caps.set_ant != nullptr)
            // NET rigctl adds 500 ms to this value. The shared worker may be
            // finishing another transaction, and mode changes need several
            // serial exchanges. This is a timeout, not a polling delay.
            << "\ntimeout=" << std::max(caps.timeout, 5000) << "\nrig_model=" << caps.rig_model
            << "\nrigctld_version=" << rig_version() << '\n';
        if (caps.agc_level_count > 0) {
            out << "agc_levels=";
            for (int i = 0; i < caps.agc_level_count && i < HAMLIB_MAX_AGC_LEVELS; ++i)
                out << (i ? " " : "") << static_cast<int>(caps.agc_levels[i]) << '=' << clean(rig_stragclevel(caps.agc_levels[i]));
            out << '\n';
        }
        if (caps.ctcss_list) {
            out << "ctcss_list=";
            for (int i = 0; i < CTCSS_LIST_SIZE && caps.ctcss_list[i]; ++i) out << ' ' << caps.ctcss_list[i] / 10.0;
            out << '\n';
        }
        if (caps.dcs_list) {
            out << "dcs_list=";
            for (int i = 0; i < DCS_LIST_SIZE && caps.dcs_list[i]; ++i) out << ' ' << caps.dcs_list[i];
            out << '\n';
        }
        out << std::setprecision(6);
        for (bool parameter : {false, true}) {
            out << (parameter ? "parm_gran=" : "level_gran=");
            for (int i = 0; i < RIG_SETTING_MAX; ++i) {
                auto mask = setting_t(1) << i;
                const auto& gran = parameter ? state.parm_gran[i] : state.level_gran[i];
                out << i << '=';
                if (parameter && RIG_PARM_IS_STRING(mask)) out << clean(gran.step.cs);
                else if (parameter ? RIG_PARM_IS_FLOAT(mask) : RIG_LEVEL_IS_FLOAT(mask))
                    out << gran.min.f << ',' << gran.max.f << ',' << gran.step.f;
                else out << gran.min.i << ',' << gran.max.i << ',' << gran.step.i;
                out << ';';
            }
            out << '\n';
        }
        out << "hamlib_version=" << rig_version() << "\ndone\n";
    }
    return out.str();
}
}

CatReply Worker::executeCat(const CatRequest& request) {
    CatReply reply;
    const auto& name = request.name;
    const auto& args = request.arguments;
    auto add = [&](std::string label, std::string value) { reply.values.emplace_back(std::move(label), std::move(value)); };
    auto fail = [&](int result) { reply.result = result; return reply; };
    auto need = [&](std::size_t count) { if (args.size() != count) throw std::invalid_argument("Wrong argument count"); };
    auto vfoValue = [](const std::string& text) {
        vfo_t vfo = rig_parse_vfo(text.c_str());
        if (vfo == RIG_VFO_NONE) throw std::invalid_argument("Invalid VFO");
        return vfo;
    };
    auto modeValue = [](const std::string& text) {
        rmode_t mode = rig_parse_mode(text.c_str());
        if (mode == RIG_MODE_NONE) throw std::invalid_argument("Invalid mode");
        return mode;
    };
    auto frequencyValue = [&](const std::string& text) {
        double value = real(text);
        if (!state_.demo && (value < 30000 || value > 60000000)) throw std::invalid_argument("Invalid receive frequency");
        if (value <= 0) throw std::invalid_argument("Invalid frequency");
        return value;
    };
    auto widthValue = [&](const std::string& text) {
        int width = integer(text);
        // WSJT-X uses -1 to change mode without changing the filter. Pass the
        // sentinel through to Hamlib for normal and split-mode operations.
        if (width < RIG_PASSBAND_NOCHANGE || width > 100000) throw std::invalid_argument("Invalid passband");
        return width;
    };
    if (request.generation != state_.generation || !rig_ || !state_.portOpen || connection_.type == ConnectionType::Network)
        return fail(-RIG_EIO);
    if (name.starts_with("set_") || name == "vfo_op") invalidateReadbacks();
    if (name == "__release_client") {
        if (catPttOwner_ == request.client) {
            reply.result = releaseTransmit("Release PTT for disconnected CAT client " + std::to_string(request.client));
            reply.changed = true;
        }
        return reply;
    }
    auto& rs = *HAMLIB_STATE(rig_);
    const auto& caps = *rig_->caps;
    vfo_t vfo = vfoValue(request.vfo);
    if (name == "dump_state") { need(0); reply.raw = dumpState(rig_, request.modern); return reply; }
    if (name == "get_vfo_list") { need(0); add("VFOs", names(rs.vfo_list, rig_strvfo)); return reply; }
    if (name == "get_modes") { need(0); add("Modes", names(rs.mode_list, rig_strrmode)); return reply; }
    if (name == "get_mode_bandwidths") {
        need(1); auto mode = modeValue(args[0]); std::string widths;
        for (const auto& filter : rs.filters) {
            if (RIG_IS_FLT_END(filter)) break;
            if (filter.modes & mode) { if (!widths.empty()) widths += ' '; widths += std::to_string(filter.width); }
        }
        add("Mode", widths); return reply;
    }
    if (name == "client_version") { need(1); add("Version", args[0]); return reply; }
    if (name == "hamlib_version") { need(0); add("Version", clean(rig_version())); return reply; }
    if (name == "dump_caps") {
        need(0);
        reply.raw = "Model name: " + clean(caps.model_name) + "\nMfg name: " + clean(caps.mfg_name) +
            "\nBackend version: " + clean(caps.version) + "\nServer: FT-710 Dashboard\n" +
            "Get levels: " + names(rs.has_get_level, rig_strlevel) + "\nSet levels: " + names(rs.has_set_level, rig_strlevel) +
            "\nGet functions: " + names(rs.has_get_func, rig_strfunc) + "\nSet functions: " + names(rs.has_set_func, rig_strfunc) + "\n";
        return reply;
    }
    if (name == "get_conf") {
        need(1); hamlib_token_t token = rig_token_lookup(rig_, args[0].c_str());
        if (token == RIG_CONF_END) return fail(-RIG_EINVAL);
        char value[1024]{}; reply.result = rig_get_conf2(rig_, token, value, sizeof value); add("Value", clean(value)); return reply;
    }
    if (name == "set_powerstat") {
        need(1); int on = integer(args[0]); if (on != 0 && on != 1) return fail(-RIG_EINVAL);
        reply.result = power(on == 1); reply.changed = reply.result == RIG_OK; return reply;
    }
    if (name == "get_powerstat") {
        need(0); powerstat_t status = RIG_POWER_OFF;
        if (!state_.poweredOff) reply.result = rig_get_powerstat(rig_, &status);
        add("Power Status", std::to_string(status)); return reply;
    }
    // Capability queries work in standby without waking the transceiver.
    if (args.size() == 1 && args[0] == "?") {
        if (name == "set_mode") add("Mode", names(rs.mode_list, rig_strrmode));
        else if (name == "set_vfo" || name == "get_vfo_info") add("VFO", names(rs.vfo_list, rig_strvfo));
        else if (name == "get_level" || name == "set_level") add("Level", names(name == "get_level" ? rs.has_get_level : rs.has_set_level, rig_strlevel));
        else if (name == "get_func" || name == "set_func") add("Func", names(name == "get_func" ? rs.has_get_func : rs.has_set_func, rig_strfunc));
        else if (name == "get_parm" || name == "set_parm") add("Parm", names(name == "get_parm" ? rs.has_get_parm : rs.has_set_parm, rig_strparm));
        else if (name == "vfo_op") add("Mem/VFO Op", names(caps.vfo_ops, [](setting_t bit) { return rig_strvfop(static_cast<vfo_op_t>(bit)); }));
        else return fail(-RIG_ENAVAIL);
        return reply;
    }
    if (state_.poweredOff) return fail(-RIG_EPOWER);
    bool setting = name.starts_with("set_") || name == "vfo_op";
    if (name == "get_freq") {
        need(0); freq_t frequency{}; reply.result = readFrequency(vfo, &frequency); add("Frequency", std::to_string(static_cast<long long>(std::llround(frequency))));
    } else if (name == "set_freq") {
        need(1); reply.result = rig_set_freq(rig_, vfo, frequencyValue(args[0]));
    } else if (name == "get_mode") {
        need(0); rmode_t mode{}; pbwidth_t width{}; reply.result = readMode(vfo, &mode, &width);
        add("Mode", clean(rig_strrmode(mode))); add("Passband", std::to_string(width));
    } else if (name == "set_mode") {
        need(2); reply.result = rig_set_mode(rig_, vfo, modeValue(args[0]), widthValue(args[1]));
    } else if (name == "get_vfo") {
        need(0); vfo_t current{}; reply.result = readActiveVfo(&current); add("VFO", clean(rig_strvfo(current)));
    } else if (name == "set_vfo") {
        need(1); reply.result = rig_set_vfo(rig_, vfoValue(args[0]));
    } else if (name == "get_ptt") {
        need(0); ptt_t status{}; reply.result = rig_get_ptt(rig_, vfo, &status); add("PTT", std::to_string(status));
    } else if (name == "set_ptt") {
        need(1); int status = integer(args[0]); if (status < 0 || status > 3) return fail(-RIG_EINVAL);
        if (status != 0 && (state_.releasePending || heldPtt_ || (catPttOwner_ && catPttOwner_ != request.client))) return fail(-RIG_ERJCTED);
        reply.result = status == 0 ? releaseTransmit("CAT PTT release") : rig_set_ptt(rig_, vfo, static_cast<ptt_t>(status));
        if (reply.result == RIG_OK) {
            catPttOwner_ = status ? request.client : 0;
            if (!status) heldPtt_ = false;
            state_.transmitting = status != 0; state_.pttKnown = true;
        }
    } else if (name == "get_split_vfo") {
        need(0); split_t split{}; vfo_t tx{}; reply.result = readSplit(vfo, &split, &tx);
        add("Split", std::to_string(split)); add("TX VFO", clean(rig_strvfo(tx)));
    } else if (name == "set_split_vfo") {
        need(2); int split = integer(args[0]); if (split != 0 && split != 1) return fail(-RIG_EINVAL);
        reply.result = rig_set_split_vfo(rig_, vfo, static_cast<split_t>(split), vfoValue(args[1]));
    } else if (name == "get_split_freq" || name == "get_split_mode" || name == "get_split_freq_mode") {
        need(0); freq_t frequency{}; rmode_t mode{}; pbwidth_t width{};
        if (name != "get_split_mode") {
            reply.result = rig_get_split_freq(rig_, vfo, &frequency); add("TX Frequency", std::to_string(static_cast<long long>(std::llround(frequency))));
        }
        if (reply.result == RIG_OK && name != "get_split_freq") {
            reply.result = rig_get_split_mode(rig_, vfo, &mode, &width); add("TX Mode", clean(rig_strrmode(mode))); add("TX Passband", std::to_string(width));
        }
    } else if (name == "set_split_freq") {
        need(1); reply.result = rig_set_split_freq(rig_, vfo, frequencyValue(args[0]));
    } else if (name == "set_split_mode") {
        need(2); reply.result = rig_set_split_mode(rig_, vfo, modeValue(args[0]), widthValue(args[1]));
    } else if (name == "set_split_freq_mode") {
        need(3); reply.result = rig_set_split_freq_mode(rig_, vfo, frequencyValue(args[0]), modeValue(args[1]), widthValue(args[2]));
    } else if (name == "get_vfo_info") {
        need(1); freq_t frequency{}; rmode_t mode{}; pbwidth_t width{}; split_t split{}; int sat = 0;
        reply.result = readVfoInfo(vfoValue(args[0]), &frequency, &mode, &width, &split, &sat);
        add("Freq", std::to_string(static_cast<long long>(std::llround(frequency)))); add("Mode", clean(rig_strrmode(mode)));
        add("Width", std::to_string(width)); add("Split", std::to_string(split)); add("SatMode", std::to_string(sat));
    } else if (name == "get_rig_info") {
        need(0); char info[4096]{}; reply.result = rig_get_rig_info(rig_, info, sizeof info);
        // Preserve line breaks: Hamlib includes a CRC for the original text.
        if (reply.result == RIG_OK) reply.raw = std::string(info) + "\n";
    } else if (name == "get_info") {
        need(0); const char* info = rig_get_info(rig_); if (!info) reply.result = -RIG_ENAVAIL; add("Info", clean(info));
    } else if (name == "get_level" || name == "set_level" || name == "get_func" || name == "set_func" || name == "get_parm" || name == "set_parm") {
        need(setting ? 2 : 1);
        bool function = name.ends_with("func"), parameter = name.ends_with("parm");
        setting_t mask = function ? rig_parse_func(args[0].c_str()) : parameter ? rig_parse_parm(args[0].c_str()) : rig_parse_level(args[0].c_str());
        const confparams* extension = mask ? nullptr : rig_ext_lookup(rig_, args[0].c_str());
        if (!mask && !extension) return fail(-RIG_EINVAL);
        if (!function && !parameter && (isMeter(mask) || mask == RIG_LEVEL_RFPOWER)) vfo = RIG_VFO_CURR;
        if (setting && mask == RIG_LEVEL_RFPOWER) {
            double power = real(args[1]);
            // Apply the FT-710 driver's real limits rather than the copied
            // 200 W transmit-range metadata in Hamlib 4.7.2.
            if (!state_.demo && (power < .05 || power > rfPowerMaximum() / 100.0)) return fail(-RIG_EINVAL);
        }
        bool text = extension ? extension->type == RIG_CONF_STRING : parameter && RIG_PARM_IS_STRING(mask);
        bool floating = extension ? extension->type == RIG_CONF_NUMERIC : !function && (parameter ? RIG_PARM_IS_FLOAT(mask) : RIG_LEVEL_IS_FLOAT(mask));
        value_t value{}; int status = 0;
        if (setting) {
            if (text) value.cs = args[1].c_str();
            else if (floating) {
                double number = real(args[1]);
                if (std::abs(number) > std::numeric_limits<float>::max()) return fail(-RIG_EINVAL);
                value.f = static_cast<float>(number);
            } else value.i = integer(args[1]);
            if (function && value.i != 0 && value.i != 1) return fail(-RIG_EINVAL);
            if (extension) reply.result = function ? rig_set_ext_func(rig_, vfo, extension->token, value.i) :
                parameter ? rig_set_ext_parm(rig_, extension->token, value) : rig_set_ext_level(rig_, vfo, extension->token, value);
            else reply.result = function ? rig_set_func(rig_, vfo, mask, value.i) :
                parameter ? rig_set_parm(rig_, mask, value) : rig_set_level(rig_, vfo, mask, value);
        } else {
            if (extension) reply.result = function ? rig_get_ext_func(rig_, vfo, extension->token, &status) :
                parameter ? rig_get_ext_parm(rig_, extension->token, &value) : rig_get_ext_level(rig_, vfo, extension->token, &value);
            else reply.result = function ? rig_get_func(rig_, vfo, mask, &status) :
                parameter ? rig_get_parm(rig_, mask, &value) : rig_get_level(rig_, vfo, mask, &value);
            add(function ? "Func Status" : parameter ? "Parm Value" : "Level Value",
                function ? std::to_string(status) : text ? clean(value.s) : floating ? precise(value.f) : std::to_string(value.i));
        }
    } else if (name == "get_rit" || name == "get_xit" || name == "get_rptr_offs") {
        need(0); shortfreq_t value{};
        reply.result = name == "get_rit" ? rig_get_rit(rig_, vfo, &value) : name == "get_xit" ? rig_get_xit(rig_, vfo, &value) : rig_get_rptr_offs(rig_, vfo, &value);
        add(name == "get_rit" ? "RIT" : name == "get_xit" ? "XIT" : "Repeater Offset", std::to_string(value));
    } else if (name == "set_rit" || name == "set_xit" || name == "set_rptr_offs") {
        need(1); int value = integer(args[0]);
        reply.result = name == "set_rit" ? rig_set_rit(rig_, vfo, value) : name == "set_xit" ? rig_set_xit(rig_, vfo, value) : rig_set_rptr_offs(rig_, vfo, value);
    } else if (name == "get_ts" || name == "get_mem") {
        need(0); shortfreq_t step{}; int memory{};
        reply.result = name == "get_ts" ? rig_get_ts(rig_, vfo, &step) : rig_get_mem(rig_, vfo, &memory);
        add(name == "get_ts" ? "Tuning Step" : "Memory#", std::to_string(name == "get_ts" ? step : memory));
    } else if (name == "set_ts" || name == "set_mem" || name == "set_bank") {
        need(1); int value = integer(args[0]); reply.result = name == "set_ts" ? rig_set_ts(rig_, vfo, value) : name == "set_mem" ? rig_set_mem(rig_, vfo, value) : rig_set_bank(rig_, vfo, value);
    } else if (name == "vfo_op") {
        need(1); auto op = rig_parse_vfo_op(args[0].c_str()); if (!op) return fail(-RIG_EINVAL); reply.result = rig_vfo_op(rig_, vfo, op);
    } else if (name == "get_rptr_shift") {
        need(0); rptr_shift_t shift{}; reply.result = rig_get_rptr_shift(rig_, vfo, &shift); add("Repeater Shift", clean(rig_strptrshift(shift)));
    } else if (name == "set_rptr_shift") {
        need(1); if (args[0] != "+" && args[0] != "-" && args[0] != "None") return fail(-RIG_EINVAL);
        reply.result = rig_set_rptr_shift(rig_, vfo, rig_parse_rptr_shift(args[0].c_str()));
    } else if (name == "get_ctcss_tone" || name == "get_ctcss_sql" || name == "get_dcs_code" || name == "get_dcs_sql") {
        need(0); tone_t tone{}; reply.result = name == "get_ctcss_tone" ? rig_get_ctcss_tone(rig_, vfo, &tone) : name == "get_ctcss_sql" ? rig_get_ctcss_sql(rig_, vfo, &tone) :
            name == "get_dcs_code" ? rig_get_dcs_code(rig_, vfo, &tone) : rig_get_dcs_sql(rig_, vfo, &tone);
        add(name == "get_ctcss_tone" ? "CTCSS Tone" : name == "get_ctcss_sql" ? "CTCSS Squelch" : name == "get_dcs_code" ? "DCS Code" : "DCS Squelch", std::to_string(tone));
    } else if (name == "set_ctcss_tone" || name == "set_ctcss_sql" || name == "set_dcs_code" || name == "set_dcs_sql") {
        need(1); int tone = integer(args[0]); if (tone < 0) return fail(-RIG_EINVAL);
        reply.result = name == "set_ctcss_tone" ? rig_set_ctcss_tone(rig_, vfo, tone) : name == "set_ctcss_sql" ? rig_set_ctcss_sql(rig_, vfo, tone) :
            name == "set_dcs_code" ? rig_set_dcs_code(rig_, vfo, tone) : rig_set_dcs_sql(rig_, vfo, tone);
    } else if (name == "get_ant") {
        need(1); int antenna = integer(args[0]); if (antenna < 0 || antenna > 16) return fail(-RIG_EINVAL);
        value_t option{}; ant_t current{}, tx{}, rx{};
        reply.result = rig_get_ant(rig_, vfo, antenna ? RIG_ANT_N(antenna - 1) : RIG_ANT_CURR, &option, &current, &tx, &rx);
        auto ant = [](ant_t value) { return value ? std::to_string(rig_setting2idx(value) + 1) : "0"; };
        add("AntCurr", ant(current)); add("Option", std::to_string(option.i)); add("AntTx", ant(tx)); add("AntRx", ant(rx));
    } else if (name == "set_ant") {
        need(2); int antenna = integer(args[0]); if (antenna < 1 || antenna > 16) return fail(-RIG_EINVAL);
        value_t option{}; option.i = integer(args[1]); reply.result = rig_set_ant(rig_, vfo, RIG_ANT_N(antenna - 1), option);
    } else if (name == "get_dcd") {
        need(0); dcd_t dcd{}; reply.result = rig_get_dcd(rig_, vfo, &dcd); add("DCD", std::to_string(dcd));
    } else if (name == "get_trn") {
        need(0); add("Transceive", "0");
    } else if (name == "set_trn") {
        // Unsolicited CAT events are incompatible with the request/response
        // serial queue. Polling (0) is the supported client mode.
        need(1); reply.result = integer(args[0]) == 0 ? RIG_OK : -RIG_ENAVAIL;
    } else if (name == "power2mW") {
        need(3); unsigned int power{}; reply.result = rig_power2mW(rig_, &power, static_cast<float>(real(args[0])), real(args[1]), modeValue(args[2])); add("Power mW", std::to_string(power));
    } else if (name == "mW2power") {
        need(3); int power = integer(args[0]); if (power < 0) return fail(-RIG_EINVAL);
        float fraction{}; reply.result = rig_mW2power(rig_, &fraction, power, real(args[1]), modeValue(args[2])); add("Power [0.0..1.0]", precise(fraction));
    } else return fail(-RIG_ENAVAIL);
    reply.changed = setting && reply.result == RIG_OK;
    if (setting) {
        std::string operation = "CAT client " + std::to_string(request.client) + ": " + name;
        for (const auto& arg : args) operation += " " + arg;
        report(operation, reply.result);
    }
    return reply;
}
}
