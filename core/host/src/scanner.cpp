// Plugin Scanner implementation
// Scans standard system directories for plugin bundles.
// Each format has platform-specific default paths.

#include <pulp/host/scanner.hpp>
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <string>

namespace fs = std::filesystem;

namespace pulp::host {

namespace {

// Parse the plugin URI out of an LV2 bundle's manifest.ttl so we can set
// PluginInfo::unique_id to the stable LV2 URI instead of the directory
// name. The URI is what `plugin_slot_lv2.cpp` keys against when selecting
// a descriptor at load time, and it's what graph_serializer rehydration
// uses to re-find the plugin across sessions (filenames collide; URIs
// don't).
//
// We only support the common manifest.ttl shape:
//   <URI> a lv2:Plugin ;  ...
// This matches LV2's canonical manifest pattern. If we can't find a URI
// stanza we fall back to the previous behavior (directory stem) so the
// scanner stays best-effort.
std::string parse_lv2_plugin_uri(const std::string& bundle_dir) {
    std::error_code ec;
    auto manifest = fs::path(bundle_dir) / "manifest.ttl";
    if (!fs::exists(manifest, ec)) return {};

    std::ifstream f(manifest);
    if (!f) return {};
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // Match only subject-position `<URI>` tokens. A single broad regex can
    // accidentally capture an object URI from predicates such as
    // `rdfs:seeAlso <plugin.ttl> ; a lv2:Plugin`, which is not the plugin ID.
    const std::regex type_re(
        R"((^|[;\s])a\s+(?:[A-Za-z_][\w-]*:[A-Za-z_][\w-]*\s*,\s*)*lv2:Plugin)",
        std::regex::ECMAScript);
    std::size_t pos = 0;
    while ((pos = content.find('<', pos)) != std::string::npos) {
        const auto line_start = content.rfind('\n', pos);
        const auto token_start = line_start == std::string::npos ? 0 : line_start + 1;
        bool first_token_on_line = true;
        for (std::size_t i = token_start; i < pos; ++i) {
            if (!std::isspace(static_cast<unsigned char>(content[i]))) {
                first_token_on_line = false;
                break;
            }
        }
        if (!first_token_on_line) {
            ++pos;
            continue;
        }

        const auto uri_end = content.find('>', pos + 1);
        if (uri_end == std::string::npos) break;

        const auto next_line = content.find('\n', uri_end + 1);
        auto statement_end = std::string::npos;
        std::size_t scan_line = next_line;
        while (scan_line != std::string::npos) {
            const auto next_token = content.find_first_not_of(" \t\r\n", scan_line);
            if (next_token == std::string::npos) break;
            if (content[next_token] == '<') {
                statement_end = next_token;
                break;
            }
            scan_line = content.find('\n', next_token);
        }
        const auto body_begin = uri_end + 1;
        const auto body_len = statement_end == std::string::npos
                            ? std::string::npos
                            : statement_end - body_begin;
        const auto body = content.substr(body_begin, body_len);
        if (std::regex_search(body, type_re)) {
            return content.substr(pos + 1, uri_end - pos - 1);
        }
        pos = uri_end + 1;
    }
    return {};
}

}  // namespace

// Forward-declared in scanner_vst3.cpp. Reads
// Contents/Resources/moduleinfo.json and returns the first audio-effect
// class's CID normalized to a 32-char lowercase hex string. Returns
// empty string when moduleinfo.json is missing or unparseable — the
// scanner then falls back to the directory stem. No dlopen, no
// bundleEntry: safe to call across an entire plugin folder.
std::string read_vst3_bundle_fuid(const std::string& path);

std::vector<std::string> PluginScanner::default_paths(PluginFormat format) {
    std::vector<std::string> paths;

#ifdef __APPLE__
    auto home = runtime::get_env("HOME");
    std::string home_str = home.value_or("");

    switch (format) {
        case PluginFormat::VST3:
            paths.push_back(home_str + "/Library/Audio/Plug-Ins/VST3");
            paths.push_back("/Library/Audio/Plug-Ins/VST3");
            break;
        case PluginFormat::AudioUnit:
        case PluginFormat::AudioUnitV3:
            paths.push_back(home_str + "/Library/Audio/Plug-Ins/Components");
            paths.push_back("/Library/Audio/Plug-Ins/Components");
            break;
        case PluginFormat::CLAP:
            paths.push_back(home_str + "/Library/Audio/Plug-Ins/CLAP");
            paths.push_back("/Library/Audio/Plug-Ins/CLAP");
            break;
        case PluginFormat::LV2:
            break; // LV2 not typical on macOS
    }
#elif defined(_WIN32)
    paths.push_back("C:\\Program Files\\Common Files\\VST3");
    paths.push_back("C:\\Program Files\\Common Files\\CLAP");
#elif defined(__linux__)
    auto home = runtime::get_env("HOME");
    std::string home_str = home.value_or("");

    switch (format) {
        case PluginFormat::VST3:
            paths.push_back(home_str + "/.vst3");
            paths.push_back("/usr/lib/vst3");
            paths.push_back("/usr/local/lib/vst3");
            break;
        case PluginFormat::CLAP:
            paths.push_back(home_str + "/.clap");
            paths.push_back("/usr/lib/clap");
            break;
        case PluginFormat::LV2:
            paths.push_back(home_str + "/.lv2");
            paths.push_back("/usr/lib/lv2");
            paths.push_back("/usr/local/lib/lv2");
            break;
        default: break;
    }
#endif

    return paths;
}

bool PluginScanner::is_plugin_bundle(const std::string& path, PluginFormat format) {
    switch (format) {
        case PluginFormat::VST3:
            return path.ends_with(".vst3");
        case PluginFormat::AudioUnit:
        case PluginFormat::AudioUnitV3:
            return path.ends_with(".component");
        case PluginFormat::CLAP:
            return path.ends_with(".clap");
        case PluginFormat::LV2:
            return path.ends_with(".lv2");
    }
    return false;
}

PluginInfo PluginScanner::scan_vst3_bundle(const std::string& path) {
    PluginInfo info;
    info.path = path;
    info.format = PluginFormat::VST3;
    info.name = fs::path(path).stem().string();

    // Prefer the real VST3 FUID (`PClassInfo::cid`) over the display name so
    // graph_serializer rehydration keys against a stable 32-char plugin
    // identity. Two plugins that happen to share a display name (e.g.
    // "Compressor.vst3") would otherwise collide when the graph serializes
    // and reloads across sessions. We read the CID from
    // Contents/Resources/moduleinfo.json — Steinberg's declarative bundle
    // metadata — so we never dlopen the plugin at scan time.
    std::string fuid = read_vst3_bundle_fuid(path);
    if (!fuid.empty()) {
        info.unique_id = std::move(fuid);
        return info;
    }
    // Fallback: bundle doesn't ship moduleinfo.json. Keep the old
    // stem-based identifier so rehydration remains best-effort.
    info.unique_id = info.name;
    return info;
}

// Forward-declared in scanner_clap.cpp when CLAP SDK is available; returns
// all descriptors in a bundle (one .clap file may host multiple plugins).
#if PULP_HOST_HAS_CLAP
std::vector<PluginInfo> scan_clap_bundle_descriptors(const std::string& path);
#endif

PluginInfo PluginScanner::scan_clap_bundle(const std::string& path) {
    // Single-entry stub retained for callers that expect one PluginInfo per
    // bundle. Real multi-plugin enumeration happens in scan_directory via
    // scan_clap_bundle_descriptors.
    PluginInfo info;
    info.path = path;
    info.format = PluginFormat::CLAP;
    info.name = fs::path(path).stem().string();
    info.unique_id = info.name;
    return info;
}

PluginInfo PluginScanner::scan_lv2_bundle(const std::string& path) {
    PluginInfo info;
    info.path = path;
    info.format = PluginFormat::LV2;
    info.name = fs::path(path).stem().string();

    // Prefer the plugin URI from manifest.ttl. The URI is what
    // plugin_slot_lv2.cpp uses at load time to select the correct descriptor,
    // and what graph_serializer rehydration matches against across sessions.
    // Falls back to the directory stem on parse failure so the scanner stays
    // best-effort.
    std::string uri = parse_lv2_plugin_uri(path);
    if (!uri.empty()) {
        info.unique_id = std::move(uri);
    } else {
        info.unique_id = info.name;
    }
    return info;
}

std::vector<PluginInfo> PluginScanner::scan_directory(const std::string& dir, PluginFormat format) {
    return scan_directory(dir, format, nullptr);
}

std::vector<PluginInfo> PluginScanner::scan_directory(const std::string& dir,
                                                     PluginFormat format,
                                                     const ScanBlacklist* blacklist) {
    std::vector<PluginInfo> results;

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return results;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        auto path = entry.path().string();
        if (!is_plugin_bundle(path, format)) continue;

        // Consult the blacklist before opening the bundle so an entry that
        // crashed a previous scan is never dlopen'd again.
        if (blacklist && blacklist->is_blacklisted(path)) continue;

        switch (format) {
            case PluginFormat::VST3:
                results.push_back(scan_vst3_bundle(path));
                break;
            case PluginFormat::CLAP: {
#if PULP_HOST_HAS_CLAP
                auto descs = scan_clap_bundle_descriptors(path);
                results.insert(results.end(), descs.begin(), descs.end());
#else
                results.push_back(scan_clap_bundle(path));
#endif
                break;
            }
            case PluginFormat::LV2:
                results.push_back(scan_lv2_bundle(path));
                break;
            default: break;
        }
    }

