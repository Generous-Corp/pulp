// update_mode.hpp — update-mode state machine.
//
// Pure-logic core for the auto/prompt/manual/off mode enforcement
// state machine. The invocation-path hook in pulp_cli.cpp consumes
// this module to decide:
//
//   - Should we emit a banner? If so, which shape?
//   - Should we snooze (prompt mode, user declined or `update-snooze`
//     timestamp is still fresh)?
//   - Should we stage a pending upgrade (auto mode)?
//   - Should we complete a staged pending upgrade on this invocation?
//
// Designed so tests never hit the network, the clock, or the real
// filesystem. All time enters via an injected `now_epoch_sec`
// parameter; all filesystem access goes through the filesystem-path
// helpers which accept a caller-owned tmp dir in tests.
//
// This module deliberately does NOT include cli_common.hpp — tests
// link it standalone (same decoupling pattern as update_check /
// version_diag / projects_registry).

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace pulp::cli::update_mode {

namespace fs = std::filesystem;

// ── Mode enum + parse ───────────────────────────────────────────────────────

enum class Mode {
    Auto,     // silently stage new version; swap on next invocation
    Prompt,   // print a banner; snooze for 24h on decline
    Manual,   // never auto-check or nag (respond only to `pulp upgrade`)
    Off,      // zero network calls (air-gapped / frozen-CI)
};

// Parse a string from ~/.pulp/config.toml. Defaults to Prompt when the
// string is empty or unrecognized. Kept tolerant because a typo in the
// config file should degrade to the safest behavior (prompt once),
// not silently break the update path or fail the invocation.
Mode parse_mode(const std::string& s);

// Printable name — used by `pulp config list` and the banner bodies.
const char* mode_name(Mode m);

// ── Snooze (prompt-mode) ────────────────────────────────────────────────────
//
// `prompt` mode prints one-shot informational banners per version via
// banner_shown_for_version (in update-cache.json). On top of that, a
// time-based snooze lets a user who sees the banner today but isn't
// ready to upgrade suppress re-prompts for 24h regardless of which
// version is current.
//
// File: ~/.pulp/update-snooze (single line: epoch-seconds-of-expiry).
// Missing file / malformed content => not snoozed.

// Serialize the expiry epoch to the single-line file format.
std::string serialize_snooze(std::int64_t expiry_epoch_sec);

// Parse. Returns 0 on error (treated as "not snoozed" by is_snooze_active).
std::int64_t parse_snooze(const std::string& contents);

// True if the snooze file expiry is strictly in the future relative to
// `now_epoch_sec`. Returns false for expired/malformed/missing files.
bool is_snooze_active(const fs::path& snooze_path,
                      std::int64_t now_epoch_sec);

// Write a snooze that expires `hours` from `now_epoch_sec`. Returns
// true on success. Creates the parent dir if missing.
bool write_snooze(const fs::path& snooze_path,
                  std::int64_t now_epoch_sec,
                  int hours);

// Remove the snooze file if present. Used by `pulp upgrade` and
// `pulp config set update.mode <anything>` — either action means the
// user has re-engaged with update management. Returns true if the file
// was removed or absent; false only on a real fs error.
bool clear_snooze(const fs::path& snooze_path);

// ── Pending-upgrade marker (auto-mode staged download) ──────────────────────
//
// auto mode downloads the new binary in the background on one
// invocation and swaps it in on the NEXT invocation — never in the
// middle of the user's command. This is the same two-step model used
// by Claude Code and Codex CLI.
//
// Marker file shape (JSON, single line tolerated):
//   {
//     "version":   "0.31.0",
//     "staged_at": 1713638400,
//     "binary":    "/tmp/pulp-upgrade-0.31.0/pulp"
//   }
//
// The staged binary path points at a file on disk that has NOT yet
// replaced the installed pulp. The completer reads this marker and
// performs the swap (platform-specific, see below).

struct PendingUpgrade {
    std::string version;
    std::int64_t staged_at_epoch_sec = 0;
    std::string staged_binary_path;  // absolute path to the staged binary
};

// Round-trip helpers (never throw).
std::string serialize_pending_upgrade(const PendingUpgrade& p);
std::optional<PendingUpgrade> parse_pending_upgrade(const std::string& json);

bool write_pending_upgrade(const fs::path& marker_path,
                           const PendingUpgrade& p);
std::optional<PendingUpgrade> read_pending_upgrade(const fs::path& marker_path);
bool clear_pending_upgrade(const fs::path& marker_path);

// ── Windows tombstone pattern ───────────────────────────────────────────────
//
// On Windows the running binary is file-locked. The swap pattern used
// by rustup / pip / Python is:
//
//   1. MoveFileEx(installed.exe, installed.exe.old, REPLACE_EXISTING)
//      — allowed even while the old binary is running, because the
//      OS renames by path, not by inode.
//   2. Copy/move the staged binary into the original path.
//   3. On the NEXT invocation of the new binary, detect `*.old` in the
//      same directory and delete it (the previous process has exited,
//      so the OS lock is gone).
//
// Step 1 happens in cmd_upgrade's swap path. Steps 2 happens same-call.
// Step 3 — the tombstone cleanup — happens from the banner hook at the
// top of `main`, the first place any subsequent pulp invocation
// touches. This module owns that cleanup.
//
// On macOS / Linux the OS lets us overwrite a running binary because
// the process holds the inode open; the old bytes stay mapped until
// the process exits. We still expose the cleanup function for parity
// so the caller doesn't platform-switch; on POSIX it's a no-op
// (returns true if the tombstone path does not exist).

// Compute the tombstone path for a given executable path. For
// `/usr/local/bin/pulp` this is `/usr/local/bin/pulp.pulp.old`. The
// `.pulp.old` suffix namespaces the tombstone so we don't collide with
// user files and so sibling tools can recognize and skip it.
fs::path tombstone_path_for(const fs::path& executable);

// Delete the tombstone if it exists. Logs nothing — the caller (the
// banner hook) swallows errors. Returns true if the file was removed
// or was absent; false only on a fatal fs error (e.g. permission
// denied). Safe to call on every invocation.
bool cleanup_tombstone(const fs::path& executable);

// ── Banner composition (mode-specific) ──────────────────────────────────────
//
// These strings are tested verbatim so any change to them must also
// update test_cli_update_mode.cpp in the same PR.

// Manual mode — print once per version, then stay quiet. Shape:
//   "Pulp vX.Y.Z available (you have vA.B.C). Run `pulp upgrade` when
//    you're ready."
std::string compose_manual_notice(const std::string& installed_version,
                                  const std::string& latest_version);

// Auto mode — staged, will swap on next invocation. Shape:
//   "Pulp vX.Y.Z downloaded. The upgrade will complete on your next
//    `pulp` invocation."
std::string compose_auto_staged_notice(const std::string& staged_version);

// Auto mode — staged upgrade has just been completed. Printed once,
// then the marker is cleared. Shape:
//   "Pulp CLI upgraded to vX.Y.Z. Run `pulp upgrade --notes` to see
//    what changed."
std::string compose_auto_completed_notice(const std::string& new_version);

// ── Decision helpers ────────────────────────────────────────────────────────
//
// Small pure functions the dispatch hook composes. Keeping them here
// (rather than inline in pulp_cli.cpp) means the state machine is
// fully exercised by unit tests.

struct PromptDecision {
    // True if the banner should be printed this invocation. False means
    // "stay quiet" — either snooze is active, or the version was
    // already notified this cycle (banner_shown_for_version tracks
    // that).
    bool show_banner = false;

    // True if the caller should write a new snooze file. Only set when
    // the user has already seen the banner for this version and the
    // snooze file is absent. The banner hook is intentionally
    // non-interactive (we print a one-line nag + respect a 24h snooze
    // rather than blocking on stdin) — stdin prompts would break pipes
    // and CI. The Claude Code `/upgrade` skill provides the interactive
    // experience.
    bool write_snooze = false;
};

// Pure helper so the dispatch path doesn't have to inline the full
// conditional. Arguments mirror the real signals: mode, versions,
// whether the banner has already been shown this cycle, whether the
// snooze file is currently active.
PromptDecision decide_prompt_banner(Mode mode,
                                    const std::string& installed_version,
                                    const std::string& latest_version,
                                    bool banner_already_shown_this_cycle,
                                    bool snooze_active);

// True when auto-mode should kick off a background download for
// `latest_version`. False if: not auto mode; no latest known; already
// at latest; a pending-upgrade marker for this same version is already
// on disk (don't re-stage).
bool should_stage_auto_download(Mode mode,
                                const std::string& installed_version,
                                const std::string& latest_version,
                                const std::optional<PendingUpgrade>& existing);

}  // namespace pulp::cli::update_mode
