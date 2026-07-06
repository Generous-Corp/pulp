#pragma once

/// @file reload_controller.hpp
/// Watch a logic library on disk and hot-reload it when it changes (the
/// standalone dev loop).
///
/// The shell drives poll() on a control-thread timer (the standalone app ticks
/// it from its UI/event loop). When the watched logic library's modification
/// time changes — i.e. the developer recompiled it — the controller runs a
/// ReloadSession reload, which gates and (on success) swaps the new DSP into the
/// live slot. Polling mtime is portable and dependency-free; a half-written file
/// is harmless because the transaction's load/fingerprint/contract gates reject
/// it (no swap), and the next completed write (new mtime) is retried.
///
/// Each distinct file version is attempted exactly once: the controller records
/// the mtime it acted on whether the reload succeeded or failed, so a candidate
/// that fails its gates is not retried on every tick — only the next edit (a new
/// mtime) triggers another attempt. The first poll() establishes the baseline
/// without reloading (the shell already loaded the initial logic).
///
/// CRITICAL — load each version from a UNIQUE path, for TWO independent reasons
/// (do not "simplify" the staging away on the strength of only one):
///   1. dlopen/LoadLibrary cache by path, so re-opening the watched path after
///      the developer overwrites it returns the STALE in-memory image, not the
///      new bytes.
///   2. A live mapping must never sit on the watched path: a rebuild overwrites
///      that file, often IN PLACE (truncate + rewrite of the same inode). On
///      Linux an in-place overwrite of a still-mapped image bleeds the new bytes
///      into the live mapping and corrupts the Processor running from it (its
///      code/vtable) — a crash that surfaces when that processor is the
///      fading-out side of the next crossfade. (macOS tolerates the overwrite,
///      so this only bites on Linux.) ReloadableShell::load_initial stages the
///      INITIAL image for the same reason.
/// The controller therefore stages each detected version to a fresh,
/// uniquely-named copy in `stage_dir` and reloads THAT, guaranteeing the loader
/// reads the new file AND that the watched path never carries a live mapping.
/// `stage_dir` must be a dlopen-safe location (NOT a world-writable temp dir on
/// macOS, where dyld kills a process that loads an unsigned dylib from /tmp);
/// it defaults to the watched file's own directory.

#include <pulp/format/reload/reload_transaction.hpp>
#include <pulp/format/reload/reload_trust_policy.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace pulp::format::reload {