    return results;
}

#ifdef __APPLE__
// Implemented in scanner_au.mm using AudioComponent APIs (the supported
// discovery mechanism — also handles AUv3 app extensions).
std::vector<PluginInfo> scan_audio_units_api();

std::vector<PluginInfo> PluginScanner::scan_audio_units() {
    return scan_audio_units_api();
}
#endif

std::vector<PluginInfo> PluginScanner::scan(const ScanOptions& options) {
    std::vector<PluginInfo> all;
    int total_dirs = 0;
    int scanned = 0;

    auto scan_format = [&](PluginFormat fmt) {
        // Hermetic lanes can supply explicit scan roots without also walking
        // the machine's installed plugin collection.
        std::vector<std::string> paths;
        if (!options.only_extra_paths) {
            paths = default_paths(fmt);
        }
        for (auto& extra : options.extra_paths) paths.push_back(extra);

        for (auto& dir : paths) {
            if (options.on_progress) {
                options.on_progress(dir, scanned++, total_dirs);
            }
            // Keep blacklist filtering inside scan_directory so skipped
            // bundles are rejected before any format scanner can open them.
            auto found = scan_directory(dir, fmt, options.blacklist);
            all.insert(all.end(), found.begin(), found.end());
        }
    };

    if (options.scan_vst3)  scan_format(PluginFormat::VST3);
    if (options.scan_clap)  scan_format(PluginFormat::CLAP);
    if (options.scan_lv2)   scan_format(PluginFormat::LV2);

#ifdef __APPLE__
    // AU enumeration uses CoreAudio's AudioComponentFindNext, which walks
    // the system component registry — `extra_paths` doesn't apply. Honor
    // `only_extra_paths` by skipping AU entirely when the caller wants a
    // hermetic path-list scan.
    if (options.scan_au && !options.only_extra_paths) {
        auto au_plugins = scan_audio_units();
        all.insert(all.end(), au_plugins.begin(), au_plugins.end());
    }
#endif

    // Sort by name
    std::sort(all.begin(), all.end(),
        [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });

    runtime::log_info("PluginScanner: found {} plugins", all.size());
    return all;
}

