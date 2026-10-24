// SPDX-License-Identifier: MIT
//
// Crash-isolated plugin scanner.
//
// Spawns `pulp-scan-worker` per bundle via `pulp::platform::ChildProcess`
// and classifies the worker's outcome into a `ScanStatus`. The worker's
// stdout is one flat JSON object per line, one per descriptor (see
// tools/scan-worker/scan_worker_main.cpp); each line is parsed as JSON so a
// descriptor carries the same PluginInfo fields an in-process scan fills.

#include <pulp/host/isolated_scanner.hpp>

#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pulp::host {

namespace {

// Strip surrounding whitespace.
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

PluginFormat parse_format(std::string_view s) {
    if (s == "clap") return PluginFormat::CLAP;
    if (s == "vst3") return PluginFormat::VST3;
    if (s == "au")   return PluginFormat::AudioUnit;
    if (s == "auv3") return PluginFormat::AudioUnitV3;
    if (s == "lv2")  return PluginFormat::LV2;
    return PluginFormat::VST3;  // arbitrary default; caller checks status==Ok first
}

std::string member_string(const choc::value::ValueView& object, std::string_view key) {
    if (!object.hasObjectMember(key))
        return {};
    const auto member = object[key];
    return member.isString() ? std::string(member.getString()) : std::string{};
}

bool member_bool(const choc::value::ValueView& object, std::string_view key, bool fallback) {
    if (!object.hasObjectMember(key))
        return fallback;
    const auto member = object[key];
    return member.isBool() ? member.getBool() : fallback;
}

int member_int(const choc::value::ValueView& object, std::string_view key, int fallback) {
    if (!object.hasObjectMember(key))
        return fallback;
    const auto member = object[key];
    if (member.isInt32() || member.isInt64())
        return static_cast<int>(member.getInt64());
    return fallback;
}

// One worker line is one flat JSON object. A line that is not an object, or
// that names neither a plugin nor a path, is a contract violation.
std::optional<PluginInfo> parse_descriptor_line(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() != '{') return std::nullopt;

    // choc::json::parse walks a NUL-terminated buffer, not the view's
    // length, so the line is copied: the parser then stops at this line's
    // end instead of reading on into the next one.
    const std::string owned_line(line);
    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(owned_line);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!parsed.isObject())
        return std::nullopt;
    const choc::value::ValueView object = parsed.getView();

    PluginInfo info;
    info.name = member_string(object, "name");
    info.manufacturer = member_string(object, "manufacturer");
    info.version = member_string(object, "version");
    info.unique_id = member_string(object, "unique_id");
    info.path = member_string(object, "path");
    info.format = parse_format(member_string(object, "format"));
    info.is_instrument = member_bool(object, "is_instrument", false);
    info.is_effect = member_bool(object, "is_effect", true);
    info.num_inputs = member_int(object, "num_inputs", info.num_inputs);
    info.num_outputs = member_int(object, "num_outputs", info.num_outputs);
    info.category = member_string(object, "category");
    info.description = member_string(object, "description");
    info.has_editor = member_bool(object, "has_editor", false);
    info.supports_sidechain = member_bool(object, "supports_sidechain", false);
    info.supports_midi_in = member_bool(object, "supports_midi_in", false);
    info.supports_midi_out = member_bool(object, "supports_midi_out", false);
    if (object.hasObjectMember("features")) {
        const auto features = object["features"];
        if (features.isArray()) {
            for (uint32_t i = 0; i < features.size(); ++i) {
                const auto feature = features[i];
                if (feature.isString())
                    info.features.emplace_back(feature.getString());
            }
        }
    }

    // A well-formed worker descriptor always has at least a name+path.
    if (info.name.empty() && info.path.empty()) return std::nullopt;
    return info;
}

std::filesystem::path current_executable_directory() {
#if defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0)
        return {};
    std::error_code ec;
    const auto canonical = fs::canonical(buffer, ec);
    return (ec ? fs::path(buffer) : canonical).parent_path();
#elif defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return fs::path(buffer).parent_path();
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0)
        return {};
    buffer[length] = '\0';
    return fs::path(buffer).parent_path();
#endif
}

bool is_existing_regular_file(const fs::path& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec);
}

// A worker runs in every scan with the scanning user's privileges, so a copy
// that another account could rewrite is refused, not merely warned about.
bool worker_candidate_qualifies(const fs::path& path) {
    if (!is_existing_regular_file(path))
        return false;
#if !defined(_WIN32)
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        runtime::log_warn("scan worker '{}' is group- or world-writable; refusing to run it",
                          path.string());
        return false;
    }
#endif
    return true;
}

// Tail of stderr, capped so the error_message stays small (the parent
// often serializes it into ScanBlacklist's reason field).
std::string short_stderr(const std::string& s) {
    constexpr std::size_t kCap = 256;
    if (s.size() <= kCap) return s;
    std::string out = "...";
    out.append(s, s.size() - kCap, kCap);
    return out;
}

}  // namespace

