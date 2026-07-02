#pragma once

/// @file reload_controller.hpp
/// Watch a logic library on disk and hot-reload it when it changes (v2 plan
/// §4.4 / Phase 1, the standalone dev loop).
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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pulp::format::reload {

class ReloadController {
public:
    /// @param stage_dir where versioned copies are staged before loading; must
    /// be dlopen-safe. Defaults to the watched file's directory.
    ReloadController(ReloadSession& session, std::filesystem::path logic_path,
                     std::filesystem::path stage_dir = {})
        : session_(session), path_(std::move(logic_path)),
          stage_dir_(stage_dir.empty() ? path_.parent_path() : std::move(stage_dir)) {}

    /// Poll once. Returns the reload outcome if the file changed since the last
    /// poll (and exists), otherwise std::nullopt (no change / missing file /
    /// baseline-establishing first call).
    std::optional<ReloadOutcome> poll() {
        const auto mtime = current_mtime();
        if (!mtime) return std::nullopt;          // missing / unreadable — nothing to do
        if (!baseline_set_) {                     // first sight: record, don't reload
            last_mtime_ = *mtime;
            baseline_set_ = true;
            return std::nullopt;
        }
        if (*mtime == last_mtime_) return std::nullopt;  // unchanged
        last_mtime_ = *mtime;                     // act once per distinct version
        return stage_and_reload();
    }

    /// Force a reload regardless of mtime (e.g. a manual "reload now" command),
    /// and resync the baseline so the next poll() doesn't double-fire.
    ReloadOutcome reload_now() {
        if (const auto mtime = current_mtime()) {
            last_mtime_ = *mtime;
            baseline_set_ = true;
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

    // Copy the watched file to a fresh, uniquely-named path and reload THAT, so
    // the loader reads the new bytes rather than returning a cached image for a
    // path it already opened.
    ReloadOutcome stage_and_reload() {
        ++attempts_;
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
        // follow-up under item 1.11's staged-file bookkeeping.)
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
    std::filesystem::file_time_type last_mtime_{};
    std::filesystem::path last_staged_;  // this instance's prior staged copy (reaped on next stage)
    bool baseline_set_ = false;
    std::uint64_t attempts_ = 0;
};

} // namespace pulp::format::reload
