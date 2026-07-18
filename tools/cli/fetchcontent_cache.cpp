// fetchcontent_cache.cpp — Implementation of the discovery and healing
// helpers powering `pulp doctor --caches[ --fix]` and the cache
// preflight inside `pulp build` / `pulp test`.
//
// Pure-logic core + a small set of narrow filesystem helpers. Only
// `make_real_env` and `apply_fixes` interact with the disk; everything
// else operates on the injected `DiscoveryEnv` callables, which keeps
// the unit tests deterministic and decoupled from the developer's
// real `~/Library/Caches/Pulp/` state.

#include "fetchcontent_cache.hpp"
#include "json_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ostream>
#include <regex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  include <cstdlib>   // _dupenv_s
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace pulp::cli::fetchcontent_cache {

namespace {

// Local home-directory lookup — we deliberately don't depend on
// cli_common's helper so this TU links cleanly into the standalone
// unit-test binary, mirroring `version_diag` / `projects_registry`.
fs::path user_home_dir_local() {
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "USERPROFILE") == 0 && buf) {
        fs::path p(buf);
        std::free(buf);
        return p;
    }
    return {};
#else
    if (const char* h = std::getenv("HOME")) return fs::path(h);
    return {};
#endif
}

std::string env_string(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        std::string out(buf);
        std::free(buf);
        return out;
    }
    return {};
#else
    if (const char* v = std::getenv(name)) return std::string(v);
    return {};
#endif
}

// Lowercase ASCII — used to canonicalise dependency names so the
// CMakeLists.txt scrape can be matched against the on-disk cache
// directory naming convention (which is always lowercased by
// `pulp_register_fetchcontent_source`).
std::string to_lower_ascii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Mirror of `pulp_sanitize_cache_suffix` in PulpFetchContent.cmake.
// Replaces any non-alphanumeric / non-`._-` byte with `_`, collapses
// runs of `_`, then trims leading/trailing `_`. Only used so the
// declared-ref ↔ cached-ref comparison can be done apples-to-apples
// (a declared REF of `release/3.2.12` becomes `release_3.2.12`).
std::string sanitize_ref(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool prev_underscore = false;
    for (char c : input) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) {
            out.push_back(c);
            prev_underscore = false;
        } else {
            if (!prev_underscore) out.push_back('_');
            prev_underscore = true;
        }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// Split a directory name like "threejs-077dd13c…" into ("threejs",
// "077dd13c…"). The separator is the LAST '-' so dep names that
// themselves contain hyphens (e.g. "wgpu-macos-aarch64") still parse
// — we keep the longest matching dep prefix and treat the trailing
// chunk as the cached ref.
std::pair<std::string, std::string> split_entry_name(
    const std::string& name, const DeclaredRefs& declared) {
    // Try longest-prefix match against the declared dep names first.
    // Names are lowercased on both sides.
    std::string lower = to_lower_ascii(name);
    std::string best_dep;
    for (const auto& [dep, ref] : declared) {
        if (lower.size() <= dep.size()) continue;
        if (lower.compare(0, dep.size(), dep) != 0) continue;
        if (lower[dep.size()] != '-') continue;
        if (dep.size() > best_dep.size()) best_dep = dep;
    }
    if (!best_dep.empty()) {
        return {best_dep, name.substr(best_dep.size() + 1)};
    }
    // Fall back: split at last '-'.
    auto pos = name.rfind('-');
    if (pos == std::string::npos) return {to_lower_ascii(name), {}};
    return {to_lower_ascii(name.substr(0, pos)), name.substr(pos + 1)};
}

// Recursive remove that swallows the not-found case (which is fine
// — we just want the path gone). Other errors propagate via the
// out-parameter so callers can attach the message to FixResult.
bool remove_path_safely(const fs::path& path, std::string& err) {
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        err = "symlink_status: " + ec.message();
        return false;
    }
    if (!fs::exists(status)) return true;  // already gone

    // Symlinks must be removed via fs::remove (not remove_all, which
    // refuses on some platforms). Plain files & dirs use remove_all
    // because a stale-commit cache is always a directory tree.
    if (fs::is_symlink(status)) {
        fs::remove(path, ec);
    } else {
        fs::remove_all(path, ec);
    }
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
}

}  // namespace

