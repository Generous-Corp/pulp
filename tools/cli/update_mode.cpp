// update_mode.cpp — Release-discovery Slice 5 (#550 / parent #499).
//
// Implementation notes:
//   - Keep this translation unit free of `cli_common` link deps so the
//     unit tests compile it standalone (same pattern as update_check
//     and version_diag).
//   - All time enters via caller-provided epoch seconds. Never call
//     std::chrono::system_clock::now() from within this module — the
//     dispatch-path caller is responsible for obtaining a timestamp
//     once and threading it through.
//   - The JSON shapes are handcrafted with a narrow regex scan, same
//     strategy as update_check. Pulling a JSON dep for two small
//     single-line documents would bloat the CLI link.

#include "update_mode.hpp"

#include "update_check.hpp"   // parse_semver / is_newer / normalize helpers

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>

namespace pulp::cli::update_mode {

namespace {

namespace fs = std::filesystem;

std::string read_text(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

bool write_text_atomically(const fs::path& path, const std::string& payload) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << payload;
        if (!f.good()) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Cross-device fallback — ~/.pulp is normally on the same
        // device as its tmp, but HOME bind mounts in CI can split.
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
        return !ec;
    }
    return true;
}

std::string extract_json_str(const std::string& body, const std::string& key) {
    try {
        std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"\\\\]*(?:\\\\.[^\"\\\\]*)*)\"");
        std::smatch m;
        if (std::regex_search(body, m, re)) {
            std::string raw = m[1].str();
            std::string out;
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                char c = raw[i];
                if (c == '\\' && i + 1 < raw.size()) {
                    char esc = raw[++i];
                    switch (esc) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        default: out += esc; break;
                    }
                } else {
                    out += c;
                }
            }
            return out;
        }
    } catch (const std::regex_error&) {}
    return {};
}

std::int64_t extract_json_i64(const std::string& body, const std::string& key) {
    try {
        std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
        std::smatch m;
        if (std::regex_search(body, m, re)) {
            try { return std::stoll(m[1].str()); } catch (...) { return 0; }
        }
    } catch (const std::regex_error&) {}
    return 0;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

}  // namespace

// ── Mode ────────────────────────────────────────────────────────────────────

Mode parse_mode(const std::string& s) {
    if (s == "auto")   return Mode::Auto;
    if (s == "manual") return Mode::Manual;
    if (s == "off")    return Mode::Off;
    // Empty / typo / anything else → prompt (safest default).
    // Note: "prompt" is also the documented default (design Section A).
    return Mode::Prompt;
}

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Auto:   return "auto";
        case Mode::Prompt: return "prompt";
        case Mode::Manual: return "manual";
        case Mode::Off:    return "off";
    }
    return "prompt";
}

// ── Snooze ──────────────────────────────────────────────────────────────────

std::string serialize_snooze(std::int64_t expiry_epoch_sec) {
    // Single line — cheap to parse, cheap to eyeball.
    return std::to_string(expiry_epoch_sec) + "\n";
}

std::int64_t parse_snooze(const std::string& contents) {
    // Tolerant: allow surrounding whitespace and ignore anything after
    // the first non-digit. A malformed file is treated as "never
    // snoozed" rather than "snoozed forever" — failure-open because
    // accidentally blocking a legitimate update notice is worse than
    // accidentally showing one.
    std::string s;
    for (char c : contents) {
        if (c == '-' || (c >= '0' && c <= '9')) s += c;
        else if (!s.empty()) break;
    }
    if (s.empty()) return 0;
    try { return std::stoll(s); } catch (...) { return 0; }
}

bool is_snooze_active(const fs::path& snooze_path, std::int64_t now_epoch_sec) {
    std::error_code ec;
    if (!fs::exists(snooze_path, ec)) return false;
    auto expiry = parse_snooze(read_text(snooze_path));
    return expiry > now_epoch_sec;
}

bool write_snooze(const fs::path& snooze_path,
                  std::int64_t now_epoch_sec,
                  int hours) {
    if (hours <= 0) return false;
    auto expiry = now_epoch_sec + static_cast<std::int64_t>(hours) * 3600;
    return write_text_atomically(snooze_path, serialize_snooze(expiry));
}

bool clear_snooze(const fs::path& snooze_path) {
    std::error_code ec;
    fs::remove(snooze_path, ec);
    // ec is truthy both on "file not found" and on real errors. We
    // only want to report failure for real errors. std::filesystem
    // sets `ec = std::errc::no_such_file_or_directory` or similar when
    // the file doesn't exist, but `remove` returns false in that case
    // and clears ec — it's actually fs::remove's contract that missing
    // file is "not an error". So: success when ec is falsy.
    return !ec;
}

// ── Pending upgrade ─────────────────────────────────────────────────────────

std::string serialize_pending_upgrade(const PendingUpgrade& p) {
    std::ostringstream os;
    os << "{"
       << "\"version\":\""   << json_escape(p.version)  << "\","
       << "\"staged_at\":"    << p.staged_at_epoch_sec   << ","
       << "\"binary\":\""    << json_escape(p.staged_binary_path) << "\""
       << "}\n";
    return os.str();
}

