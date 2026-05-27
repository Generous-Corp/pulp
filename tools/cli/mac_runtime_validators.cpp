// mac_runtime_validators.cpp — implementation
//
// See mac_runtime_validators.hpp for the rationale and dispatch table.

#include "mac_runtime_validators.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <system_error>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace pulp::cli::mac_runtime {

namespace {

// Truncate combined stderr/stdout to a manageable size for the report
// row. Keeps the validator output readable when a bundle blows up.
std::string trim_to(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    s.resize(max);
    s += "…";
    return s;
}

// Local shell-quote — implemented here (instead of pulling cli_common's
// version) so this TU links cleanly into the unit-test binary without
// dragging in the whole CLI dep chain (network, sdk cache, etc.).
// Single-quote wrap + `'\''` escape, matches the POSIX shell idiom.
std::string sh_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

std::string q(const fs::path& p) { return sh_quote(p.string()); }

// Look up an executable on $PATH without depending on cli_common.
// Returns the full path or empty on miss. Mirrors `which` semantics:
// PATHEXT not consulted (we never look for `.exe`s here — the
// validators are macOS only).
std::string default_find_executable(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) {
        std::error_code ec;
        if (fs::exists(name, ec)) return name;
        return {};
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::string path = path_env;
    std::string::size_type start = 0;
    while (start <= path.size()) {
        auto end = path.find(':', start);
        auto dir = path.substr(start, end == std::string::npos
                                          ? std::string::npos
                                          : end - start);
        if (!dir.empty()) {
            fs::path candidate = fs::path(dir) / name;
            std::error_code ec;
            if (fs::exists(candidate, ec)) return candidate.string();
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

// Run a shell command, capture combined stdout+stderr, return (exit, output).
std::pair<int, std::string> default_run_capture(const std::string& cmd) {
#if defined(_WIN32)
    (void)cmd;
    return {-1, "command capture unsupported on Windows"};
#else
    std::string out;
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return {-1, "popen failed"};
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int raw = pclose(p);
    if (raw == -1) return {-1, out};
    if (WIFEXITED(raw)) return {WEXITSTATUS(raw), out};
    return {raw, out};
#endif
}

// Build a ValidatorResult quickly.
ValidatorResult make_result(const std::string& target,
                            const std::string& tool,
                            const fs::path& bundle,
                            const std::string& status,
                            int exit_code,
                            std::string summary) {
    ValidatorResult r;
    r.target = target;
    r.tool = tool;
    r.bundle = bundle.string();
    r.status = status;
    r.exit_code = exit_code;
    r.summary = trim_to(std::move(summary), 800);
    return r;
}

}  // namespace

MacValidatorEnv make_default_env() {
    MacValidatorEnv env;
    env.find_executable = default_find_executable;
    env.run_capture = default_run_capture;
    env.path_exists = [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec);
    };
#ifdef __APPLE__
    env.is_apple_host = true;
#else
    env.is_apple_host = false;
#endif
    return env;
}

fs::path resolve_standalone_executable(const fs::path& bundle,
                                       const MacValidatorEnv& env) {
    if (bundle.empty()) return {};
    if (bundle.extension() != ".app") return {};
    auto macos_dir = bundle / "Contents" / "MacOS";
    if (!env.path_exists(macos_dir)) return {};
    // Use the bundle stem as the convention. Pulp's standalone
    // packager always names the binary after the bundle (see
    // PulpStandalone.cmake), so this is robust for our own artifacts
    // and degrades gracefully — the smoke check will skip if the
    // candidate path doesn't exist.
    auto candidate = macos_dir / bundle.stem();
    if (env.path_exists(candidate)) return candidate;
    return {};
}

ValidatorResult run_standalone_validator(const fs::path& bundle,
                                         const MacValidatorEnv& env) {
    if (!env.is_apple_host) {
        return make_result("standalone", "open", bundle, "skip", -1,
                           "macOS host required");
    }
    if (!env.path_exists(bundle)) {
        return make_result("standalone", "open", bundle, "fail", -1,
                           "bundle not found");
    }
    auto exe = resolve_standalone_executable(bundle, env);
    if (exe.empty()) {
        // Distinguish "this isn't a .app at all" (legitimately skip) from
        // "this IS a .app bundle but the runnable binary inside is
        // missing/broken" (fail). The latter is a packaging regression
        // that MUST surface as a non-zero exit (Codex PR #3005 P2).
        const bool looks_like_app_bundle = bundle.extension() == ".app";
        if (looks_like_app_bundle) {
            return make_result("standalone", "exec", bundle, "fail", -1,
                               "malformed .app bundle: no runnable binary "
                               "at Contents/MacOS/<stem>");
        }
        return make_result("standalone", "exec", bundle, "skip", -1,
                           "not a .app bundle");
    }
    // Execute the binary directly so we get the real exit code (open
    // --wait-apps would route through LaunchServices and lose detail).
    //
    // Env recipe (regression: Codex PR #3005 review):
    //   PULP_HEADLESS=1  → run_with_editor() takes the headless path
    //                      and returns false IF no screenshot target is
    //                      provided. We MUST give a screenshot path so
    //                      the standalone actually smoke-runs instead
    //                      of fail-fast'ing on missing PULP_SCREENSHOT.
    //   PULP_SCREENSHOT=<tmp> → wires the headless one-shot capture so
    //                      the binary exits cleanly after rendering one
    //                      frame to disk. The capture file itself is
    //                      treated as smoke artifact, not validated
    //                      pixel-by-pixel by this validator.
    //   PULP_DISABLE_PLUGIN_EDITOR=1 → ensures any embedded plugin host
    //                      mode skips the editor; standalone owns the UI.
    //
    // The previous PULP_HEADLESS=1 + PULP_TEST_MODE=1 without a
    // PULP_SCREENSHOT path made `standalone_config_from_environment`
    // flip headless on and then `run_with_editor` immediately returned
    // false, so EVERY healthy standalone bundle would have reported a
    // false failure (`pulp validate --target standalone`).
    std::string screenshot_path;
#if defined(_WIN32)
    screenshot_path = (fs::temp_directory_path() / "pulp-standalone-smoke.png").string();
#else
    char tmpl[] = "/tmp/pulp-standalone-smoke-XXXXXX.png";
    int fd = mkstemps(tmpl, 4);
    if (fd != -1) {
        screenshot_path = tmpl;
        ::close(fd);
    } else {
        // Fallback to a fixed path. If smoke runs concurrently this could
        // race, but a fixed path still beats an empty one (which would
        // trigger the very fail-fast regression we're avoiding).
        screenshot_path = "/tmp/pulp-standalone-smoke.png";
    }
#endif
    std::ostringstream cmd;
    cmd << "PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 "
        << "PULP_SCREENSHOT=" << sh_quote(screenshot_path) << " "
        << q(exe);
    auto [rc, out] = env.run_capture(cmd.str());
    // Best-effort cleanup; ignore failure.
    std::error_code ec;
    fs::remove(screenshot_path, ec);
    if (rc == 0) {
        return make_result("standalone", "exec", bundle, "pass", 0, "");
    }
    return make_result("standalone", "exec", bundle, "fail", rc, out);
}

AuComponentTuple parse_au_component_tuple(const std::string& plutil_output) {
    AuComponentTuple out;
    // `plutil -p` emits human-ish output like:
    //   "AudioComponents" => [
    //       0 => {
    //         "type" => "aufx"
    //         "subtype" => "Plpe"
    //         "manufacturer" => "Plpa"
    //       }
    //   ]
    // We hunt for the three keys lexically; this avoids pulling in a
    // plist parser. Whitespace and quoting are tolerant.
    auto find_value = [&](const std::string& key) -> std::string {
        auto pos = plutil_output.find("\"" + key + "\"");
        if (pos == std::string::npos) return {};
        pos = plutil_output.find("=>", pos);
        if (pos == std::string::npos) return {};
        pos = plutil_output.find('"', pos);
        if (pos == std::string::npos) return {};
        auto end = plutil_output.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return plutil_output.substr(pos + 1, end - pos - 1);
    };
    out.type = find_value("type");
    out.subtype = find_value("subtype");
    out.manufacturer = find_value("manufacturer");
    return out;
}

ValidatorResult run_auv3_validator(const fs::path& bundle,
                                   const MacValidatorEnv& env) {
    if (!env.is_apple_host) {
        return make_result("auv3", "auval", bundle, "skip", -1,
                           "macOS host required");
    }
    if (!env.path_exists(bundle)) {
        return make_result("auv3", "auval", bundle, "fail", -1,
                           "bundle not found");
    }
    // AUv3 bundles can be either container .app (with the appex
    // embedded under Contents/PlugIns) or the standalone .appex.
    // We locate the appex either way.
    fs::path appex = bundle;
    if (bundle.extension() == ".app") {
        auto plugins = bundle / "Contents" / "PlugIns";
        if (env.path_exists(plugins)) {
            std::error_code ec;
            for (auto& entry : fs::directory_iterator(plugins, ec)) {
                if (entry.path().extension() == ".appex") {
                    appex = entry.path();
                    break;
                }
            }
        }
    }
    if (appex.extension() != ".appex") {
        return make_result("auv3", "auval", bundle, "skip", -1,
                           "no .appex found inside container");
    }
    // Read AudioComponents from the appex Info.plist via `plutil`.
    auto plist = appex / "Contents" / "Info.plist";
    if (!env.path_exists(plist)) {
        return make_result("auv3", "auval", bundle, "fail", -1,
                           "Info.plist missing inside .appex");
    }
    auto plutil_path = env.find_executable("plutil");
    if (plutil_path.empty()) {
        return make_result("auv3", "auval", bundle, "skip", -1,
                           "plutil not on PATH");
    }
    auto [prc, plist_dump] = env.run_capture(plutil_path + " -p " + q(plist));
    if (prc != 0) {
        return make_result("auv3", "auval", bundle, "fail", prc,
                           "plutil -p failed: " + plist_dump);
    }
    auto tuple = parse_au_component_tuple(plist_dump);
    if (tuple.type.empty() || tuple.subtype.empty() ||
        tuple.manufacturer.empty()) {
        return make_result("auv3", "auval", bundle, "fail", -1,
                           "could not parse AudioComponents tuple");
    }
    // Register the appex with LaunchServices so pluginkit sees it,
    // then ask pluginkit to confirm it's discoverable.
    auto pluginkit = env.find_executable("pluginkit");
    if (!pluginkit.empty()) {
        // Best-effort registration; ignore failure (developer machines
        // typically already have the appex registered via Xcode).
        (void)env.run_capture(pluginkit + " -a " + q(appex));
    }
    auto auval = env.find_executable("auval");
    if (auval.empty()) {
        return make_result("auv3", "auval", bundle, "skip", -1,
                           "auval not on PATH (xcode-select --install)");
    }
    std::ostringstream cmd;
    cmd << auval << " -strict -v "
        << tuple.type << " " << tuple.subtype << " " << tuple.manufacturer;
    auto [rc, out] = env.run_capture(cmd.str());
    if (rc == 0) {
        return make_result("auv3", "auval", bundle, "pass", 0, "");
    }
    return make_result("auv3", "auval", bundle, "fail", rc, out);
}

std::vector<fs::path> enumerate_mach_o_payloads(const fs::path& bundle,
                                                const MacValidatorEnv& env) {
    std::vector<fs::path> out;
    if (!env.path_exists(bundle)) return out;
    auto consider = [&](const fs::path& p) {
        auto ext = p.extension().string();
        if (ext == ".dylib" || ext == ".so") { out.push_back(p); return; }
        // Mach-O executables typically have no extension. Heuristic:
        // anything under Contents/MacOS without an extension is fair
        // game; same for the binary inside a .framework version dir.
        if (ext.empty()) out.push_back(p);
    };
    std::error_code ec;
    // Walk the entire bundle recursively so we catch
    // Contents/Frameworks/*.framework/Versions/A/<binary> too.
    if (!fs::is_directory(bundle, ec)) {
        consider(bundle);
        return out;
    }
    for (auto it = fs::recursive_directory_iterator(
             bundle, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        consider(it->path());
    }
    // De-dup; some bundles symlink the current Version into the
    // framework root which would double-enumerate the binary.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string extract_min_macos_version(const std::string& otool_output) {
    // Look for either form:
    //   cmd LC_VERSION_MIN_MACOSX
    //   ...
    //   version 13.0
    // or:
    //   cmd LC_BUILD_VERSION
    //   ...
    //   minos 13.0
    // We pull the FIRST occurrence — multi-arch slices share the same
    // floor in Pulp's build, so this is enough for the AudioWorkgroup
    // floor check. The actual lipo-slice-walk lives in Phase 6.
    auto find_keyword = [&](const std::string& keyword) -> std::string {
        auto pos = otool_output.find(keyword);
        if (pos == std::string::npos) return {};
        pos += keyword.size();
        // Skip whitespace
        while (pos < otool_output.size() &&
               std::isspace(static_cast<unsigned char>(otool_output[pos]))) {
            ++pos;
        }
        // Read until whitespace
        std::string v;
        while (pos < otool_output.size() &&
               !std::isspace(static_cast<unsigned char>(otool_output[pos]))) {
            v += otool_output[pos++];
        }
        return v;
    };
    // Prefer LC_BUILD_VERSION because it's what modern Apple linkers
    // emit; fall back to LC_VERSION_MIN_MACOSX for older binaries.
    if (otool_output.find("LC_BUILD_VERSION") != std::string::npos) {
        auto v = find_keyword("minos");
        if (!v.empty()) return v;
    }
    if (otool_output.find("LC_VERSION_MIN_MACOSX") != std::string::npos) {
        auto v = find_keyword("version");
        if (!v.empty()) return v;
    }
    return {};
}

bool meets_macos_floor(const std::string& version) {
    // Permissive on missing data — the operator-facing flow prints a
    // warning when the version string is empty, so we don't want to
    // double-fail.
    if (version.empty()) return true;
    // Parse "X.Y[.Z]" and compare against 13.0.
    int major = 0, minor = 0;
    char dot = 0;
    std::istringstream iss(version);
    iss >> major >> dot >> minor;
    if (major > 13) return true;
    if (major < 13) return false;
    return minor >= 0;
}

ValidatorResult run_macho_validator(const fs::path& bundle,
                                    const MacValidatorEnv& env) {
    if (!env.is_apple_host) {
        return make_result("macho", "codesign", bundle, "skip", -1,
                           "macOS host required");
    }
    if (!env.path_exists(bundle)) {
        return make_result("macho", "codesign", bundle, "fail", -1,
                           "bundle not found");
    }
    auto payloads = enumerate_mach_o_payloads(bundle, env);
    if (payloads.empty()) {
        return make_result("macho", "codesign", bundle, "skip", -1,
                           "no Mach-O payloads inside bundle");
    }
    auto codesign = env.find_executable("codesign");
    auto otool = env.find_executable("otool");
    std::vector<std::string> failures;
    int floor_warnings = 0;
    // Step 1: bundle-level codesign --verify --deep. This is what
    // Gatekeeper actually runs at install time; doing it once on the
    // top-level bundle catches the common "missing CodeResources"
    // breakage.
    if (!codesign.empty()) {
        auto [rc, out] = env.run_capture(
            codesign + " --verify --deep --strict " + q(bundle));
        if (rc != 0) {
            failures.push_back("codesign: " + out);
        }
    } else {
        failures.push_back("codesign not on PATH");
    }
    // Step 2: per-payload LC_VERSION_MIN / LC_BUILD_VERSION floor.
    if (!otool.empty()) {
        for (const auto& p : payloads) {
            auto [rc, out] = env.run_capture(otool + " -l " + q(p));
            if (rc != 0) continue;  // not all files are Mach-O
            auto v = extract_min_macos_version(out);
            if (v.empty()) {
                ++floor_warnings;
                continue;
            }
            if (!meets_macos_floor(v)) {
                failures.push_back("macOS floor: " + p.filename().string()
                                    + " declares " + v + " (need 13.0)");
            }
        }
    } else {
        failures.push_back("otool not on PATH");
    }
    if (failures.empty()) {
        std::string summary;
        if (floor_warnings > 0) {
            summary = std::to_string(floor_warnings) +
                      " payload(s) without a version load command";
        }
        return make_result("macho", "codesign+otool", bundle, "pass", 0,
                           summary);
    }
    std::string joined;
    for (const auto& f : failures) {
        if (!joined.empty()) joined += "; ";
        joined += f;
    }
    return make_result("macho", "codesign+otool", bundle, "fail", 1, joined);
}

std::vector<ValidatorResult> run_all_targets(const fs::path& bundle,
                                             const MacValidatorEnv& env) {
    return {
        run_standalone_validator(bundle, env),
        run_auv3_validator(bundle, env),
        run_macho_validator(bundle, env),
    };
}

std::vector<std::string> expand_target_name(const std::string& name) {
    if (name == "standalone") return {"standalone"};
    if (name == "auv3") return {"auv3"};
    if (name == "macho") return {"macho"};
    if (name == "all") return {"standalone", "auv3", "macho"};
    return {};
}

}  // namespace pulp::cli::mac_runtime
