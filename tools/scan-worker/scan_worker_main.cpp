// pulp-scan-worker — out-of-process plugin scan helper.
//
// Usage:
//   pulp-scan-worker <path-to-plugin-bundle>
//
// Reads a single plugin bundle path, runs the appropriate format scanner
// (CLAP / VST3 / AU / LV2), and emits one JSON object per descriptor on
// its original stdout, which the plugin never sees. Exit code 0 on success, non-zero on any failure. The parent
// PluginScanner relies on the process exit status to detect crashes so
// it can add the bundle to its ScanBlacklist — see
// core/host/include/pulp/host/scan_blacklist.hpp.
//
#include <pulp/host/scanner.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define PULP_DUP _dup
#define PULP_DUP2 _dup2
#define PULP_WRITE _write
#define PULP_STDOUT_FILENO 1
#define PULP_STDERR_FILENO 2
#else
#include <unistd.h>
#define PULP_DUP ::dup
#define PULP_DUP2 ::dup2
#define PULP_WRITE ::write
#define PULP_STDOUT_FILENO STDOUT_FILENO
#define PULP_STDERR_FILENO STDERR_FILENO
#endif

namespace {
namespace fs = std::filesystem;

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

// One flat JSON object per descriptor, the shape the parent's
// parse_descriptor_line reads. Built as a string so it can go to the saved
// protocol descriptor rather than whatever stdout has become.
std::string json_descriptor(const pulp::host::PluginInfo& info) {
    std::string out;
    out += "{\"name\":\"" + json_escape(info.name) + "\",";
    out += "\"manufacturer\":\"" + json_escape(info.manufacturer) + "\",";
    out += "\"version\":\"" + json_escape(info.version) + "\",";
    out += "\"unique_id\":\"" + json_escape(info.unique_id) + "\",";
    out += "\"path\":\"" + json_escape(info.path) + "\",";
    out += std::string("\"is_instrument\":") + (info.is_instrument ? "true" : "false") + ",";
    out += std::string("\"is_effect\":") + (info.is_effect ? "true" : "false") + ",";
    out += "\"num_inputs\":" + std::to_string(info.num_inputs) + ",";
    out += "\"num_outputs\":" + std::to_string(info.num_outputs) + ",";
    out += "\"category\":\"" + json_escape(info.category) + "\",";
    out += "\"description\":\"" + json_escape(info.description) + "\",";
    out += "\"features\":[";
    for (std::size_t i = 0; i < info.features.size(); ++i) {
        if (i)
            out += ",";
        out += "\"" + json_escape(info.features[i]) + "\"";
    }
    out += "],";
    out += std::string("\"has_editor\":") + (info.has_editor ? "true" : "false") + ",";
    out +=
        std::string("\"supports_sidechain\":") + (info.supports_sidechain ? "true" : "false") + ",";
    out += std::string("\"supports_midi_in\":") + (info.supports_midi_in ? "true" : "false") + ",";
    out +=
        std::string("\"supports_midi_out\":") + (info.supports_midi_out ? "true" : "false") + ",";
    const char* fmt = "unknown";
    switch (info.format) {
        case pulp::host::PluginFormat::CLAP: fmt = "clap"; break;
        case pulp::host::PluginFormat::VST3: fmt = "vst3"; break;
        case pulp::host::PluginFormat::AudioUnit:   fmt = "au";   break;
        case pulp::host::PluginFormat::AudioUnitV3: fmt = "auv3"; break;
        case pulp::host::PluginFormat::LV2:  fmt = "lv2";  break;
        default: break;
    }
    out += std::string("\"format\":\"") + fmt + "\"}\n";
    return out;
}

bool write_all(int fd, const std::string& text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const auto n =
            PULP_WRITE(fd, text.data() + done, static_cast<unsigned>(text.size() - done));
        if (n <= 0)
            return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// Lex-only normalization: collapse `.`/`..` segments and resolve to an
// absolute path WITHOUT following symlinks. `weakly_canonical` resolves
// symlinks, so two `.clap`/`.vst3` aliases pointing at the same real
// bundle would compare equal and the worker would lose the ability to
// filter to the exact directory entry it was asked to scan. The parent
// scanner only consumes the first JSON line of our output, so a request
// for one symlink could otherwise return or blacklist a sibling alias
// depending on sort order.
fs::path normalized_bundle_path(const std::string& path) {
    std::error_code ec;
    auto absolute = fs::absolute(fs::path(path), ec);
    if (ec) return fs::path(path).lexically_normal();
    return absolute.lexically_normal();
}

bool same_bundle_path(const std::string& lhs, const std::string& rhs) {
    return normalized_bundle_path(lhs) == normalized_bundle_path(rhs);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: pulp-scan-worker <path-to-plugin-bundle>\n");
        return 2;
    }
    const std::string path = argv[1];

    // Pick the scanner by file extension and keep the parent in charge of
    // format routing. Exactly this bundle runs in this process: scanning its
    // whole directory would let a crashing neighbour take the worker down and
    // get this bundle blacklisted for it.
    pulp::host::PluginFormat format;
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".clap") == 0) {
        format = pulp::host::PluginFormat::CLAP;
    } else if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".vst3") == 0) {
        format = pulp::host::PluginFormat::VST3;
    } else {
        std::fprintf(stderr, "pulp-scan-worker: unsupported bundle extension: %s\n", path.c_str());
        return 3;
    }
    // The parent reads descriptors from this process's original stdout. A
    // plugin's entry point may print whatever it likes, so that channel is
    // saved first and stdout is pointed at stderr before any plugin code runs;
    // descriptors then go to the saved descriptor and nothing the plugin
    // prints can land between or inside them.
    std::fflush(stdout);
    const int protocol_fd = PULP_DUP(PULP_STDOUT_FILENO);
    if (protocol_fd < 0 || PULP_DUP2(PULP_STDERR_FILENO, PULP_STDOUT_FILENO) < 0) {
        std::fprintf(stderr, "pulp-scan-worker: cannot detach stdout from the plugin\n");
        return 2;
    }
    const auto infos = pulp::host::PluginScanner::scan_bundle(path, format);
    // Keep only the descriptor(s) for our target bundle.
    std::vector<pulp::host::PluginInfo> filtered;
    for (const auto& i : infos) {
        if (same_bundle_path(i.path, path)) filtered.push_back(i);
    }

    std::string lines;
    for (const auto& info : filtered)
        lines += json_descriptor(info);
    if (!write_all(protocol_fd, lines)) {
        std::fprintf(stderr, "pulp-scan-worker: cannot write descriptors\n");
        return 2;
    }
    std::fflush(stdout);
    return 0;
}