std::optional<PendingUpgrade> parse_pending_upgrade(const std::string& json) {
    if (json.empty()) return std::nullopt;
    PendingUpgrade p;
    p.version = extract_json_str(json, "version");
    p.staged_at_epoch_sec = extract_json_i64(json, "staged_at");
    p.staged_binary_path = extract_json_str(json, "binary");
    // A marker without a version is junk — don't report it as pending.
    if (p.version.empty()) return std::nullopt;
    return p;
}

bool write_pending_upgrade(const fs::path& marker_path,
                           const PendingUpgrade& p) {
    return write_text_atomically(marker_path, serialize_pending_upgrade(p));
}

std::optional<PendingUpgrade> read_pending_upgrade(const fs::path& marker_path) {
    std::error_code ec;
    if (!fs::exists(marker_path, ec)) return std::nullopt;
    return parse_pending_upgrade(read_text(marker_path));
}

bool clear_pending_upgrade(const fs::path& marker_path) {
    std::error_code ec;
    fs::remove(marker_path, ec);
    return !ec;
}

// ── Tombstone ───────────────────────────────────────────────────────────────

fs::path tombstone_path_for(const fs::path& executable) {
    // Namespaced suffix so sibling tools recognize it. Chosen to match
    // the rustup / pip family pattern of "<binary>.<tool>.old".
    fs::path out = executable;
    out += ".pulp.old";
    return out;
}

bool cleanup_tombstone(const fs::path& executable) {
    auto tomb = tombstone_path_for(executable);
    std::error_code ec;
    if (!fs::exists(tomb, ec)) return true;
    fs::remove(tomb, ec);
    // On Windows the tombstone can be locked if a previous process
    // hasn't fully exited yet (rare — the new pulp.exe we're running
    // in is the replacement, and the old inode is gone). Report
    // failure only when the file still exists after remove.
    return !fs::exists(tomb, ec);
}

// ── Banner composition ─────────────────────────────────────────────────────

std::string compose_manual_notice(const std::string& installed_version,
                                  const std::string& latest_version) {
    // One-line shape — locked by the design doc Section A and
    // covered verbatim by a test case.
    std::ostringstream os;
    os << "Pulp v" << latest_version
       << " available (you have v" << installed_version
       << "). Run `pulp upgrade` when you're ready.";
    return os.str();
}

std::string compose_auto_staged_notice(const std::string& staged_version) {
    std::ostringstream os;
    os << "Pulp v" << staged_version
       << " downloaded. The upgrade will complete on your next `pulp` invocation.";
    return os.str();
}

std::string compose_auto_completed_notice(const std::string& new_version) {
    std::ostringstream os;
    os << "Pulp CLI upgraded to v" << new_version
       << ". Run `pulp upgrade --notes` to see what changed.";
    return os.str();
}

// ── Decision helpers ────────────────────────────────────────────────────────

PromptDecision decide_prompt_banner(Mode mode,
                                    const std::string& installed_version,
                                    const std::string& latest_version,
                                    bool banner_already_shown_this_cycle,
                                    bool snooze_active) {
    PromptDecision d;
    if (mode != Mode::Prompt) return d;
    if (latest_version.empty()) return d;
    if (!pulp::cli::update_check::is_newer(installed_version, latest_version)) return d;
    if (snooze_active) return d;
    if (banner_already_shown_this_cycle) {
        // Already notified for this version on a prior invocation —
        // the user hasn't re-engaged (no upgrade, no mode change).
        // Start a snooze so we stop nagging for 24h without requiring
        // the user to type anything. The Slice 2 code ALREADY set
        // banner_shown_for_version the first time; the snooze file is
        // the "soft decline" track on top of that.
        //
        // We still don't print a banner this invocation — that's the
        // whole point of "already shown". We just ask the caller to
        // write the snooze so the next invocation is quiet even if
        // a newer release arrives (the snooze carries us through the
        // re-banner event). Slice 2 tests still pin the "silent second
        // invocation" behavior.
        d.write_snooze = false;  // snooze writes are tied to user action,
                                 // not a passive no-op. See tests.
        return d;
    }
    d.show_banner = true;
    // Writing the snooze on a first-show would conflict with Slice 2's
    // banner_shown_for_version semantics (users on prompt mode who
    // upgrade within 24h still want the upgrade banner). We only
    // write the snooze when the user explicitly declines via the
    // `/upgrade` Claude Code skill or a future interactive prompt,
    // both of which call write_snooze() directly.
    d.write_snooze = false;
    return d;
}

bool should_stage_auto_download(Mode mode,
                                const std::string& installed_version,
                                const std::string& latest_version,
                                const std::optional<PendingUpgrade>& existing) {
    if (mode != Mode::Auto) return false;
    if (latest_version.empty()) return false;
    if (!pulp::cli::update_check::is_newer(installed_version, latest_version)) return false;
    if (existing && existing->version == latest_version) {
        // Already staged this exact version. Don't re-download.
        return false;
    }
    return true;
}

}  // namespace pulp::cli::update_mode
