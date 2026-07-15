// cmd_audio_plugin_inspect.cpp — `pulp audio plugin-inspect`
//
// Loads vendor code only in a disposable child process and publishes a single
// JSON artifact after successful load/prepare/warm-up/parameter enumeration.

#include "cmd_audio_plugin_inspect.hpp"

#include "au_info_plist.hpp"
#include "cli_common.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/events/message_loop_integration.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using pulp::host::PluginFormat;

struct Request {
    std::string plugin;
    std::string format = "clap";
    std::string unique_id;
    double sample_rate = 48000.0;
    std::uint32_t block = 512;
    std::optional<std::uint32_t> warmup_ms;
    std::uint32_t timeout_ms = 0;
    bool json = false;
    bool worker = false;
    fs::path result_file;
};

void usage() {
    std::cout
        << "pulp audio plugin-inspect — isolated hosted-plugin API discovery\n\n"
        << "Usage: pulp audio plugin-inspect --plugin <bundle> [options]\n\n"
        << "  --format clap|vst3|au|auv3|lv2   (default: clap)\n"
        << "  --id <unique-id>                  Descriptor/component identity\n"
        << "  --sample-rate <hz>                Prepare rate (default: 48000)\n"
        << "  --block <frames>                  Prepare block size (default: 512)\n"
        << "  --warmup-ms <n>                   Initialization/event pre-roll\n"
        << "                                      (default: 1000 for AU, 0 otherwise)\n"
        << "  --timeout-ms <n>                  Worker timeout (default: 30000)\n"
        << "  --json                            Emit machine-readable JSON\n";
}

template <typename T>
std::optional<T> parse_unsigned(std::string_view value) {
    unsigned long long parsed = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size() ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<T>::max()))
        return std::nullopt;
    return static_cast<T>(parsed);
}

bool parse_request(const std::vector<std::string>& args, Request& out, std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--json") { out.json = true; continue; }
        if (key == "--worker") { out.worker = true; continue; }
        if (key == "--help" || key == "-h") { error = "help"; return false; }
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size() ||
                (!args[i + 1].empty() && args[i + 1].front() == '-'))
                return std::nullopt;
            return args[++i];
        };
        if (key == "--plugin") {
            auto v = value(); if (!v) { error = "--plugin requires a value"; return false; }
            out.plugin = *v;
        } else if (key == "--format") {
            auto v = value(); if (!v) { error = "--format requires a value"; return false; }
            out.format = *v;
        } else if (key == "--id") {
            auto v = value(); if (!v) { error = "--id requires a value"; return false; }
            out.unique_id = *v;
        } else if (key == "--sample-rate") {
            auto v = value(); if (!v) { error = "--sample-rate requires a value"; return false; }
            char* end = nullptr;
            out.sample_rate = std::strtod(v->c_str(), &end);
            if (end != v->c_str() + v->size() || !std::isfinite(out.sample_rate) ||
                out.sample_rate <= 0.0) {
                error = "--sample-rate must be positive"; return false;
            }
        } else if (key == "--block") {
            auto v = value();
            auto n = v ? parse_unsigned<std::uint32_t>(*v) : std::nullopt;
            if (!n || *n == 0) { error = "--block must be positive"; return false; }
            out.block = *n;
        } else if (key == "--warmup-ms") {
            auto v = value();
            auto n = v ? parse_unsigned<std::uint32_t>(*v) : std::nullopt;
            if (!n) { error = "--warmup-ms must be non-negative"; return false; }
            out.warmup_ms = *n;
        } else if (key == "--timeout-ms") {
            auto v = value();
            auto n = v ? parse_unsigned<std::uint32_t>(*v) : std::nullopt;
            if (!n || *n == 0) { error = "--timeout-ms must be positive"; return false; }
            out.timeout_ms = *n;
        } else if (key == "--result-file") {
            auto v = value(); if (!v) { error = "--result-file requires a value"; return false; }
            out.result_file = *v;
        } else {
            error = "unknown flag: " + key;
            return false;
        }
    }
    if (out.plugin.empty()) { error = "--plugin <bundle> is required"; return false; }
    if (out.worker && out.result_file.empty()) {
        error = "worker result path is required"; return false;
    }
    return true;
}

PluginFormat format_from_string(std::string_view value, bool& valid) {
    valid = true;
    if (value == "clap") return PluginFormat::CLAP;
    if (value == "vst3") return PluginFormat::VST3;
    if (value == "au") return PluginFormat::AudioUnit;
    if (value == "auv3") return PluginFormat::AudioUnitV3;
    if (value == "lv2") return PluginFormat::LV2;
    valid = false;
    return PluginFormat::CLAP;
}

