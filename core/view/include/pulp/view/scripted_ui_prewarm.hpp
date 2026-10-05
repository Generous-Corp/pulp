#pragma once

// Background preparation of a scripted editor's process-wide caches, so the
// host's view-creation call does not pay for the first parse of the UI.
//
// A hosted editor mounts its document inside the host's view-creation call
// (content-first open), and the first open in a process pays one-time costs
// there: QuickJS compiling the UI runtime and every inline script of a
// materialized document, and the decode + SHA-256 verification of that
// document. Both results are already kept process-wide (the script bytecode
// cache and the materialized document cache), so a plug-in can produce them
// before an editor is requested -- typically from its constructor, which a
// host runs when it instantiates the plug-in -- on a background thread that
// never touches a realm, a view or the audio thread.
//
// What it does NOT do: build views, run any script, or keep a realm alive.
// The per-open work (evaluating the runtime, laying out the tree, the first
// GPU frame) still happens in the view-creation call.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::view {

struct ScriptedUiPrewarmRequest {
    /// Scripts the editor loads through `WidgetBridge::load_script()` (a
    /// ScriptedUiSession's script file, a design or help script the processor
    /// loads after the mount), byte-identical to the text it passes there; the
    /// prewarm compiles `WidgetBridge::loaded_script_source()` of each, which
    /// is what load_script() evaluates. Sources under the bytecode cache's size
    /// floor are ignored.
    std::vector<std::string> scripts;
    /// Inputs the editor passes to `__pulpRuntimeImport__(text,
    /// 'materialized-browser')`, byte-identical. Each is decoded and verified
    /// into the materialized document cache, and its payload and inline
    /// scripts are compiled.
    std::vector<std::string> materialized_documents;
};

/// Queue `request` for the process's prewarm worker and return at once. The
/// worker is one background thread, started on first use, that processes
/// requests in order and is joined when the process (or the plug-in image)
/// tears down. Work already cached, or in flight on another thread, is
/// skipped, so every plug-in instance may call this with the same request;
/// an editor that opens while its scripts are still compiling waits for that
/// compile instead of starting its own.
///
/// A no-op when `PULP_EDITOR_PREWARM=0` is set in the environment (A/B and
/// negative controls), or when the default script engine is not QuickJS.
void prewarm_scripted_ui(ScriptedUiPrewarmRequest request);

struct ScriptedUiPrewarmStats {
    std::uint64_t requests = 0;            ///< requests accepted
    std::uint64_t scripts_compiled = 0;    ///< scripts this worker compiled
    std::uint64_t documents_verified = 0;  ///< documents this worker decoded
    std::uint64_t documents_rejected = 0;  ///< documents that failed verification
    bool idle = true;                      ///< nothing queued or running
};
ScriptedUiPrewarmStats scripted_ui_prewarm_stats();

/// Block until the worker is idle or `timeout` passes; true when idle.
/// For tests and measurement harnesses; an editor never needs it.
bool wait_for_scripted_ui_prewarm(std::chrono::milliseconds timeout);

}  // namespace pulp::view
