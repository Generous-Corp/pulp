// host_drag_coalescer.hpp — the host-side half of per-frame drag coalescing.
//
// `PointerCoalescer` collapses a STREAM and deliberately knows nothing about
// hosts. Every host that uses it still has to get the same four things right:
//
//   1. a fail-safe, so motion is never held when nothing will release it,
//   2. one dirty signal per HELD RUN rather than one per sample — the point of
//      coalescing is removing O(events) work, and an invalidation per sample
//      is exactly that work,
//   3. flush-before-handoff ordering at every path that ends a gesture, and
//   4. the counters that make "is coalescing actually on?" answerable, because
//      the fail-safe means a host that silently loses it looks *correct* and
//      merely runs at the old frame rate,
//   5. and the one rule that is NOT obvious: a RELATIVE motion delta must be
//      SUMMED across merged samples, never replaced by the last one. Keeping
//      only the survivor's delta silently shortens every relative drag in
//      proportion to how many samples were merged, which reads as "the knob
//      feels sluggish" and not as a dropped-input bug. Absolute position needs
//      no such care — the newest one is already the whole displacement — which
//      is exactly why the distinction is easy to miss.
//
// Those were written once, inline, in the standalone macOS window host. The
// macOS PLUG-IN editor host — the surface a musician actually drags in a DAW —
// got none of them, because it is a different ObjC class. That is what this
// type exists to stop: the rules live in one portable, headlessly testable
// place, and a host supplies only delivery.
//
// Diagnostics are not decoration here. (4) is the whole reason `stats()` is
// public: the standalone host lost coalescing once to a frame driver that
// never opted in, and because the fail-safe restored the old behaviour there
// was no crash, no log and no red test — only a measured 4x that reproduced on
// some runs and not others.

#pragma once

#include "pulp/view/pointer_coalescer.hpp"

#include <cstdint>
#include <functional>

namespace pulp::view {

/// Owns a `PointerCoalescer` plus the host-side policy around it: the
/// frame-driver opt-in, the idle->pending dirty edge, and the counters.
///
/// Timing still belongs to the caller. There is no clock here: the host calls
/// `submit` per raw event and `flush_frame` once per PRESENTED frame.
class HostDragCoalescer {
  public:
    /// Delivers one sample to the view tree. Called synchronously, and the
    /// host may deliver to a target that has since been unmounted, so the
    /// callable is responsible for its own liveness re-check.
    using Deliver = std::function<void(const PointerSample&)>;

    /// What a host must do in response to a submitted sample.
    struct SubmitResult {
        /// The sample was held and this is the idle->pending edge. The host
        /// must arm EXACTLY ONE repaint/dirty request here — enough to mark
        /// the surface dirty and to keep its frame driver's dispatch gate open
        /// so a flush is guaranteed to come, without the per-sample
        /// invalidation storm coalescing exists to remove.
        bool arm_repaint = false;
        /// Samples handed to `deliver` during this call. Non-zero when the
        /// fail-safe is active, or when the sample could not merge into the
        /// held one (different pointer or button) and forced a flush.
        std::size_t delivered = 0;
    };

    struct Stats {
        /// Samples the host submitted — what an uncoalesced host would have
        /// dispatched.
        std::uint64_t raw_samples = 0;
        /// Samples that actually reached the view tree.
        std::uint64_t delivered_samples = 0;
        /// `flush_frame` calls, i.e. presented frames seen.
        std::uint64_t flushes = 0;
        /// Samples merged away and never dispatched. `raw_samples -
        /// delivered_samples` once no motion is held.
        std::uint64_t merged_samples = 0;
    };

    /// Opt in only from a host that has committed to calling `flush_frame`
    /// once per presented frame. Default is OFF, and that default is load
    /// bearing: holding motion nothing will ever release is not a slow drag,
    /// it is no drag at all.
    ///
    /// Turning it OFF flushes, so a sample submitted during teardown
    /// dispatches immediately rather than joining a batch nobody is left to
    /// release. Call `discard` first when the target may already be gone.
    void set_frame_driver_running(bool running, const Deliver& deliver);

    bool frame_driver_running() const {
        return frame_driver_running_;
    }

    /// Feed one raw pointer sample.
    SubmitResult submit(const PointerSample& sample, const Deliver& deliver);

    /// Release motion held since the last presented frame. Call once per
    /// PRESENTED frame, BEFORE the frame's render decision — a frame that
    /// decides not to paint must still release input, or latency grows without
    /// bound while the UI is visually idle. Returns the number delivered.
    std::size_t flush_frame(const Deliver& deliver);

    /// Drop held motion WITHOUT delivering it, for a host tearing down
    /// mid-gesture where the target may already be freed.
    void discard();

    bool has_pending() const {
        return coalescer_.has_pending();
    }

    const Stats& stats() const {
        return stats_;
    }

  private:
    std::size_t deliver_all_(std::vector<PointerSample>& samples, const Deliver& deliver);
    /// Move the accumulated relative delta onto the first motion sample about
    /// to be dispatched, and reset the accumulator. A dispatched motion sample
    /// stands for every sample merged into it, so it must carry their summed
    /// movement or the consumer sees only the last hop.
    void apply_accumulated_movement_(std::vector<PointerSample>& samples);
    void accumulate_movement_(const PointerSample& sample);

    PointerCoalescer coalescer_;
    Stats stats_{};
    bool frame_driver_running_ = false;
    float pending_movement_x_ = 0.0f;
    float pending_movement_y_ = 0.0f;
    bool pending_has_movement_ = false;
};

} // namespace pulp::view
