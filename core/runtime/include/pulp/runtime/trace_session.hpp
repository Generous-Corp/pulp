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
#include <optional>
#include <string>
#include <utility>
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

/// Outcome of an exclusive attempt to start the process-global trace session.
/// Inspector control uses this to distinguish a newly configured capture from
/// a session that was already active under another owner.
enum class TraceStartStatus {
    Started,
    AlreadyActive,
    Unavailable,
};

/// Opaque authority over one exact process-global trace generation.
class TraceOwnership {
public:
    TraceOwnership(TraceOwnership&& other) noexcept
        : generation_(std::exchange(other.generation_, 0)),
          token_(std::exchange(other.token_, 0)) {}
    TraceOwnership& operator=(TraceOwnership&& other) noexcept {
        if (this != &other) {
            generation_ = std::exchange(other.generation_, 0);
            token_ = std::exchange(other.token_, 0);
        }
        return *this;
    }
    TraceOwnership(const TraceOwnership&) = delete;
    TraceOwnership& operator=(const TraceOwnership&) = delete;

private:
    friend class Tracing;
    TraceOwnership(std::uint64_t generation,
                   std::uint64_t token) noexcept
        : generation_(generation), token_(token) {}

    std::uint64_t generation_ = 0;
    std::uint64_t token_ = 0;
};

struct TraceStartResult {
    TraceStartStatus status = TraceStartStatus::Unavailable;
    /// Capability returned only to the caller that started this generation.
    std::optional<TraceOwnership> ownership;
};

/// Atomic view of the process-global session relative to one ownership token.
struct TraceOwnershipStatus {
    bool active = false;
    bool owned = false;
};

/// Process-global tracing controller. All methods are thread-safe and safe to
/// call when PULP_TRACING=OFF (they no-op).
///
/// ── Lifetime contract (WAH-4) ────────────────────────────────────────────
///
/// The session, its refcount, and its auto-flush timer are PROCESS-global and
/// live in whichever module first links this TU. In a DAW that means the
/// plug-in binary, which can be unloaded (`FreeLibrary` / `dlclose`) while the
/// host keeps running. Every thread this class owns must therefore be joined
/// before the last `detach()` returns — a detached worker that wakes after
/// unload executes freed code, and on Windows that is a hard crash inside the
/// loader lock rather than a diagnosable fault.
///
/// So: `detach()` is not merely a refcount decrement. The final one cancels and
/// JOINS the auto-flush timer before flushing the session. `attach()`/`detach()`
/// must be balanced and called off the audio thread (adapters call them from
/// their construct/destroy paths); `ScopedTracingAttachment` below makes that
/// balance structural rather than conventional.
///
/// Timeouts are tagged with a session generation. Without it, a timer armed for
/// session N could stop session N+1 — a real sequence when a user closes and
/// reopens an editor inside the `PULP_TRACE_SECONDS` window, which silently
/// truncated the second capture.
class Tracing {
public:
    /// Start the single process session if none is active. `categories` selects
    /// the enabled track-event categories (empty = all). `out_path` overrides
    /// the default temp path (also overridable via $PULP_TRACE_PATH).
    /// `ring_kb` sizes the in-process ring (default 80 MB). Returns true when a
    /// session is active on return, including after an idempotent duplicate.
    static bool start(const std::vector<std::string>& categories = {},
                      const std::string& out_path = {},
                      std::uint32_t ring_kb = 80u * 1024u);

    /// Start only when no process session exists, reporting whether this call
    /// actually applied the requested configuration.
    static TraceStartResult start_exclusive(
        const std::vector<std::string>& categories = {},
        const std::string& out_path = {},
        std::uint32_t ring_kb = 80u * 1024u);

    /// Stop the active session, flush, and write the .pftrace. No-op → ok=false.
    static TraceStopResult stop();

    /// Stop only when both opaque ownership fields match the active session.
    static TraceStopResult stop_owned(const TraceOwnership& ownership);

    /// Read active and ownership state under one runtime lock.
    static TraceOwnershipStatus ownership_status(
        const TraceOwnership* ownership);

    /// Whether a process session is currently active.
    static bool active();

    /// Reference-counted attachment for multi-instance hosts. The last detach
    /// after all instances are gone cancels and JOINS the auto-flush timer,
    /// then tears the session down. Balanced calls; prefer
    /// `ScopedTracingAttachment`.
    static void attach();
    static void detach();

    /// Generation of the session currently active, or 0 when none is. Bumped by
    /// every successful `start()`. A timeout captured for generation N is a
    /// no-op once this has moved on, so a stale timer cannot truncate a later
    /// capture. Exposed for tests.
    static std::uint64_t session_generation();

    /// Current attachment count. Exposed for tests and for a host that wants to
    /// assert its attach/detach balance.
    static int attachment_count();
};

/// RAII attachment. Construct one per plug-in instance / app that wants tracing
/// alive for its lifetime; the last one destroyed flushes the trace.
///
/// Exists because the balance is easy to get wrong in exactly the place it
/// matters most: an adapter that returns early from its destroy path leaks an
/// attachment, and the trace is then never flushed — the capture silently
/// produces an empty file and the session looks like it "did not record".
/// Move-only, so an accidental copy cannot double-detach.
class ScopedTracingAttachment {
public:
    ScopedTracingAttachment() : attached_(true) { Tracing::attach(); }
    ScopedTracingAttachment(const ScopedTracingAttachment&) = delete;
    ScopedTracingAttachment& operator=(const ScopedTracingAttachment&) = delete;
    ScopedTracingAttachment(ScopedTracingAttachment&& other) noexcept
        : attached_(other.attached_) {
        other.attached_ = false;
    }
    ScopedTracingAttachment& operator=(ScopedTracingAttachment&& other) noexcept {
        if (this != &other) {
            reset();
            attached_ = other.attached_;
            other.attached_ = false;
        }
        return *this;
    }
    ~ScopedTracingAttachment() { reset(); }

    /// Detach early. Idempotent.
    void reset() noexcept {
        if (!attached_) return;
        attached_ = false;
        Tracing::detach();
    }

    bool attached() const noexcept { return attached_; }

private:
    bool attached_ = false;
};

}  // namespace pulp::runtime