std::string find_scan_worker(std::string_view explicit_path) {
    if (!explicit_path.empty()) {
        const fs::path candidate{std::string(explicit_path)};
        return worker_candidate_qualifies(candidate) ? candidate.string() : std::string{};
    }
    if (const auto from_env = runtime::get_env("PULP_SCAN_WORKER");
        from_env && !from_env->empty()) {
        const fs::path candidate{*from_env};
        return worker_candidate_qualifies(candidate) ? candidate.string() : std::string{};
    }
#if defined(_WIN32)
    const char* worker_name = "pulp-scan-worker.exe";
#else
    const char* worker_name = "pulp-scan-worker";
#endif
    auto dir = current_executable_directory();
    for (int level = 0; level < 5 && !dir.empty(); ++level) {
        const std::array<fs::path, 3> candidates{
            dir / worker_name,
            dir / "bin" / worker_name,
            dir / "tools" / "scan-worker" / worker_name,
        };
        for (const auto& candidate : candidates) {
            if (worker_candidate_qualifies(candidate))
                return candidate.string();
        }
        // An application bundle is the outermost directory an app may trust;
        // its parent is /Applications or a user folder full of other apps. A
        // directory holding bin/ is an install prefix, and the prefix above it
        // belongs to someone else.
        std::error_code ec;
        if (dir.extension() == ".app" || fs::is_directory(dir / "bin", ec))
            break;
        const auto parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
    return {};
}

ScanResult IsolatedPluginScanner::scan(const std::string& bundle_path,
                                       int timeout_ms) const {
    ScanResult result;

    // Operational preflight — the worker has to exist and be a regular
    // file. We don't try to chmod/exec-test; ChildProcess will surface
    // exec failures via exit_code = -1 with empty stdout if we get past
    // this guard.
    std::error_code ec;
    if (worker_path_.empty() || !fs::exists(worker_path_, ec) ||
        !fs::is_regular_file(worker_path_, ec)) {
        result.status = ScanStatus::WorkerMissing;
        result.error_message = "pulp-scan-worker not found at: " + worker_path_;
        return result;
    }

    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = timeout_ms;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    // Cap the captured output. The default 1 MB is plenty; a worker
    // that floods stdout is itself a sign of corruption and we'd
    // rather classify it as Crash than allocate forever.
    opts.max_output_bytes = 64 * 1024;

    auto proc = pulp::platform::ChildProcess::run(
        worker_path_, {bundle_path}, opts);

    result.exit_code = proc.exit_code;

    if (proc.timed_out) {
        result.status = ScanStatus::Timeout;
        result.error_message = "scan timed out after " +
                               std::to_string(timeout_ms) + " ms: " +
                               short_stderr(proc.stderr_output);
        return result;
    }

    // The capture cap is far above any real descriptor set. Exceeding it means
    // the worker's protocol channel is flooded, which the worker owns; the
    // bundle gets no verdict and must not be blacklisted for it.
    if (proc.output_limit_exceeded) {
        result.status = ScanStatus::ProtocolError;
        result.error_message = "worker output exceeded the capture limit for " + bundle_path;
        return result;
    }

    // exit_code 2 = usage error from worker (we always pass argv[1], so
    // this would mean the worker is older than expected); exit_code 3 =
    // unsupported bundle extension. Both are non-crash conditions.
    if (proc.exit_code == 3) {
        result.status = ScanStatus::FormatError;
        result.error_message = short_stderr(proc.stderr_output);
        return result;
    }
    if (proc.exit_code == 2) {
        result.status = ScanStatus::FormatError;  // operationally same — caller can't recover
        result.error_message = "scan-worker usage error: " +
                               short_stderr(proc.stderr_output);
        return result;
    }

    // Anything else non-zero (signal kill -> -1 on POSIX, or an OS
    // exception code on Windows) is a crash. The worker only ever
    // exits 0 / 2 / 3 deliberately.
    if (proc.exit_code != 0) {
        result.status = ScanStatus::Crash;
        result.error_message = "worker exited abnormally (code " +
                               std::to_string(proc.exit_code) + "): " +
                               short_stderr(proc.stderr_output);
        return result;
    }

    // exit_code 0 and stdout is empty → the worker ran cleanly but the
    // bundle produced no descriptor (e.g., the format scanner couldn't
    // pull anything sensible out of it). Treat as NotPlugin.
    std::string_view out{proc.stdout_output};
    out = trim(out);
    if (out.empty()) {
        result.status = ScanStatus::NotPlugin;
        result.error_message = "worker emitted no descriptor for " + bundle_path;
        return result;
    }

    // The worker emits one JSON object per line, one per descriptor, on a
    // channel the plugin cannot reach. A line that does not parse is a
    // worker-level contract violation: nothing from the bundle is trusted,
    // and nothing is held against the bundle either.
    while (!out.empty()) {
        const auto nl = out.find('\n');
        const std::string_view line = (nl == std::string_view::npos) ? out : out.substr(0, nl);
        out = (nl == std::string_view::npos) ? std::string_view{} : out.substr(nl + 1);
        if (trim(line).empty())
            continue;
        auto parsed = parse_descriptor_line(line);
        if (!parsed) {
            result.status = ScanStatus::ProtocolError;
            result.descriptors.clear();
            result.error_message =
                "worker stdout not parseable: " + short_stderr(proc.stdout_output);
            return result;
        }
        result.descriptors.push_back(std::move(*parsed));
    }
    result.status = ScanStatus::Ok;
    result.descriptor = result.descriptors.front();
    return result;
}

}  // namespace pulp::host