bool plugin_info_from_au_identity(std::string_view identity, PluginInfo& info) {
    // "TYPE:SUBT:MANU" — three four-byte OSTypes, two separators.
    if (identity.size() != 14 || identity[4] != ':' || identity[9] != ':')
        return false;

    const auto type = identity.substr(0, 4);
    const bool is_instrument = type == "aumu";
    const bool is_generator = type == "augn";
    const bool is_effect = type == "aufx" || type == "aumf";
    if (!is_instrument && !is_generator && !is_effect)
        return false;

    // OSType bytes are otherwise unconstrained, but a separator inside a field
    // means the caller mis-assembled the identity rather than chose an odd code.
    if (identity.find(':', 10) != std::string_view::npos ||
        identity.substr(5, 4).find(':') != std::string_view::npos)
        return false;

    // Build into a fresh descriptor rather than overlaying the caller's. An AU
    // identity is a *complete* descriptor, and callers branch on path.empty()
    // to tell "load by identity" from "load by bundle" — overlaying would let a
    // reused PluginInfo keep a stale path and send them down the wrong branch.
    PluginInfo built;
    built.name = std::string(identity);
    built.unique_id = std::string(identity);
    built.format = PluginFormat::AudioUnit;
    built.is_instrument = is_instrument;
    built.is_effect = is_effect;
    // Instruments and generators are sources: they take no audio input.
    built.num_inputs = (is_instrument || is_generator) ? 0 : 2;
    built.num_outputs = 2;
    built.supports_midi_in = is_instrument;
    info = std::move(built);
    return true;
}

} // namespace pulp::host
