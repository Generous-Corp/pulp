// Real-plugin fixture resolution — shared between the live integration
// runner (real_plugin_runner.cpp) and the resolver unit test
// (test_real_plugin_fixture.cpp).
//
// This header is intentionally header-only and self-contained. It does not
// pull in pulp::host — the resolver layer is pure path + manifest logic so
// it can be exercised without loading any real plugin binaries.
//
// Two lanes are supported:
//
//   1. Pinned lane (preferred for CI / reproducibility)
//      sha256 is recorded in the manifest. The downloader fetches the
//      archive, verifies sha256, unpacks into the cache. The runner only
//      accepts the bundle when its on-disk hash matches the pin. This is
//      the default behaviour for the canonical free set (Surge XT, Dexed).
//
//   2. Developer-supplied lane (for auth-gated / non-redistributable
//      plugins, e.g. Vital, OB-Xd, Diva, Pro-Q4, Serum)
//      sha256 in the manifest is "TBD" because the binary cannot be
//      fetched by a plain downloader. The developer drops the bundle at
//      `$PULP_REAL_PLUGIN_CACHE/<id>/<bundle_relpath>` themselves
//      (typically by copying out of a vendor installer or symlinking the
//      system install). The runner accepts the bundle when:
//
//        a) PULP_REAL_PLUGIN_CACHE is set in the environment, AND
//        b) the bundle exists at the expected relative path, AND
//        c) the bundle has the right shape for its format
//           (`.vst3` and `.component` and `.clap` on macOS are usually
//            directory bundles; `.vst3` and `.clap` on Linux/Windows are
//            usually single files — we accept either to stay format-
//            tolerant; format-specific validation is the host's job).
//
//      The resolved fixture flags `developer_supplied = true` so the
//      runner can log a clear note distinguishing it from a hash-pinned
//      run.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::test::real_plugins {

namespace fs = std::filesystem;

// ── Manifest data model ───────────────────────────────────────────────────

struct PluginPlatform {
    std::string url;
    std::string sha256;
    std::string archive_kind;
};

struct PluginEntry {
    std::string id;
    std::string display_name;
    std::string format;
    bool is_instrument = false;
    std::string expected_name;
    std::string expected_manufacturer;
    std::string bundle_relpath;
    std::optional<PluginPlatform> macos;
    std::optional<PluginPlatform> linux_;
    std::optional<PluginPlatform> windows;
};

// ── Minimal TOML subset reader ───────────────────────────────────────────

inline std::string strip(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return std::string{s};
}

inline std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return std::string{s.substr(1, s.size() - 2)};
    return std::string{s};
}

inline PluginPlatform& ensure_platform(PluginEntry& e, std::string_view os) {
    if (os == "macos") {
        if (!e.macos) e.macos.emplace();
        return *e.macos;
    }
    if (os == "linux") {
        if (!e.linux_) e.linux_.emplace();
        return *e.linux_;
    }
    if (!e.windows) e.windows.emplace();
    return *e.windows;
}

inline void assign(PluginEntry& e, const std::string& table_path,
                   const std::string& key, const std::string& raw_value) {
    const std::string value = unquote(raw_value);

    if (table_path.empty() || table_path == "plugins") {
        if (key == "id") e.id = value;
        else if (key == "display_name") e.display_name = value;
        else if (key == "format") e.format = value;
        else if (key == "is_instrument") e.is_instrument = (raw_value == "true");
        else if (key == "expected_name") e.expected_name = value;
        else if (key == "expected_manufacturer") e.expected_manufacturer = value;
        else if (key == "bundle_relpath") e.bundle_relpath = value;
        return;
    }

    constexpr std::string_view kPlatformsPrefix = "plugins.platforms.";
    if (table_path.rfind(kPlatformsPrefix, 0) == 0) {
        const std::string os = table_path.substr(kPlatformsPrefix.size());
        PluginPlatform& p = ensure_platform(e, os);
        if (key == "url") p.url = value;
        else if (key == "sha256") p.sha256 = value;
        else if (key == "archive_kind") p.archive_kind = value;
    }
}

inline std::vector<PluginEntry> parse_real_plugins_toml(const fs::path& path) {
    std::vector<PluginEntry> out;
    std::ifstream in(path);
    if (!in) return out;

    std::string current_table;
    std::string line;
    bool inside_entry = false;
    PluginEntry cur;

    auto flush = [&]() {
        if (inside_entry) {
            out.push_back(std::move(cur));
            cur = PluginEntry{};
            inside_entry = false;
        }
    };

    while (std::getline(in, line)) {
        const std::string t = strip(line);
        if (t.empty() || t.front() == '#') continue;

        if (t.front() == '[') {
            if (t.size() >= 4 && t.substr(0, 2) == "[[" && t.substr(t.size() - 2) == "]]") {
                const std::string name = t.substr(2, t.size() - 4);
                if (name == "plugins") {
                    flush();
                    inside_entry = true;
                    current_table = "plugins";
                }
            } else if (t.front() == '[' && t.back() == ']') {
                current_table = t.substr(1, t.size() - 2);
            }
            continue;
        }

        if (!inside_entry) continue;

        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = strip(std::string_view{t}.substr(0, eq));
        std::string val = strip(std::string_view{t}.substr(eq + 1));
        if (!val.empty() && val.front() != '"') {
            const auto hash = val.find('#');
            if (hash != std::string::npos) val = strip(val.substr(0, hash));
        }
        assign(cur, current_table, key, val);
    }
    flush();
    return out;
}

