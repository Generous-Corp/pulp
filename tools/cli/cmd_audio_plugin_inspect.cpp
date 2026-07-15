// cmd_audio_plugin_inspect.cpp — `pulp audio plugin-inspect`
//
// Loads vendor code only in a disposable child process and publishes a single
// JSON artifact after successful load/prepare/warm-up/parameter enumeration.

#include "cmd_audio_plugin_inspect.hpp"

#include "cmd_audio_plugin_common.hpp"

#include <pulp/host/plugin_slot.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
        << "  --timeout-ms <n>                  Worker timeout (default: at least 30000;\n"
        << "                                      scales with warm-up)\n\n"
        << "Output is always pulp.audio.plugin-inspect.v1 JSON.\n";
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

bool parse_request(const std::vector<std::string>& args, Request& out, std::string& error,
                   bool allow_internal = false) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
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
            if (!allow_internal) { error = "unknown flag: " + key; return false; }
            auto v = value(); if (!v) { error = "--result-file requires a value"; return false; }
            out.result_file = *v;
        } else {
            error = "unknown flag: " + key;
            return false;
        }
    }
    if (out.plugin.empty()) { error = "--plugin <bundle> is required"; return false; }
    if (allow_internal && out.result_file.empty()) {
        error = "worker result path is required"; return false;
    }
    return true;
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

std::string build_result_json(const pulp::host::PluginSlot& slot) {
    const auto& info = slot.info();
    const auto params = slot.parameters();
    std::ostringstream out;
    out.imbue(std::locale::classic());
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
    auto info = pulp::cli::plugin_lab::resolve_plugin_info(
        request.plugin, format, request.unique_id);

    auto slot = pulp::host::PluginSlot::load(info);
    if (!slot) { std::fprintf(stderr, "plugin-inspect: load failed\n"); return 10; }
    if (!slot->prepare(request.sample_rate, static_cast<int>(request.block))) {
        std::fprintf(stderr, "plugin-inspect: prepare failed\n"); return 11;
    }
    const auto warmup = request.warmup_ms.value_or(
        pulp::cli::plugin_lab::default_warmup_ms(format));
    pulp::cli::plugin_lab::process_discarded_preroll(
        *slot, warmup,
        static_cast<std::uint32_t>(std::max(0, slot->info().num_inputs)),
        static_cast<std::uint32_t>(std::max(1, slot->info().num_outputs)), request.block);
    const auto json = build_result_json(*slot);
    slot->release();
    if (!atomic_write(request.result_file, json)) {
        std::fprintf(stderr, "plugin-inspect: result write failed\n"); return 12;
    }
    return 0;
}

int coordinator_main(const Request& request, PluginFormat format) {
    auto temp = pulp::cli::plugin_lab::PrivateTempDirectory::create("pulp-plugin-inspect");
    if (!temp) { std::fprintf(stderr, "plugin-inspect: cannot create temporary directory\n"); return 1; }
    const auto result_path = temp->path() / "result.json";

    std::vector<std::string> child{"audio", "__plugin-inspect-worker",
        "--result-file", result_path.string(), "--plugin", request.plugin,
        "--format", request.format, "--sample-rate", std::to_string(request.sample_rate),
        "--block", std::to_string(request.block)};
    if (!request.unique_id.empty()) { child.push_back("--id"); child.push_back(request.unique_id); }
    if (request.warmup_ms) {
        child.push_back("--warmup-ms"); child.push_back(std::to_string(*request.warmup_ms));
    }
    const auto warmup_ms = request.warmup_ms.value_or(
        pulp::cli::plugin_lab::default_warmup_ms(format));
    const auto timeout_ms = static_cast<int>(std::min<std::uint32_t>(
        plugin_inspect_timeout_ms(
            request.timeout_ms == 0 ? std::nullopt
                                    : std::optional<std::uint32_t>{request.timeout_ms},
            warmup_ms),
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    const auto result = pulp::cli::plugin_lab::run_disposable_worker(child, timeout_ms);
    if (!result.stderr_output.empty()) std::cerr << result.stderr_output;
    if (result.timed_out) std::fprintf(stderr, "plugin-inspect: worker timed out\n");

    std::ifstream file(result_path, std::ios::binary);
    std::ostringstream json;
    json << file.rdbuf();
    if (result.timed_out || result.exit_code != 0 || json.str().empty()) {
        if (!result.timed_out)
            std::fprintf(stderr, "plugin-inspect: worker failed (exit %d)\n", result.exit_code);
        return 1;
    }
    std::cout << json.str();
    return 0;
}

}  // namespace

int run_plugin_inspect(const std::vector<std::string>& args, bool worker) {
    Request request;
    std::string error;
    if (!parse_request(args, request, error, worker)) {
        if (error == "help") { usage(); return 0; }
        std::fprintf(stderr, "pulp audio plugin-inspect: %s\n", error.c_str());
        return 2;
    }
    const auto format = pulp::cli::plugin_lab::parse_format(request.format);
    if (!format) {
        std::fprintf(stderr, "pulp audio plugin-inspect: unknown format '%s'\n",
                     request.format.c_str());
        return 2;
    }
    return worker ? worker_main(request, *format) : coordinator_main(request, *format);
}

int cmd_audio_plugin_inspect(const std::vector<std::string>& args) {
    return run_plugin_inspect(args, false);
}

int cmd_audio_plugin_inspect_worker(const std::vector<std::string>& args) {
    return run_plugin_inspect(args, true);
}