const char* status_label(CacheStatus s) {
    switch (s) {
        case CacheStatus::Healthy:     return "healthy";
        case CacheStatus::Dangling:    return "dangling-symlink";
        case CacheStatus::StaleCommit: return "stale-commit";
        case CacheStatus::RootOwned:   return "root-owned";
        case CacheStatus::Unknown:     return "unknown";
    }
    return "unknown";
}

const char* fix_outcome_label(FixOutcome o) {
    switch (o) {
        case FixOutcome::Removed: return "removed";
        case FixOutcome::Skipped: return "skipped";
        case FixOutcome::Failed:  return "failed";
        case FixOutcome::DryRun:  return "dry-run";
    }
    return "unknown";
}

fs::path default_cache_root() {
    auto override_env = env_string("PULP_SHARED_FETCHCONTENT_SOURCE_DIR");
    if (!override_env.empty()) return fs::path(override_env);

#if defined(__APPLE__)
    auto home = user_home_dir_local();
    if (home.empty()) return {};
    return home / "Library" / "Caches" / "Pulp" / "fetchcontent-src";
#elif defined(_WIN32)
    auto local = env_string("LOCALAPPDATA");
    if (!local.empty()) return fs::path(local) / "Pulp" / "fetchcontent-src";
    auto user = env_string("USERPROFILE");
    if (!user.empty()) {
        return fs::path(user) / "AppData" / "Local" / "Pulp" / "fetchcontent-src";
    }
    return {};
#else
    auto xdg = env_string("XDG_CACHE_HOME");
    if (!xdg.empty()) return fs::path(xdg) / "pulp" / "fetchcontent-src";
    auto home = user_home_dir_local();
    if (home.empty()) return {};
    return home / ".cache" / "pulp" / "fetchcontent-src";
#endif
}

DeclaredRefs parse_declared_refs_from_text(const std::string& text) {
    DeclaredRefs out;
    // Strip CMake `#`-comments line by line first, so a commented-out
    // example like `# pulp_register_fetchcontent_source(foo REF bar)`
    // doesn't show up as a phantom declared ref. CMake's `#` is
    // line-only (no block comment that starts on a non-comment line),
    // so a straight first-`#` cut per line is sufficient.
    std::string stripped;
    stripped.reserve(text.size());
    {
        std::stringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            auto h = line.find('#');
            if (h != std::string::npos) line.resize(h);
            stripped += line;
            stripped += '\n';
        }
    }

    // Match `pulp_register_fetchcontent_source(<name> [...] REF <ref> [...])`.
    // The body between the parens may contain other keyword args (DIR,
    // EXCLUDE_FROM_ALL, etc.) in any order, so we capture the body
    // first and pull `REF <token>` out of that. Multi-line calls are
    // common — std::regex with no special flag handles `\s` across
    // newlines because we explicitly include them in the char class.
    std::regex call_re(
        R"(pulp_register_fetchcontent_source\s*\(\s*([A-Za-z0-9._-]+)([^)]*)\))");
    std::regex ref_re(R"(\bREF[\s\n\r]+([^\s\)\n\r]+))");

    auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), call_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string name = (*it)[1].str();
        std::string body = (*it)[2].str();
        std::smatch ref_m;
        if (!std::regex_search(body, ref_m, ref_re)) continue;
        std::string ref = ref_m[1].str();
        out[to_lower_ascii(name)] = ref;
    }
    return out;
}

DeclaredRefs parse_declared_refs_from_file(const fs::path& cmake_file) {
    if (!fs::exists(cmake_file)) return {};
    std::ifstream f(cmake_file);
    if (!f.is_open()) return {};
    std::stringstream buf;
    buf << f.rdbuf();
    return parse_declared_refs_from_text(buf.str());
}