bool is_au(PluginFormat format) {
    return format == PluginFormat::AudioUnit || format == PluginFormat::AudioUnitV3;
}

std::string json_escape(std::string_view input) {
    std::ostringstream out;
    for (unsigned char c : input) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                                  << std::setfill('0') << static_cast<int>(c) << std::dec;
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

const char* latency_status(pulp::host::PluginSlot::LatencyQuery query) {
    switch (query) {
        case pulp::host::PluginSlot::LatencyQuery::Available: return "available";
        case pulp::host::PluginSlot::LatencyQuery::Unsupported: return "unsupported";
        case pulp::host::PluginSlot::LatencyQuery::QueryFailed: return "query_failed";
    }
    return "unsupported";
}

void append_json_number(std::ostringstream& out, float value) {
    if (std::isfinite(value)) out << value;
    else out << "null";
}

void warm_up(pulp::host::PluginSlot& slot, std::uint32_t duration_ms,
             std::uint32_t inputs, std::uint32_t outputs, std::uint32_t block) {
    if (duration_ms == 0) return;
    pulp::audio::Buffer<float> input(inputs, block), output(outputs, block);
    input.clear(); output.clear();
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::state::ParameterEventQueue params;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto out = output.view();
        const auto in = static_cast<const pulp::audio::Buffer<float>&>(input).view();
        slot.process(out, in, midi_in, midi_out, params, static_cast<int>(block));
        const auto result = pulp::events::MessageLoopIntegration::pump_main_loop_for(
            std::chrono::milliseconds(10));
        if (result == pulp::events::MainLoopPumpResult::Unsupported ||
            result == pulp::events::MainLoopPumpResult::Finished ||
            result == pulp::events::MainLoopPumpResult::Stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::string build_result_json(const pulp::host::PluginSlot& slot) {
    const auto& info = slot.info();
    const auto params = slot.parameters();
    std::ostringstream out;
    const auto latency = slot.latency_query();
    out << std::setprecision(9)
        << "{\"schema\":\"pulp.audio.plugin-inspect.v1\",\"plugin\":{"
        << "\"name\":\"" << json_escape(info.name) << "\","
        << "\"manufacturer\":\"" << json_escape(info.manufacturer) << "\","
        << "\"version\":\"" << json_escape(info.version) << "\","
        << "\"unique_id\":\"" << json_escape(info.unique_id) << "\","
        << "\"path\":\"" << json_escape(info.path) << "\","
        << "\"instrument\":" << (info.is_instrument ? "true" : "false") << ','
        << "\"effect\":" << (info.is_effect ? "true" : "false") << ','
        << "\"inputs\":" << info.num_inputs << ",\"outputs\":" << info.num_outputs
        << "},\"latency\":{\"status\":\"" << latency_status(latency)
        << "\",\"samples\":";
    if (latency == pulp::host::PluginSlot::LatencyQuery::Available)
        out << slot.latency_samples();
    else
        out << "null";
    out << "},\"tail_samples\":"
        << slot.tail_samples() << ",\"parameters\":[";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) out << ',';
        const auto& p = params[i];
        const float current = slot.get_parameter(p.id);
        out << "{\"id\":" << p.id << ",\"name\":\"" << json_escape(p.name)
            << "\",\"unit\":\"" << json_escape(p.unit) << "\",\"min\":";
        append_json_number(out, p.min_value);
        out << ",\"max\":";
        append_json_number(out, p.max_value);
        out << ",\"default\":";
        append_json_number(out, p.default_value);
        out << ",\"current\":";
        append_json_number(out, current);
        out << ",\"automatable\":" << (p.flags.automatable ? "true" : "false")
            << ",\"read_only\":" << (p.flags.read_only ? "true" : "false")
            << ",\"hidden\":" << (p.flags.hidden ? "true" : "false")
            << ",\"stepped\":" << (p.flags.stepped ? "true" : "false")
            << ",\"bypass\":" << (p.flags.is_bypass ? "true" : "false")
            << ",\"rampable\":" << (p.flags.rampable ? "true" : "false")
            << ",\"modulatable\":" << (p.flags.modulatable ? "true" : "false")
            << ",\"rate\":\""
            << (p.rate == pulp::state::ParamRate::AudioRate ? "audio" : "control")
            << "\"}";
    }
    out << "]}";
    return out.str();
}

bool atomic_write(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << contents << '\n';
        if (!file.good()) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp); return false; }
    return true;
}

