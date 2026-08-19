// host_pointer_input.hpp — the host-side half of per-frame pointer coalescing.
//
// `PointerCoalescer` collapses a stream; it deliberately knows nothing about
// timing. Somebody still has to answer the two questions it leaves open:
//
//   1. Is there a driver that will release held motion once per presented
//      frame? Holding motion with nothing to flush it is not a slow drag, it
//      is NO drag — silently, with no error anywhere.
//   2. Did that driver actually get installed on the path this build took?
//
// Question 2 is not hypothetical. The first host to wire coalescing set its
// opt-in inside ONE of its two frame-driver start paths (`start_display_link`
// but not `start_hidden_frame_timer`). Both paths drive the same callback, so
// the flush ran either way — but on the second path the opt-in was never set,
// so every sample took the immediate-dispatch fail-safe and coalescing silently
// never engaged. The same binary behaved differently depending on how it was
// started, and nothing said so.
//
// This type closes that class of bug two ways:
//
//   * The opt-in is not a flag the caller sets next to the driver, it IS the
//     act of declaring a driver: `begin_flush_driver()` / `end_flush_driver()`.
//     A host with two start paths calls the same method from both.
//   * When a frame tick observes that motion has been dispatching undriven, it
//     says so — once, cheaply, always on. "Frames are being driven AND motion
//     bypassed the coalescer" is exactly the silent-no-engage signature, and
//     nothing else produces it: a host with no frame driver never ticks, so the
//     legitimate CPU-only case (see below) stays quiet.
//
// The fail-safe is preserved throughout: with no driver declared, every sample
// dispatches immediately, which is precisely the pre-coalescing behavior. A
// host that never calls `begin_flush_driver()` is correct, just unimproved —
// the macOS CPU window host has no display link and lives there permanently.

#pragma once

#include "pulp/view/pointer_coalescer.hpp"

#include <cstddef>
#include <functional>

namespace pulp::view {

/// Owns a `PointerCoalescer` plus the "is anything going to flush this?"
/// decision, and reports when that decision was wrong.
///
/// Single-threaded: every method runs on the UI thread, the same thread the
/// platform delivers input on and the same thread a display-link callback
/// hops to before flushing.
class HostPointerInput {
public:
    /// Invoked for each sample that must reach the view tree now. The host
    /// supplies delivery because only the host knows how to resolve its
    /// capture and which dispatch verb the sample means.
    using Deliver = std::function<void(const PointerSample&)>;

    // ── Flush-driver lifecycle ──────────────────────────────────────────
    //
    // Call `begin_flush_driver()` from EVERY path that establishes a per-frame
    // flush — not from one chosen start function. If a host has a display link
    // and a timer fallback, both call this, because both will be calling
    // `flush_for_frame()`.

    /// Declare that a per-frame flush driver is now running. Idempotent.
    void begin_flush_driver();

    /// Withdraw the driver and release anything still held.
    ///
    /// Order is load-bearing: the flag clears FIRST, so a sample arriving
    /// during teardown dispatches immediately rather than joining a batch
    /// nobody will flush. Idempotent.
    void end_flush_driver(const Deliver& deliver);

    bool flush_driver_active() const { return driver_active_; }

    // ── Input ───────────────────────────────────────────────────────────

    /// Feed one raw sample.
    ///
    /// Motion is held for the next frame when a driver is active, and
    /// dispatched immediately when one is not. Anything that is not motion is
    /// dispatched immediately, behind any motion it must not overtake — the
    /// coalescer's own ordering contract.
    ///
    /// Returns true when this call took the coalescer from idle to holding.
    /// That transition — not every sample — is when a host should mark its
    /// surface dirty: one repaint request per frame is enough both to schedule
    /// the paint and to keep the frame driver's dispatch gate open, and issuing
    /// one per sample is the O(events) cost this machinery exists to remove.
    bool submit(const PointerSample& sample, const Deliver& deliver);

    /// Release held motion for a PRESENTED FRAME. Call once per frame tick,
    /// and only from the frame driver — this is the entry point that reports
    /// the silent-no-engage bug, so calling it from an ordinary event path
    /// would blunt the detection.
    void flush_for_frame(const Deliver& deliver);

    /// Release held motion from a path that must not defer: a terminal event,
    /// or a handoff that is about to make the current capture unreachable.
    /// Safe when nothing is held.
    void flush_now(const Deliver& deliver);

    /// Drop held motion WITHOUT delivering it. For teardown, where the target
    /// may already be gone.
    void reset() { coalescer_.reset(); }

    bool has_pending() const { return coalescer_.has_pending(); }

    // ── Diagnostics ─────────────────────────────────────────────────────

    /// Motion samples that bypassed the coalescer because no driver was
    /// declared. Cleared when a driver is declared, so this counts undriven
    /// motion under the CURRENT driver state rather than accumulating a
    /// legitimate pre-attach history.
    std::size_t undriven_motion_samples() const { return undriven_motion_; }

    /// Latched true once a frame tick observed undriven motion — i.e. this
    /// host drives frames but its opt-in never reached the input path.
    /// Read by tests; the same condition also logs once to stderr.
    bool undriven_motion_detected() const { return undriven_detected_; }

    /// Motion samples merged away since construction. Proof of work.
    std::size_t total_merged() const { return coalescer_.total_merged(); }

    /// Motion samples this host has been handed since construction, merged or
    /// not. The POSITIVE counterpart to the detector below: the detector only
    /// reports when coalescing is off, and its silence is equally consistent
    /// with "working" and with "no input ever arrived" — which is exactly the
    /// ambiguity that makes a real-host measurement worthless. A run that can
    /// state how many samples it received, and how many of those it merged,
    /// distinguishes the two.
    std::size_t total_motion_samples() const { return motion_samples_; }

    /// One line of the above, for a host to log at the end of a gesture under
    /// an env gate. Returns false when no motion was seen at all, so a caller
    /// can say "no input reached this view" rather than print a row of zeros
    /// that reads like a successful measurement.
    bool format_stats(char* out, std::size_t out_size) const;

private:
    PointerCoalescer coalescer_;
    bool driver_active_ = false;
    std::size_t undriven_motion_ = 0;
    std::size_t motion_samples_ = 0;
    bool undriven_detected_ = false;
    bool logged_undriven_ = false;
};

}  // namespace pulp::view
