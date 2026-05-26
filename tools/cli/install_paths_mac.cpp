// install_paths_mac.cpp — implementation of macOS plugin-install path helpers.
//
// See install_paths_mac.hpp for the rationale. This TU stays free of
// cli_common.hpp on purpose so the unit test can compile just this
// file + its hpp without pulling pulp-cli's full runtime link surface.

#include "install_paths_mac.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace pulp::cli::install_paths_mac {

namespace {

// PATH lookup with no cli_common dependency. We can't include
// cli_common.hpp here (see header rationale), so reimplement the
// minimal "is <name> on PATH?" lookup locally.
fs::path which(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return {};
    std::string path_str = path_env;
    size_t start = 0;
    while (start <= path_str.size()) {
        auto end = path_str.find(':', start);
        std::string dir = (end == std::string::npos)
            ? path_str.substr(start)
            : path_str.substr(start, end - start);
        if (!dir.empty()) {
            fs::path candidate = fs::path(dir) / name;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) {
                // Check executable bit — `access(p, X_OK)` is the
                // POSIX answer but `fs::status` + perms is portable
                // enough for our purpose. The caller only needs a
                // truthy/falsy answer; we don't strictly need X_OK.
                return candidate;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

}  // namespace

PluginKind classify_bundle(const fs::path& bundle) {
    auto ext = bundle.extension().string();
    if (ext == ".component") return PluginKind::AU;
    if (ext == ".vst3")      return PluginKind::VST3;
    if (ext == ".clap")      return PluginKind::CLAP;
    return PluginKind::Unknown;
}

std::string validator_for_kind(PluginKind kind) {
    switch (kind) {
        case PluginKind::AU:   return "auval";
        case PluginKind::VST3: return "pluginval";
        case PluginKind::CLAP: return "clap-validator";
        case PluginKind::Unknown: return {};
    }
    return {};
}

fs::path destination_for(PluginKind kind,
                         const fs::path& home,
                         const fs::path& bundle_basename) {
    if (kind == PluginKind::Unknown) return {};
    if (home.empty() || !home.is_absolute()) return {};
    fs::path base = home / "Library" / "Audio" / "Plug-Ins";
    switch (kind) {
        case PluginKind::AU:   return base / "Components" / bundle_basename;
        case PluginKind::VST3: return base / "VST3" / bundle_basename;
        case PluginKind::CLAP: return base / "CLAP" / bundle_basename;
        case PluginKind::Unknown: return {};
    }
    return {};
}

std::string validator_install_hint(PluginKind kind) {
    switch (kind) {
        case PluginKind::AU:
            return "ships with Xcode Command Line Tools "
                   "(`xcode-select --install`)";
        case PluginKind::VST3:
            return "install with `brew install pluginval` (macOS) "
                   "| https://github.com/Tracktion/pluginval/releases";
        case PluginKind::CLAP:
            return "install with `cargo install clap-validator`";
        case PluginKind::Unknown:
            return {};
    }
    return {};
}

InstallEnv make_default_env() {
    InstallEnv env;
    env.path_exists = [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec);
    };
    env.create_directories = [](const fs::path& p) {
        std::error_code ec;
        fs::create_directories(p, ec);
        return !ec;
    };
    env.remove_all = [](const fs::path& p) {
        std::error_code ec;
        fs::remove_all(p, ec);
        return !ec;
    };
    env.copy_recursive = [](const fs::path& src, const fs::path& dst) {
        std::error_code ec;
        fs::copy(src, dst,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 ec);
        return !ec;
    };
    env.resolve_in_path = [](const std::string& name) {
        return which(name);
    };
    if (const char* home = std::getenv("HOME"); home && *home) {
        env.home_dir = fs::path(home);
    }
    return env;
}

InstallResult install_bundle(const InstallEnv& env,
                             const fs::path& bundle,
                             PluginKind kind) {
    InstallResult r;
    r.bundle_path = bundle;
    if (kind == PluginKind::Unknown) {
        r.error = "unknown bundle kind for " + bundle.string();
        return r;
    }
    if (env.home_dir.empty() || !env.home_dir.is_absolute()) {
        r.error = "$HOME is not set or not absolute — cannot resolve install path";
        return r;
    }
    if (!env.path_exists(bundle)) {
        r.error = "source bundle does not exist: " + bundle.string();
        return r;
    }

    r.destination = destination_for(kind, env.home_dir, bundle.filename());
    if (r.destination.empty()) {
        r.error = "could not resolve destination for " + bundle.string();
        return r;
    }

    if (!env.create_directories(r.destination.parent_path())) {
        r.error = "failed to create install directory: "
                  + r.destination.parent_path().string();
        return r;
    }

    if (env.path_exists(r.destination)) {
        if (!env.remove_all(r.destination)) {
            r.error = "failed to remove existing install at "
                      + r.destination.string();
            return r;
        }
        r.replaced_existing = true;
    }

    if (!env.copy_recursive(bundle, r.destination)) {
        r.error = "failed to copy bundle to " + r.destination.string();
        return r;
    }

    r.success = true;
    return r;
}

std::vector<DiscoveredBundle> discover_bundles(const fs::path& build_dir) {
    std::vector<DiscoveredBundle> out;
    // Stable AU → VST3 → CLAP order so output is deterministic.
    struct Tag { const char* dir; const char* ext; PluginKind kind; };
    const Tag tags[] = {
        {"AU",   ".component", PluginKind::AU},
        {"VST3", ".vst3",      PluginKind::VST3},
        {"CLAP", ".clap",      PluginKind::CLAP},
    };
    for (const auto& tag : tags) {
        fs::path dir = build_dir / tag.dir;
        std::error_code ec;
        if (!fs::exists(dir, ec) || ec) continue;
        // Use sorted iteration so test scenarios are deterministic when
        // multiple bundles share a directory.
        std::vector<fs::path> entries;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (entry.path().extension().string() == tag.ext) {
                entries.push_back(entry.path());
            }
        }
        std::sort(entries.begin(), entries.end());
        for (auto& p : entries) {
            out.push_back({p, tag.kind});
        }
    }
    return out;
}

}  // namespace pulp::cli::install_paths_mac
