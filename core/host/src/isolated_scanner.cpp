// SPDX-License-Identifier: MIT
//
// Crash-isolated plugin scanner — macOS plan item 4.1.
//
// Spawns `pulp-scan-worker` per bundle via `pulp::platform::ChildProcess`
// and classifies the worker's outcome into a `ScanStatus`. The worker's
// stdout is the JSON line(s) it emits per descriptor (see
// tools/scan-worker/scan_worker_main.cpp); we keep the parser deliberately
// small — flat, double-quoted-only, no escapes-with-unicode reassembly —
// because the worker is the only producer and we control its format.

#include <pulp/host/isolated_scanner.hpp>

#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

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

// Unescape the small set of escapes the worker emits (json_escape in
// scan_worker_main.cpp). Anything else is passed through unchanged —
// good enough for worker-controlled output.
std::string json_unescape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char n = raw[i + 1];
            switch (n) {
                case '"':  out += '"';  ++i; continue;
                case '\\': out += '\\'; ++i; continue;
                case 'b':  out += '\b'; ++i; continue;
                case 'f':  out += '\f'; ++i; continue;
                case 'n':  out += '\n'; ++i; continue;
                case 'r':  out += '\r'; ++i; continue;
                case 't':  out += '\t'; ++i; continue;
                default: break;
            }
        }
        out += c;
    }
    return out;
}

// Locate `"key":"value"` and return value. Returns std::nullopt when the
// key isn't present in `line`. Deliberately string-search rather than a
// real JSON parser — keeps this TU tiny and the worker output schema is
// frozen by us.
std::optional<std::string> find_string(std::string_view line, std::string_view key) {
    std::string needle = "\"";
    needle += std::string(key);
    needle += "\":\"";
    auto k = line.find(needle);
    if (k == std::string_view::npos) return std::nullopt;
    auto value_start = k + needle.size();
    // Find the closing quote, honouring a single level of backslash escape.
    auto i = value_start;
    while (i < line.size()) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            i += 2;
            continue;
        }
        if (line[i] == '"') break;
        ++i;
    }
    if (i >= line.size()) return std::nullopt;
    return json_unescape(line.substr(value_start, i - value_start));
}

// Locate `"key":true|false` and return the bool. Defaults to `dflt`.
bool find_bool(std::string_view line, std::string_view key, bool dflt) {
    std::string needle = "\"";
    needle += std::string(key);
    needle += "\":";
    auto k = line.find(needle);
    if (k == std::string_view::npos) return dflt;
    auto v = k + needle.size();
    if (v + 4 <= line.size() && line.compare(v, 4, "true") == 0) return true;
    if (v + 5 <= line.size() && line.compare(v, 5, "false") == 0) return false;
    return dflt;
}

PluginFormat parse_format(std::string_view s) {
    if (s == "clap") return PluginFormat::CLAP;
    if (s == "vst3") return PluginFormat::VST3;
    if (s == "au")   return PluginFormat::AudioUnit;
    if (s == "auv3") return PluginFormat::AudioUnitV3;
    if (s == "lv2")  return PluginFormat::LV2;
    return PluginFormat::VST3;  // arbitrary default; caller checks status==Ok first
}

std::optional<PluginInfo> parse_descriptor_line(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() != '{') return std::nullopt;

    PluginInfo info;
    info.name = find_string(line, "name").value_or("");
    info.manufacturer = find_string(line, "manufacturer").value_or("");
    info.version = find_string(line, "version").value_or("");
    info.unique_id = find_string(line, "unique_id").value_or("");
    info.path = find_string(line, "path").value_or("");
    info.is_instrument = find_bool(line, "is_instrument", false);
    info.is_effect = find_bool(line, "is_effect", true);
    info.format = parse_format(find_string(line, "format").value_or(""));

    // A well-formed worker descriptor always has at least a name+path.
    if (info.name.empty() && info.path.empty()) return std::nullopt;
    return info;
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

    // Parse the first line. The worker emits one JSON object per line;
    // we report the first descriptor and ignore the rest (multi-class
    // CLAPs are rare and the bundle-level identity is what matters for
    // the host-side blacklist + cache).
    auto nl = out.find('\n');
    std::string_view first = (nl == std::string_view::npos) ? out : out.substr(0, nl);
    auto parsed = parse_descriptor_line(first);
    if (!parsed) {
        // exit 0 but unparseable output — treat as crash so the bundle
        // is blacklisted; a malformed descriptor is a worker-level
        // contract violation we shouldn't trust.
        result.status = ScanStatus::Crash;
        result.error_message = "worker stdout not parseable: " +
                               short_stderr(proc.stdout_output);
        return result;
    }
    result.status = ScanStatus::Ok;
    result.descriptor = std::move(parsed);
    return result;
}

}  // namespace pulp::host