DiscoveryEnv make_real_env(fs::path cache_root, DeclaredRefs refs) {
    DiscoveryEnv env;
    env.cache_root = std::move(cache_root);
    env.declared_refs = std::move(refs);

    env.lstat = [](const fs::path& p) -> std::optional<StatInfo> {
        std::error_code ec;
        auto status = fs::symlink_status(p, ec);
        if (ec || !fs::exists(status)) return std::nullopt;
        StatInfo info;
        info.exists = true;
        info.is_symlink = fs::is_symlink(status);
        info.is_directory = fs::is_directory(status);

        // Resolve the symlink target without following it further —
        // we only want the immediate link's pointer here.
        if (info.is_symlink) {
            std::error_code rec;
            info.symlink_target = fs::read_symlink(p, rec);
            if (rec) info.symlink_target.clear();
        }

#ifndef _WIN32
        struct ::stat st {};
        if (::lstat(p.c_str(), &st) == 0) {
            info.is_user_writable = (st.st_uid == ::geteuid());
        } else {
            info.is_user_writable = false;
        }
#else
        // Windows: cache lives under the user's profile; ownership
        // checks via `geteuid` don't apply. Treat all entries as
        // user-writable on Windows — root-owned classification is a
        // POSIX concept and the spec scopes it to macOS/Linux.
        info.is_user_writable = true;
#endif
        return info;
    };

    env.stat_follow = [](const fs::path& p) -> std::optional<StatInfo> {
        std::error_code ec;
        auto status = fs::status(p, ec);  // follows symlinks
        if (ec || !fs::exists(status)) return std::nullopt;
        StatInfo info;
        info.exists = true;
        info.is_symlink = false;  // we followed, so it's the target
        info.is_directory = fs::is_directory(status);
        info.is_user_writable = true;  // unused after follow
        return info;
    };

    env.list_dir = [](const fs::path& root) -> std::vector<fs::path> {
        std::vector<fs::path> out;
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return out;
        for (auto& entry : fs::directory_iterator(root, ec)) {
            out.push_back(entry.path());
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    return env;
}

std::vector<CacheEntry> discover_fetchcontent_cache(const DiscoveryEnv& env) {
    std::vector<CacheEntry> entries;
    if (env.cache_root.empty() || !env.list_dir) return entries;

    auto children = env.list_dir(env.cache_root);
    std::sort(children.begin(), children.end());

    for (const auto& path : children) {
        CacheEntry e;
        e.name = path.filename().string();
        e.path = path;

        auto split = split_entry_name(e.name, env.declared_refs);
        e.dep_name = split.first;
        e.cached_ref = split.second;

        if (auto it = env.declared_refs.find(e.dep_name);
            it != env.declared_refs.end()) {
            e.declared_ref = it->second;
        }

        auto info = env.lstat ? env.lstat(path) : std::nullopt;
        if (!info) {
            e.status = CacheStatus::Unknown;
            e.reason = "lstat failed";
            e.remediation = "manually inspect: ls -la " + path.string();
            entries.push_back(std::move(e));
            continue;
        }

        e.is_symlink = info->is_symlink;
        e.resolved_target = info->symlink_target;

        if (info->is_symlink) {
            // Follow the link: if the target is missing, this entry is
            // dangling and CMake configure will fail at the explicit
            // override check (see PulpFetchContent.cmake:53).
            auto target_info = env.stat_follow ? env.stat_follow(path) : std::nullopt;
            if (!target_info || !target_info->exists) {
                e.status = CacheStatus::Dangling;
                e.fixable = info->is_user_writable;
                e.reason = "symlink target missing";
                if (!e.resolved_target.empty()) {
                    e.reason += " (was -> " + e.resolved_target.string() + ")";
                }
                if (info->is_user_writable) {
                    e.remediation = "rm " + path.string()
                        + "  # then re-run cmake configure to refetch";
                } else {
                    e.status = CacheStatus::RootOwned;
                    e.fixable = false;
                    e.remediation = "sudo rm " + path.string()
                        + "  # entry is not user-writable";
                }
                entries.push_back(std::move(e));
                continue;
            }
        }

        // Non-symlink, or symlink with a live target. Check ownership
        // before looking at staleness — a root-owned entry is reportable
        // even when the contents look fine.
        if (!info->is_user_writable) {
            e.status = CacheStatus::RootOwned;
            e.fixable = false;
            e.reason = "not user-writable (likely root-owned)";
            e.remediation = "sudo rm -rf " + path.string()
                + "  # then re-run cmake configure";
            entries.push_back(std::move(e));
            continue;
        }

        // Stale-commit detection. The cache directory name baked the
        // declared REF in via `pulp_sanitize_cache_suffix`, so a fresh
        // checkout produces a fresh directory; mismatch means the
        // CMakeLists.txt advanced the pin but the user's old cache
        // entry is still present. CMake will silently use it (the
        // explicit override path exists), which leads to "why is my
        // CI building stale code?" mysteries. Surface it.
        //
        // Skip the scratch dirs FetchContent always materialises next
        // to a populated source dir: `<name>-src`, `<name>-build`, and
        // `<name>-subbuild`. Those aren't `pulp_register_fetchcontent_source`
        // entries; they're CMake's own working state, and clobbering
        // them defeats the configure-time cache that makes the shared
        // source root worthwhile in the first place.
        bool is_fetchcontent_scratch =
            e.cached_ref == "src" || e.cached_ref == "build"
            || e.cached_ref == "subbuild";
        if (!e.declared_ref.empty() && !e.cached_ref.empty()
            && !is_fetchcontent_scratch) {
            std::string sanitized = sanitize_ref(e.declared_ref);
            if (sanitized != e.cached_ref) {
                e.status = CacheStatus::StaleCommit;
                e.fixable = true;
                e.reason = "cached ref " + e.cached_ref
                         + " differs from declared "
                         + e.declared_ref + " (sanitized: " + sanitized + ")";
                e.remediation = "rm -rf " + path.string()
                    + "  # CMakeLists.txt advanced the pin";
                entries.push_back(std::move(e));
                continue;
            }
        }

        e.status = CacheStatus::Healthy;
        e.fixable = false;
        e.reason = "ok";
        entries.push_back(std::move(e));
    }
    return entries;
}

bool any_unhealthy(const std::vector<CacheEntry>& entries) {
    for (const auto& e : entries) {
        if (e.status != CacheStatus::Healthy) return true;
    }
    return false;
}

bool blocks_preflight(const std::vector<CacheEntry>& entries) {
    // Stale-ref directories don't actually break configure/build —
    // CMake's override path keys on the *current* sanitized ref, so
    // leftover `<dep>-<oldref>` dirs are simply ignored or refetched.
    // Only states that genuinely prevent a successful configure/build
    // should gate the preflight (see fetchcontent_cache.hpp for the
    // full rationale).
    for (const auto& e : entries) {
        switch (e.status) {
            case CacheStatus::Dangling:
            case CacheStatus::RootOwned:
            case CacheStatus::Unknown:
                return true;
            case CacheStatus::Healthy:
            case CacheStatus::StaleCommit:
                break;
        }
    }
    return false;
}

namespace {

const char* glyph_for(CacheStatus s) {
    switch (s) {
        case CacheStatus::Healthy:     return "[ok]";
        case CacheStatus::Dangling:
        case CacheStatus::StaleCommit:
        case CacheStatus::RootOwned:   return "[!!]";
        case CacheStatus::Unknown:     return "[??]";
    }
    return "[??]";
}

}  // namespace

int render_report(const std::vector<CacheEntry>& entries,
                  const fs::path& cache_root,
                  std::ostream& out) {
    out << "Pulp FetchContent cache: " << cache_root.string() << "\n";
    if (entries.empty()) {
        out << "  (no entries — cache is empty or root does not exist)\n";
        return 0;
    }
    out << "  " << entries.size() << " entr"
        << (entries.size() == 1 ? "y" : "ies") << "\n";
    bool any_bad = false;
    for (const auto& e : entries) {
        out << "  " << glyph_for(e.status) << "  " << e.name;
        if (e.status != CacheStatus::Healthy) {
            out << "\n        status: " << status_label(e.status)
                << "\n        reason: " << e.reason;
            if (!e.remediation.empty()) {
                out << "\n        fix:    " << e.remediation;
            }
            any_bad = true;
        }
        out << "\n";
    }
    if (any_bad) {
        out << "\nRun `pulp doctor --caches --fix` to heal the entries marked "
               "[!!] above.\nRoot-owned entries require sudo and are not "
               "auto-healed.\n";
        return 1;
    }
    return 0;
}

int render_preflight(const std::vector<CacheEntry>& entries,
                     const fs::path& cache_root,
                     std::ostream& out) {
    // Preflight only blocks on states that genuinely break configure
    // (dangling symlinks, root-owned dirs, unknown). Stale-ref dirs
    // are surfaced by `pulp doctor --caches` but never gate the build
    // — see blocks_preflight().
    if (!blocks_preflight(entries)) return 0;
    out << "pulp: FetchContent cache has unhealthy entries — would fail at "
           "configure time:\n";
    out << "  cache root: " << cache_root.string() << "\n";
    for (const auto& e : entries) {
        // Skip both Healthy and StaleCommit here: the preflight gate
        // is for blocking states only. Stale entries remain visible in
        // `pulp doctor --caches` and are cleaned by `--fix`.
        if (e.status == CacheStatus::Healthy
            || e.status == CacheStatus::StaleCommit) {
            continue;
        }
        out << "  " << glyph_for(e.status) << "  " << e.name
            << "  (" << status_label(e.status) << ")\n";
        if (!e.reason.empty()) {
            out << "        " << e.reason << "\n";
        }
        if (!e.remediation.empty()) {
            out << "        fix: " << e.remediation << "\n";
        }
    }
    out << "\nRun `pulp doctor --caches --fix` to auto-heal user-owned "
           "entries,\nor set PULP_SKIP_CACHE_PREFLIGHT=1 to bypass this "
           "check.\n";
    return 1;
}

int render_report_json(const std::vector<CacheEntry>& entries,
                       const fs::path& cache_root,
                       std::ostream& out) {
    out << "{\n";
    out << "  \"cache_root\": \"" << json_escape(cache_root.string()) << "\",\n";
    out << "  \"healthy\": " << (any_unhealthy(entries) ? "false" : "true") << ",\n";
    out << "  \"entries\": [";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (i > 0) out << ",";
        out << "\n    {\n";
        out << "      \"name\": \"" << json_escape(e.name) << "\",\n";
        out << "      \"path\": \"" << json_escape(e.path.string()) << "\",\n";
        out << "      \"status\": \"" << status_label(e.status) << "\",\n";
        out << "      \"is_symlink\": " << (e.is_symlink ? "true" : "false") << ",\n";
        out << "      \"resolved_target\": \""
            << json_escape(e.resolved_target.string()) << "\",\n";
        out << "      \"declared_ref\": \"" << json_escape(e.declared_ref) << "\",\n";
        out << "      \"cached_ref\": \"" << json_escape(e.cached_ref) << "\",\n";
        out << "      \"dep_name\": \"" << json_escape(e.dep_name) << "\",\n";
        out << "      \"reason\": \"" << json_escape(e.reason) << "\",\n";
        out << "      \"remediation\": \"" << json_escape(e.remediation) << "\",\n";
        out << "      \"fixable\": " << (e.fixable ? "true" : "false") << "\n";
        out << "    }";
    }
    if (!entries.empty()) out << "\n  ";
    out << "]\n";
    out << "}\n";
    return 0;
}

std::vector<FixResult> apply_fixes(const std::vector<CacheEntry>& entries,
                                   bool dry_run) {
    std::vector<FixResult> results;
    results.reserve(entries.size());
    for (const auto& e : entries) {
        FixResult r;
        r.path = e.path;
        if (!e.fixable || e.status == CacheStatus::Healthy
            || e.status == CacheStatus::RootOwned) {
            r.outcome = FixOutcome::Skipped;
            results.push_back(std::move(r));
            continue;
        }
        if (dry_run) {
            r.outcome = FixOutcome::DryRun;
            results.push_back(std::move(r));
            continue;
        }
        std::string err;
        if (remove_path_safely(e.path, err)) {
            r.outcome = FixOutcome::Removed;
        } else {
            r.outcome = FixOutcome::Failed;
            r.error = err;
        }
        results.push_back(std::move(r));
    }
    return results;
}

}  // namespace pulp::cli::fetchcontent_cache
