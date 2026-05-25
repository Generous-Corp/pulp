// pulp-scan-worker — out-of-process plugin scan helper.
//
// Usage:
//   pulp-scan-worker <path-to-plugin-bundle>
//
// Reads a single plugin bundle path, runs the appropriate format scanner
// (CLAP / VST3 / AU / LV2), and emits one JSON object per descriptor on
// stdout. Exit code 0 on success, non-zero on any failure. The parent
// PluginScanner relies on the process exit status to detect crashes so
// it can add the bundle to its ScanBlacklist — see
// core/host/include/pulp/host/scan_blacklist.hpp.
//
// Workstream 03 slice 3.3b. This first commit ships the binary skeleton;
// the parent-side `ChildProcessManager` wiring that actually spawns the
// worker and the JSON schema negotiation land in follow-up slices.

#include <pulp/host/scanner.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b";  break;
            case '\f': escaped += "\\f";  break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[7] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    escaped += buf;
                } else {
                    escaped += static_cast<char>(c);
                }
                break;
        }
    }
    return escaped;
}

void write_json_descriptor(const pulp::host::PluginInfo& info) {
    // Minimal JSON emitter — no nested objects, just the flat shape the
    // parent expects. Keeps this TU free of the full json/value code so
    // the worker binary stays tiny and quick to fork.
    const auto name = json_escape(info.name);
    const auto manufacturer = json_escape(info.manufacturer);
    const auto version = json_escape(info.version);
    const auto unique_id = json_escape(info.unique_id);
    const auto path = json_escape(info.path);
    std::printf("{\"name\":\"%s\",\"manufacturer\":\"%s\",\"version\":\"%s\",",
                name.c_str(), manufacturer.c_str(), version.c_str());
    std::printf("\"unique_id\":\"%s\",\"path\":\"%s\",",
                unique_id.c_str(), path.c_str());
    std::printf("\"is_instrument\":%s,\"is_effect\":%s,",
                info.is_instrument ? "true" : "false",
                info.is_effect ? "true" : "false");
    const char* fmt = "unknown";
    switch (info.format) {
        case pulp::host::PluginFormat::CLAP: fmt = "clap"; break;
        case pulp::host::PluginFormat::VST3: fmt = "vst3"; break;
        case pulp::host::PluginFormat::AudioUnit:   fmt = "au";   break;
        case pulp::host::PluginFormat::AudioUnitV3: fmt = "auv3"; break;
        case pulp::host::PluginFormat::LV2:  fmt = "lv2";  break;
        default: break;
    }
    std::printf("\"format\":\"%s\"}\n", fmt);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pulp-scan-worker <path-to-plugin-bundle>\n");
        return 2;
    }
    const std::string path = argv[1];

    // Pick the scanner by file extension. Extending to heuristic
    // detection is a follow-up; this first pass keeps the parent in
    // charge of format routing.
    // Route through the public scan() API with the containing folder
    // added to extra_paths. Filtering to the exact bundle keeps us from
    // reporting other plug-ins that happen to live alongside it.
    auto slash = path.find_last_of("/\\");
    std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);

    pulp::host::PluginScanner scanner;
    pulp::host::ScanOptions opts;
    // Narrow the formats to ones we understand so the scanner does less work.
    opts.scan_vst3 = opts.scan_clap = false;
    opts.scan_au   = opts.scan_lv2  = false;
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".clap") == 0) {
        opts.scan_clap = true;
    } else if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".vst3") == 0) {
        opts.scan_vst3 = true;
    } else {
        std::fprintf(stderr, "pulp-scan-worker: unsupported bundle extension: %s\n", path.c_str());
        return 3;
    }
    opts.only_extra_paths = true;
    opts.extra_paths.push_back(dir);
    auto infos = scanner.scan(opts);
    // Keep only the descriptor(s) for our target bundle.
    std::vector<pulp::host::PluginInfo> filtered;
    for (const auto& i : infos) if (i.path == path) filtered.push_back(i);

    for (const auto& info : filtered) {
        write_json_descriptor(info);
    }
    std::fflush(stdout);
    return 0;
}