int worker_main(const Request& request, PluginFormat format) {
    pulp::host::PluginInfo info;
    info.path = request.plugin;
    info.format = format;
    info.unique_id = request.unique_id;
    if (info.unique_id.empty() && is_au(format))
        info.unique_id = pulp::cli::au_info_plist::unique_id_from_bundle(request.plugin);

    // AU component metadata (notably instrument/effect and bus shape) comes
    // from AudioComponent enumeration rather than the bundle Info.plist. Keep
    // load and enumeration in this same disposable worker.
    if (is_au(format) && !info.unique_id.empty()) {
        pulp::host::ScanOptions options;
        options.scan_vst3 = false;
        options.scan_clap = false;
        options.scan_lv2 = false;
        const auto installed = pulp::host::PluginScanner{}.scan(options);
        const auto found = std::find_if(installed.begin(), installed.end(),
            [&](const pulp::host::PluginInfo& candidate) {
                return candidate.format == format &&
                       candidate.unique_id == info.unique_id;
            });
        if (found != installed.end()) {
            const auto requested_path = info.path;
            info = *found;
            if (info.path.empty()) info.path = requested_path;
        }
    }

    auto slot = pulp::host::PluginSlot::load(info);
    if (!slot) { std::fprintf(stderr, "plugin-inspect: load failed\n"); return 10; }
    if (!slot->prepare(request.sample_rate, static_cast<int>(request.block))) {
        std::fprintf(stderr, "plugin-inspect: prepare failed\n"); return 11;
    }
    const auto warmup = request.warmup_ms.value_or(is_au(format) ? 1000u : 0u);
    warm_up(*slot, warmup, static_cast<std::uint32_t>(std::max(0, slot->info().num_inputs)),
            static_cast<std::uint32_t>(std::max(1, slot->info().num_outputs)), request.block);
    const auto json = build_result_json(*slot);
    slot->release();
    if (!atomic_write(request.result_file, json)) {
        std::fprintf(stderr, "plugin-inspect: result write failed\n"); return 12;
    }
    return 0;
}

int coordinator_main(const Request& request) {
    const auto self = current_executable_path();
    if (self.empty()) { std::fprintf(stderr, "plugin-inspect: cannot resolve executable\n"); return 1; }
    std::random_device random;
    const auto dir = fs::temp_directory_path() /
        ("pulp-plugin-inspect-" + std::to_string(random()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { std::fprintf(stderr, "plugin-inspect: cannot create temporary directory\n"); return 1; }
    const auto result_path = dir / "result.json";

    std::vector<std::string> child{"audio", "plugin-inspect", "--worker",
        "--result-file", result_path.string(), "--plugin", request.plugin,
        "--format", request.format, "--sample-rate", std::to_string(request.sample_rate),
        "--block", std::to_string(request.block)};
    if (!request.unique_id.empty()) { child.push_back("--id"); child.push_back(request.unique_id); }
    if (request.warmup_ms) {
        child.push_back("--warmup-ms"); child.push_back(std::to_string(*request.warmup_ms));
    }
    pulp::platform::ProcessOptions options;
    options.timeout_ms = static_cast<int>(std::min<std::uint32_t>(
        request.timeout_ms == 0 ? 30000u : request.timeout_ms,
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    options.max_output_bytes = 4u << 20;
    const auto result = pulp::platform::ChildProcess::run(self.string(), child, options);
    if (!result.stderr_output.empty()) std::cerr << result.stderr_output;
    if (result.timed_out) std::fprintf(stderr, "plugin-inspect: worker timed out\n");

    std::ifstream file(result_path, std::ios::binary);
    std::ostringstream json;
    json << file.rdbuf();
    fs::remove_all(dir, ec);
    if (result.timed_out || result.exit_code != 0 || json.str().empty()) {
        if (!result.timed_out)
            std::fprintf(stderr, "plugin-inspect: worker failed (exit %d)\n", result.exit_code);
        return 1;
    }
    if (request.json) std::cout << json.str();
    else std::cout << json.str();  // v1 stays machine-readable in both modes.
    return 0;
}

}  // namespace

int cmd_audio_plugin_inspect(const std::vector<std::string>& args) {
    Request request;
    std::string error;
    if (!parse_request(args, request, error)) {
        if (error == "help") { usage(); return 0; }
        std::fprintf(stderr, "pulp audio plugin-inspect: %s\n", error.c_str());
        return 2;
    }
    bool valid_format = false;
    const auto format = format_from_string(request.format, valid_format);
    if (!valid_format) {
        std::fprintf(stderr, "pulp audio plugin-inspect: unknown format '%s'\n",
                     request.format.c_str());
        return 2;
    }
    return request.worker ? worker_main(request, format) : coordinator_main(request);
}