// ── Cache root resolution ─────────────────────────────────────────────────
//
// Mirrors tools/scripts/fetch_real_plugins.py — both must agree on the
// location or the runner will not see what the downloader wrote.
//
// `developer_cache_override()` distinguishes "the developer explicitly set
// PULP_REAL_PLUGIN_CACHE" from "we fell back to the per-OS default". The
// developer-supplied lane only activates when the override is set, so a
// stale `~/.cache/pulp/real-plugins/vital/Vital.vst3` (e.g. left over from
// an earlier sha-pinned download) does not silently bypass the hash gate.

inline std::optional<fs::path> developer_cache_override() {
    if (const char* env = std::getenv("PULP_REAL_PLUGIN_CACHE"); env && *env)
        return fs::path(env);
    return std::nullopt;
}

inline fs::path cache_root() {
    if (auto override_ = developer_cache_override()) return *override_;
#ifdef _WIN32
    if (const char* home = std::getenv("LOCALAPPDATA"); home && *home)
        return fs::path(home) / "pulp" / "real-plugins";
#endif
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".cache" / "pulp" / "real-plugins";
    return fs::temp_directory_path() / "pulp-real-plugins";
}

// ── Per-host platform pick ────────────────────────────────────────────────

inline const PluginPlatform* platform_for_host(const PluginEntry& e) {
#if defined(__APPLE__)
    return e.macos ? &*e.macos : nullptr;
#elif defined(_WIN32)
    return e.windows ? &*e.windows : nullptr;
#else
    return e.linux_ ? &*e.linux_ : nullptr;
#endif
}

// ── Fixture resolution ────────────────────────────────────────────────────

enum class FixtureSource {
    Missing,            // No fixture available; runner SKIPs.
    PinnedDownload,     // sha256 pinned + bundle on disk (CI-friendly).
    DeveloperSupplied,  // Cache override set + bundle present, sha TBD.
};

struct ResolvedFixture {
    const PluginEntry* entry = nullptr;
    fs::path bundle_path;            // Empty when source == Missing.
    FixtureSource source = FixtureSource::Missing;
    std::string skip_reason;         // Set when source == Missing.
};

// Test seam: lets `test_real_plugin_fixture.cpp` point the resolver at a
// synthetic cache root without touching the real $HOME. Production code
// passes the empty optional so the default `cache_root()` is used.
inline ResolvedFixture resolve_fixture(const PluginEntry& e,
                                       std::optional<fs::path> cache_root_override = std::nullopt) {
    ResolvedFixture r;
    r.entry = &e;

    const PluginPlatform* p = platform_for_host(e);
    if (!p) {
        r.skip_reason = "no platform entry for host OS";
        return r;
    }

    const fs::path root = cache_root_override.value_or(cache_root());
    const fs::path bundle = root / e.id / e.bundle_relpath;
    const bool has_bundle = fs::exists(bundle);
    const bool sha_pinned = (p->sha256 != "TBD" && !p->sha256.empty());

    if (sha_pinned) {
        if (!has_bundle) {
            r.skip_reason = "fixture not downloaded (run tools/scripts/fetch_real_plugins.py)";
            return r;
        }
        r.bundle_path = bundle;
        r.source = FixtureSource::PinnedDownload;
        return r;
    }

    // sha256 is TBD — only the developer-supplied lane can rescue this entry.
    // Requires (a) the developer explicitly set PULP_REAL_PLUGIN_CACHE, and
    // (b) the bundle exists in that cache. We never accept a TBD entry from
    // the default `~/.cache/pulp/real-plugins/` path because nothing in that
    // path was verified by a hash — it could be a stale leftover from any
    // prior run.
    const bool override_set = cache_root_override.has_value()
                              || developer_cache_override().has_value();

    if (!override_set) {
        r.skip_reason = "fixture sha256 not pinned and PULP_REAL_PLUGIN_CACHE not set "
                        "(see docs/validation/real-plugins-developer-lane.md)";
        return r;
    }
    if (!has_bundle) {
        r.skip_reason = "developer-supplied bundle expected at " + bundle.string()
                        + " (see docs/validation/real-plugins-developer-lane.md)";
        return r;
    }

    // Shape check: refuse empty placeholders (`touch …/Vital.vst3`) or empty
    // directories. Without this, `PluginSlot::load` runs against the bogus
    // bundle and the integration test hard-fails instead of cleanly
    // SKIPping with actionable guidance (Codex PR #3015 P2). Mirrors the
    // checks used by `fetch_real_plugins.py --validate-cache`.
    std::error_code ec;
    if (fs::is_regular_file(bundle, ec)) {
        const auto size = fs::file_size(bundle, ec);
        if (ec || size == 0) {
            r.skip_reason = "developer-supplied bundle is empty at " + bundle.string()
                            + " (delete or replace; see "
                              "docs/validation/real-plugins-developer-lane.md)";
            return r;
        }
    } else if (fs::is_directory(bundle, ec)) {
        bool has_any = false;
        for (auto it = fs::directory_iterator(bundle, ec);
             !ec && it != fs::directory_iterator(); ++it) {
            has_any = true;
            break;
        }
        if (!has_any) {
            r.skip_reason = "developer-supplied bundle directory is empty at "
                            + bundle.string()
                            + " (delete or replace; see "
                              "docs/validation/real-plugins-developer-lane.md)";
            return r;
        }
    } else {
        r.skip_reason = "developer-supplied bundle is neither a file nor a "
                        "directory at " + bundle.string()
                        + " (see docs/validation/real-plugins-developer-lane.md)";
        return r;
    }

    r.bundle_path = bundle;
    r.source = FixtureSource::DeveloperSupplied;
    return r;
}

inline std::string_view source_label(FixtureSource s) {
    switch (s) {
        case FixtureSource::PinnedDownload:    return "pinned-download";
        case FixtureSource::DeveloperSupplied: return "developer-supplied";
        case FixtureSource::Missing:           return "missing";
    }
    return "missing";
}

} // namespace pulp::test::real_plugins
