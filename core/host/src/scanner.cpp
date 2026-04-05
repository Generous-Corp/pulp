// Plugin Scanner implementation
// Scans standard system directories for plugin bundles.
// Each format has platform-specific default paths.

#include <pulp/host/scanner.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace pulp::host {

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
    info.unique_id = info.name; // TODO: read moduleinfo.json for proper FUID
    return info;
}

PluginInfo PluginScanner::scan_clap_bundle(const std::string& path) {
    PluginInfo info;
    info.path = path;
    info.format = PluginFormat::CLAP;
    info.name = fs::path(path).stem().string();
    info.unique_id = info.name; // TODO: load and query clap_plugin_descriptor
    return info;
}

PluginInfo PluginScanner::scan_lv2_bundle(const std::string& path) {
    PluginInfo info;
    info.path = path;
    info.format = PluginFormat::LV2;
    info.name = fs::path(path).stem().string();
    info.unique_id = info.name; // TODO: parse manifest.ttl for plugin URI
    return info;
}

std::vector<PluginInfo> PluginScanner::scan_directory(const std::string& dir, PluginFormat format) {
    std::vector<PluginInfo> results;

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return results;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        auto path = entry.path().string();
        if (!is_plugin_bundle(path, format)) continue;

        switch (format) {
            case PluginFormat::VST3:
                results.push_back(scan_vst3_bundle(path));
                break;
            case PluginFormat::CLAP:
                results.push_back(scan_clap_bundle(path));
                break;
            case PluginFormat::LV2:
                results.push_back(scan_lv2_bundle(path));
                break;
            default: break;
        }
    }

    return results;
}

#ifdef __APPLE__
std::vector<PluginInfo> PluginScanner::scan_audio_units() {
    std::vector<PluginInfo> results;
    // Scan .component bundles in AU directories
    for (auto& dir : default_paths(PluginFormat::AudioUnit)) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            auto path = entry.path().string();
            if (path.ends_with(".component")) {
                PluginInfo info;
                info.path = path;
                info.format = PluginFormat::AudioUnit;
                info.name = entry.path().stem().string();
                info.unique_id = info.name;
                results.push_back(std::move(info));
            }
        }
    }
    return results;
}
#endif

std::vector<PluginInfo> PluginScanner::scan(const ScanOptions& options) {
    std::vector<PluginInfo> all;
    int total_dirs = 0;
    int scanned = 0;

    auto scan_format = [&](PluginFormat fmt) {
        auto paths = default_paths(fmt);
        for (auto& extra : options.extra_paths) paths.push_back(extra);

        for (auto& dir : paths) {
            if (options.on_progress) {
                options.on_progress(dir, scanned++, total_dirs);
            }
            auto found = scan_directory(dir, fmt);
            all.insert(all.end(), found.begin(), found.end());
        }
    };

    if (options.scan_vst3)  scan_format(PluginFormat::VST3);
    if (options.scan_clap)  scan_format(PluginFormat::CLAP);
    if (options.scan_lv2)   scan_format(PluginFormat::LV2);

#ifdef __APPLE__
    if (options.scan_au) {
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

} // namespace pulp::host