// True if `pid` names a live process. On POSIX, kill(pid,0) succeeds (0) or
// fails with EPERM when the process exists but we lack permission; ESRCH means
// it's gone. On Windows we have no cheap portable check, so we report "not
// alive" and rely on the fact that a loaded module's file is locked — remove()
// then fails harmlessly, so we never delete a live instance's mapped image.
inline bool staged_pid_is_alive(long pid) {
#if defined(_WIN32)
    (void)pid;
    return false;
#else
    if (::kill(static_cast<::pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

// Reap stale staged copies (`<stem>.initial.<pid>.*` / `<stem>.reload.<pid>.*`)
// left by DEAD processes — dev-loop litter that accumulates because each run
// stages under a unique pid+counter name (ReloadableShell::stage_initial and
// ReloadController::stage_and_reload). Best-effort and startup-only. Files whose
// embedded PID is still a live process are left untouched, so a concurrently
// running instance's staged images are never removed.
inline void reap_stale_staged(const std::filesystem::path& dir,
                              const std::string& stem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    const std::string prefixes[2] = {stem + ".initial.", stem + ".reload."};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string fn = it->path().filename().string();
        std::size_t pos = std::string::npos;
        for (const auto& p : prefixes)
            if (fn.rfind(p, 0) == 0) { pos = p.size(); break; }
        if (pos == std::string::npos) continue;
        const std::size_t dot = fn.find('.', pos);          // pid runs up to the next '.'
        if (dot == std::string::npos || dot == pos) continue;
        long pid = 0;
        const std::string pid_str = fn.substr(pos, dot - pos);
        try { pid = std::stol(pid_str); } catch (...) { continue; }
        if (pid <= 0 || staged_pid_is_alive(pid)) continue;  // keep live-owned files
        std::error_code rm;
        fs::remove(it->path(), rm);                          // best-effort
    }
}

class ReloadController {
public:
    /// @param stage_dir where versioned copies are staged before loading; must
    /// be dlopen-safe. Defaults to the watched file's directory.
    ReloadController(ReloadSession& session, std::filesystem::path logic_path,
                     std::filesystem::path stage_dir = {},
                     ReloadTrustPolicy trust_policy = {})
        : session_(session), path_(std::move(logic_path)),
          stage_dir_(stage_dir.empty() ? path_.parent_path() : std::move(stage_dir)),
          trust_policy_(std::move(trust_policy)) {
        // Startup housekeeping: clear dead-process staged litter so a long dev
        // history doesn't leave hundreds of <stem>.initial.*/<stem>.reload.*
        // copies behind (bounds staged-copy accumulation across restarts).
        reap_stale_staged(stage_dir_, path_.stem().string());
    }

    /// Poll once. Returns the reload outcome if the file changed since the last
    /// poll (and exists), otherwise std::nullopt (no change / missing file /
    /// baseline-establishing first call).
    std::optional<ReloadOutcome> poll() {
        const auto mtime = current_mtime();
        if (!mtime) return std::nullopt;          // missing / unreadable — nothing to do
        if (!baseline_set_) {                     // first sight: record, don't reload
            last_mtime_ = *mtime;
            if (const auto h = hash_file(path_)) { last_hash_ = *h; hash_valid_ = true; }
            baseline_set_ = true;
            return std::nullopt;
        }
        if (*mtime == last_mtime_) return std::nullopt;  // unchanged mtime
        last_mtime_ = *mtime;                     // act once per distinct mtime

        // Content-hash gate: an mtime bump does not prove the BYTES
        // changed. Distinguish the three non-reload cases from a real edit:
        //   - empty / unreadable right now (a rebuild mid-write) → skip; the next
        //     completed write (new mtime) is retried.
        //   - identical content (a `touch`, or a rebuild that produced byte-
        //     identical output) → skip; no redundant stage + dlopen + gate churn.
        //   - genuinely new bytes → record the hash and reload.
        const auto hash = hash_file(path_);
        if (!hash) return std::nullopt;           // empty / unreadable → not a real version yet
        if (hash_valid_ && *hash == last_hash_) return std::nullopt;  // unchanged content
        last_hash_ = *hash;
        hash_valid_ = true;
        return stage_and_reload();
    }

    /// Force a reload regardless of mtime (e.g. a manual "reload now" command),
    /// and resync the baseline so the next poll() doesn't double-fire.
    ReloadOutcome reload_now() {
        if (const auto mtime = current_mtime()) {
            last_mtime_ = *mtime;
            baseline_set_ = true;
        }
        if (const auto hash = hash_file(path_)) {   // resync the content baseline too
            last_hash_ = *hash;
            hash_valid_ = true;
        }
        return stage_and_reload();
    }

    const std::filesystem::path& path() const { return path_; }
    std::uint64_t reload_attempts() const { return attempts_; }

private:
    std::optional<std::filesystem::file_time_type> current_mtime() const {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path_, ec);
        if (ec) return std::nullopt;
        return t;
    }

    // FNV-1a 64-bit over the watched file's bytes — a content fingerprint for
    // change detection only (not security; the fail-closed ABI/fingerprint/
    // contract gates are the trust boundary). Returns nullopt for an unreadable
    // OR empty file (an empty logic image is never a real version — usually a
    // rebuild mid-write), so the caller skips rather than acting on it.
    static std::optional<std::uint64_t> hash_file(const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return std::nullopt;
        std::uint64_t h = 1469598103934665603ULL;   // FNV offset basis
        std::array<char, 64 * 1024> buf{};
        std::size_t total = 0;
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = f.gcount();
            for (std::streamsize i = 0; i < got; ++i) {
                h ^= static_cast<unsigned char>(buf[static_cast<std::size_t>(i)]);
                h *= 1099511628211ULL;                // FNV prime
            }
            total += static_cast<std::size_t>(got);
        }
        if (f.bad() || total == 0) return std::nullopt;  // unreadable or empty
        return h;
    }

    // Copy the watched file to a fresh, uniquely-named path and reload THAT, so
    // the loader reads the new bytes rather than returning a cached image for a
    // path it already opened.
    ReloadOutcome stage_and_reload() {
        ++attempts_;
        // require_signed enforcement (opt-in; default OFF = the frictionless dev
        // loop). Verify the WATCHED file's signed pack BEFORE staging: the pack
        // manifest pins the watched path as its member, but we load a renamed
        // stage copy, so membership is checked here on the source and the faithful
        // copy_file below carries those exact verified bytes into the loaded image.
        // Fail closed — a refusal returns without staging or loading anything.
        if (trust_policy_.require_signed) {
            auto decision = resolve_reload_trust(path_, trust_policy_);
            if (auto* refused = std::get_if<ReloadOutcome>(&decision)) return *refused;
            if (auto* trust = std::get_if<SwapPackTrust>(&decision)) {
                if (auto rejected = verify_pack_before_load(*trust, path_.string()))
                    return *rejected;
            }
        }
        // Stage to a name unique per-INSTANCE and per-process. Two shells
        // watching the same logic_path share a default stage_dir (the watched
        // file's directory), so a name keyed only on a per-controller counter
        // (both start at 1) collides: the second instance's copy_file races the
        // first's dlopen of the same path, and on Linux an overwrite-while-mapped
        // corrupts the other instance's live image (see the header note + the
        // identical hazard ReloadableShell::stage_initial guards). PID
        // disambiguates processes; the process-wide atomic counter disambiguates
        // instances and successive reloads within a process.
        static std::atomic<std::uint64_t> global_counter{0};
        const long pid =
#if defined(_WIN32)
            static_cast<long>(::_getpid());
#else
            static_cast<long>(::getpid());
#endif
        const auto staged =
            stage_dir_ /
            (path_.stem().string() + ".reload." + std::to_string(pid) + "." +
             std::to_string(global_counter.fetch_add(1, std::memory_order_relaxed)) +
             path_.extension().string());
        std::error_code ec;
        std::filesystem::copy_file(
            path_, staged, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return {ReloadOutcome::Status::RejectedLoadFailed,
                    "stage copy failed: " + ec.message()};
        }
        // Best-effort reap THIS instance's previous staged copy so a long dev
        // session doesn't accumulate one file per reload. Safe: on POSIX the
        // prior image was dlopen'd (LeakPolicy::Retain) and its mapping survives
        // unlink, so the fading-out processor keeps running; on Windows a loaded
        // module's file is locked, so remove() fails and is ignored. Only reaps
        // our own files — never another instance's, and never the just-staged
        // one. (Cross-restart orphans from dead PIDs are a separate startup-reap
        // follow-up under the staged-file bookkeeping.)
        if (!last_staged_.empty() && last_staged_ != staged) {
            std::error_code rm_ec;
            std::filesystem::remove(last_staged_, rm_ec);
        }
        last_staged_ = staged;
        return session_.reload(staged.string());
    }

    ReloadSession& session_;
    std::filesystem::path path_;
    std::filesystem::path stage_dir_;
    ReloadTrustPolicy trust_policy_;  // opt-in require_signed enforcement (default OFF)
    std::filesystem::file_time_type last_mtime_{};
    std::filesystem::path last_staged_;  // this instance's prior staged copy (reaped on next stage)
    std::uint64_t last_hash_ = 0;        // content hash last acted on
    bool hash_valid_ = false;            // last_hash_ has been established
    bool baseline_set_ = false;
    std::uint64_t attempts_ = 0;
};

} // namespace pulp::format::reload
