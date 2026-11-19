// Plugin node resolution for GraphSerializer loads: decides which bundle path
// a persisted plugin node loads from, checking the known plugin roots and
// scanning them by identity when the stored path does not resolve.

#include "graph_serializer_plugin_resolver.hpp"

#include <pulp/host/graph_serializer.hpp>

// The web DSP builds (Emscripten for WAM, plain clang targeting wasm32 for
// WebCLAP) compile this serializer without the plugin scanner and have no
// plugin bundles to load; the resolver below has a stub for them.
#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(__wasm32__)
#define PULP_GRAPH_SERIALIZER_WITHOUT_PLUGIN_SCANNER 1
#endif

#if !defined(PULP_GRAPH_SERIALIZER_WITHOUT_PLUGIN_SCANNER)
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scan_cache.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/safe_relative_path.hpp>

#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace pulp::host::detail {

#if defined(PULP_GRAPH_SERIALIZER_WITHOUT_PLUGIN_SCANNER)
// The web build compiles this serializer without the plugin scanner and has
// no plugin bundles to load, so every plugin node stays an unresolved
// placeholder: nothing is opened and no scanner symbol is referenced.
class PluginNodeResolver::Impl {
  public:
    explicit Impl(const GraphLoadOptions&) {}

    std::optional<std::string> resolve(const PluginInfo&, std::string& reason) {
        reason = "hosted plugins are not loadable in the web build";
        return std::nullopt;
    }
};
#else
// Resolves plugin nodes for one from_json call. The scan of the known roots
// is run at most once per format, and only when a node's path hint does not
// already lie inside a root.
class PluginNodeResolver::Impl {
  public:
    explicit Impl(const GraphLoadOptions& options) : options_(options) {}

    // Returns the path to load, or nullopt with `reason` set.
    std::optional<std::string> resolve(const PluginInfo& info, std::string& reason) {
        if (!options_.load_plugins) {
            reason = "plugin loading is disabled for this load";
            return std::nullopt;
        }
        // Audio Units are found through the component registry by identity;
        // the stored path is never opened.
        if (info.format == PluginFormat::AudioUnit || info.format == PluginFormat::AudioUnitV3 ||
            info.format == PluginFormat::BuiltIn) {
            return info.path;
        }
        if (!info.path.empty() && path_within_known_root(info.path, info.format)) {
            if (options_.blacklist && options_.blacklist->is_blacklisted(info.path)) {
                reason = "last_path is blacklisted from a previous scan crash";
                return std::nullopt;
            }
            return info.path;
        }
        if (options_.scan_mode != GraphPluginScanMode::none && !info.unique_id.empty()) {
            const auto& scan = scanned(info.format);
            for (const auto& installed : scan.plugins) {
                if (installed.unique_id == info.unique_id && !installed.path.empty()) {
                    return installed.path;
                }
            }
            if (scan.clap_skipped_no_worker) {
                reason = "no scan worker found for isolated plugin resolution; " + scan.error;
                return std::nullopt;
            }
        }
        if (options_.allow_unrooted_plugin_paths && !info.path.empty()) {
            runtime::log_warn("GraphSerializer: loading '{}' from outside the known plugin roots "
                              "because unrooted paths were allowed",
                              info.path);
            return info.path;
        }
        if (options_.scan_mode == GraphPluginScanMode::none) {
            reason = info.path.empty()
                         ? "no last_path and scanning the plugin roots is disabled"
                         : "last_path is outside the known plugin roots and scanning them "
                           "for this identity is disabled";
            return std::nullopt;
        }
        reason =
            info.path.empty()
                ? "no installed plugin with this identity in the known plugin roots"
                : "last_path is outside the known plugin roots and no installed plugin matches "
                  "this identity";
        return std::nullopt;
    }

  private:
    std::vector<std::string> roots_for(PluginFormat format) const {
        std::vector<std::string> roots;
        if (options_.include_default_plugin_roots)
            roots = PluginScanner::default_paths(format);
        roots.insert(roots.end(), options_.plugin_roots.begin(), options_.plugin_roots.end());
        return roots;
    }

    bool path_within_known_root(const std::string& path, PluginFormat format) const {
        std::error_code ec;
        const auto real = std::filesystem::canonical(path, ec);
        if (ec)
            return false;
        for (const auto& root : roots_for(format)) {
            const auto real_root = std::filesystem::canonical(root, ec);
            if (ec || real_root.empty())
                continue;
            if (runtime::is_within_directory(real_root, real))
                return true;
        }
        return false;
    }

    const ScanReport& scanned(PluginFormat format) {
        auto it = scans_.find(format);
        if (it != scans_.end())
            return it->second;
        ScanOptions scan;
        scan.scan_vst3 = format == PluginFormat::VST3;
        scan.scan_clap = format == PluginFormat::CLAP;
        scan.scan_lv2 = format == PluginFormat::LV2;
        scan.scan_au = false;
        scan.extra_paths = options_.plugin_roots;
        scan.only_extra_paths = !options_.include_default_plugin_roots;
        scan.scan_worker_path = options_.scan_worker_path;
        // The scan records any bundle that crashes its worker into the
        // caller's own blacklist, and reads and feeds the caller's cache.
        scan.blacklist = options_.blacklist;
        scan.scan_cache = options_.scan_cache;
        auto report = PluginScanner{}.scan_report(scan);
        if (report.clap_skipped_no_worker &&
            options_.scan_mode == GraphPluginScanMode::allow_in_process) {
            // The caller accepts entry points running here when no worker
            // exists; isolated_only callers get the skipped report instead.
            scan.in_process_clap = true;
            report = PluginScanner{}.scan_report(scan);
        }
        return scans_.emplace(format, std::move(report)).first->second;
    }

    const GraphLoadOptions& options_;
    std::unordered_map<PluginFormat, ScanReport> scans_;
};
#endif // PULP_GRAPH_SERIALIZER_WITHOUT_PLUGIN_SCANNER

PluginNodeResolver::PluginNodeResolver(const GraphLoadOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

PluginNodeResolver::~PluginNodeResolver() = default;

std::optional<std::string> PluginNodeResolver::resolve(const PluginInfo& info,
                                                       std::string& reason) {
    return impl_->resolve(info, reason);
}

} // namespace pulp::host::detail
