// source_jump.hpp — Inspector source-jump: resolve a view's authored
// source location and open it in the user's editor.
//
// Phase 5.1 of the inspector source-jump roadmap
// (planning/2026-05-19-inspector-phase5-source-jump-spike.md).
//
// This is the concrete "jump to source" action. Phase 5.3 already
// shipped the editor-URI *configuration* (pulp/inspect/editor_url.hpp);
// Phase 0b wired a `source_loc` onto `pulp::view::View` from React's
// `__source` prop. This file ties them together:
//
//   1. resolve_source_jump() — given a config + a View's recorded
//      source location, format the editor URL via format_editor_url().
//   2. launch_editor_url() — hand the URL to the OS so the editor
//      actually opens. Kept behind a small seam so headless tests can
//      run the resolve path with `dryRun` and never spawn a process.
//
// The launch shim deliberately lives in inspect/ (dev-tooling), not in
// core/runtime — it is only ever used by the inspector overlay and the
// inspector protocol handler, and is not part of the SDK public API.
#pragma once

#include <pulp/inspect/editor_url.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace pulp::view { class View; }

namespace pulp::inspect {

/// Outcome of a source-jump resolution. `ok` is true only when the
/// view carried a usable source location AND the editor URL formatted
/// cleanly. On failure, `error` explains why (no view, no provenance,
/// bad template) and `url` is empty.
struct SourceJumpResult {
    bool ok = false;
    std::string url;     ///< Formatted editor URL (empty on failure).
    std::string path;    ///< Resolved source file path.
    int line = 0;        ///< Resolved 1-based line (0 = unknown).
    int col = 0;         ///< Resolved 1-based column (0 = unknown).
    bool launched = false;   ///< True if the URL was handed to the OS.
    std::string error;  ///< Human-readable failure reason (empty on ok).
};

/// Resolve the source-jump target for `view` using `config`'s editor
/// URL template (with environment override applied). Does NOT open the
/// editor — pure computation, safe for tests. A null `view`, or a view
/// without a recorded `source_loc()`, yields `ok == false` with a
/// descriptive `error`.
SourceJumpResult resolve_source_jump(const InspectorConfig& config,
                                     const pulp::view::View* view);

/// Hand an editor URL to the OS handler. Returns true if the launch
/// command was dispatched successfully. macOS uses `open`, Linux uses
/// `xdg-open`, Windows uses `ShellExecute`. An empty URL is a no-op
/// that returns false. In CI/headless/test contexts this refuses to
/// spawn and stores the reason in `error` when provided.
///
/// This is the side-effecting seam: callers that want a dry run (tests,
/// the protocol `dryRun` param) should call `resolve_source_jump()` and
/// skip this entirely.
bool launch_editor_url(std::string_view url, std::string* error = nullptr);

/// Convenience: resolve + launch in one call. When `dry_run` is true,
/// behaves exactly like `resolve_source_jump()` (no process spawned,
/// `launched` stays false). When false and the resolve succeeded, the
/// URL is launched and `launched` reflects the OS handler's result.
SourceJumpResult jump_to_source(const InspectorConfig& config,
                                const pulp::view::View* view,
                                bool dry_run);

} // namespace pulp::inspect
