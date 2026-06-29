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
/// Caveat: detection is mtime-equality based, so two writes within the
/// filesystem's timestamp resolution (sub-µs on APFS/ext4, but 2 s on FAT/exFAT)
/// can be missed; reload_now() is the escape hatch on a coarse filesystem.
///
/// Each distinct file version is attempted exactly once: the controller records
/// the mtime it acted on whether the reload succeeded or failed, so a candidate
/// that fails its gates is not retried on every tick — only the next edit (a new
/// mtime) triggers another attempt. The first poll() establishes the baseline
/// without reloading (the shell already loaded the initial logic).
///
/// CRITICAL — load each version from a UNIQUE path: dlopen/LoadLibrary cache by
/// path, so re-opening the watched path after the developer overwrites it
/// returns the STALE in-memory image, not the new bytes. The controller
/// therefore stages each detected version to a fresh, uniquely-named copy in
/// `stage_dir` and reloads THAT, guaranteeing the loader reads the new file.
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

namespace pulp::format::reload {

/// NOT thread-safe: a ReloadController is driven from a single (control) thread
/// — the shell's event loop ticks poll(). Its only cross-thread effect is the
/// slot swap inside ReloadSession, which is itself RT-safe.
class ReloadController {
public:
    /// @param stage_dir where versioned copies are staged before loading; must
    /// be dlopen-safe. Defaults to the watched file's directory (or "." if the
    /// path has none — never a bare leaf name, which dlopen would treat as a
    /// search-path lookup rather than a direct file load).
    ReloadController(ReloadSession& session, std::filesystem::path logic_path,
                     std::filesystem::path stage_dir = {})
        : session_(session), path_(std::move(logic_path)),
          stage_dir_(!stage_dir.empty()    ? std::move(stage_dir)
                     : path_.has_parent_path() ? path_.parent_path()
                                               : std::filesystem::path(".")) {}

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
    // path it already opened. The staged name carries a per-instance id so two
    // controllers in one process (same stem + stage_dir) can't collide on a name
    // and reintroduce the cache bug.
    ReloadOutcome stage_and_reload() {
        ++attempts_;
        const auto staged =
            stage_dir_ / (path_.stem().string() + ".reload" + std::to_string(instance_id_) +
                          "_" + std::to_string(attempts_) + path_.extension().string());
        std::error_code ec;
        std::filesystem::copy_file(
            path_, staged, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return {ReloadOutcome::Status::RejectedLoadFailed,
                    "stage copy failed: " + ec.message()};
        }
        // Reclaim the previous staged copy so a long dev session doesn't strew
        // the build dir. Unlinking a still-dlopen'd file is safe (the loaded
        // image stays mapped); only the current staged file is kept on disk.
        if (!last_staged_.empty()) {
            std::error_code rm; std::filesystem::remove(last_staged_, rm);
        }
        last_staged_ = staged;
        return session_.reload(staged.string());
    }

    ReloadSession& session_;
    std::filesystem::path path_;
    std::filesystem::path stage_dir_;
    std::filesystem::path last_staged_;
    std::filesystem::file_time_type last_mtime_{};
    bool baseline_set_ = false;
    std::uint64_t attempts_ = 0;
    // Unique per controller instance, to disambiguate staged filenames.
    inline static std::atomic<std::uint64_t> s_next_instance_{0};
    const std::uint64_t instance_id_ = s_next_instance_.fetch_add(1, std::memory_order_relaxed);
};

} // namespace pulp::format::reload
