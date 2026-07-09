// trace_session.hpp — process-global tracing session control (DEV ONLY).
//
// The lifecycle facade behind <pulp/runtime/trace.hpp>'s span macros. Perfetto's
// Tracing::Initialize() + TrackEvent::Register() are PROCESS singletons and a
// DAW loads N plugin instances into one process, so this is a single process
// wide controller with reference-counted attachment (plan §0b item 5), not a
// per-instance object. One active in-process session at a time.
//
// The API is identical in both build configs: with PULP_TRACING=OFF every call
// is a no-op returning "inactive", so callers need no #ifdef. No Perfetto type
// appears in this header, so OFF consumers pull in zero Perfetto headers.
//
// RT-safety (D1): tracing here rides UI/render/process + OFFLINE-audio threads
// only. Never call start()/the span macros from a live audio callback.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::runtime {

/// One-shot gate for the "tracing compiled in" reminder. Returns `true` exactly
/// once per `already_emitted` flag (on its first call) and `false` thereafter,
/// so a reminder can be logged a single time even across threads and multiple
/// plugin instances. Config-independent so the gate is unit-testable in the
/// default OFF build.
bool tracing_reminder_first_time(std::atomic<bool>& already_emitted);

/// Emit — at most once for the whole process — a single dev-build reminder that
/// Perfetto tracing is compiled in. In a PULP_TRACING=ON build this logs one
/// `log_info` line the first time it is called; in the default OFF build it is
/// a no-op that logs nothing. Safe to call unconditionally from app/plugin
/// startup (no `#ifdef` at the call site).
void log_tracing_reminder();

/// Result of stopping a session: the flushed trace path + basic loss accounting
/// so a caller can tell whether the ring dropped data under load (plan §0b #4).
struct TraceStopResult {
    bool ok = false;            ///< A session was active and a trace was written.
    std::string path;           ///< Absolute path to the flushed .pftrace.
    std::uint64_t trace_bytes = 0;
};

/// Process-global tracing controller. All methods are thread-safe and safe to
/// call when PULP_TRACING=OFF (they no-op).
class Tracing {
public:
    /// Start the single process session if none is active. `categories` selects
    /// the enabled track-event categories (empty = all). `out_path` overrides
    /// the default temp path (also overridable via $PULP_TRACE_PATH).
    /// `ring_kb` sizes the in-process ring (default 80 MB). Returns true if a
    /// session is active on return (either newly started or already running).
    static bool start(const std::vector<std::string>& categories = {},
                      const std::string& out_path = {},
                      std::uint32_t ring_kb = 80u * 1024u);

    /// Stop the active session, flush, and write the .pftrace. No-op → ok=false.
    static TraceStopResult stop();

    /// Whether a process session is currently active.
    static bool active();

    /// Reference-counted attachment for multi-instance hosts. The last detach
    /// after all instances are gone tears the session down. Balanced calls.
    static void attach();
    static void detach();
};

}  // namespace pulp::runtime
