#include "pulp/view/host_drag_coalescer.hpp"

#include <pulp/runtime/trace.hpp>

namespace pulp::view {

void HostDragCoalescer::accumulate_movement_(const PointerSample& sample) {
    // Only motion carries a delta forward. A transition is dispatched with its
    // own movement and is never merged into, so accumulating it would strand
    // the value on an accumulator nothing will ever apply.
    if (!sample.is_motion()) return;
    if (!sample.pointer.has_movement_delta) return;
    pending_movement_x_ += sample.pointer.movement_x;
    pending_movement_y_ += sample.pointer.movement_y;
    pending_has_movement_ = true;
}

void HostDragCoalescer::apply_accumulated_movement_(
    std::vector<PointerSample>& samples) {
    for (auto& s : samples) {
        if (!s.is_motion()) continue;
        if (pending_has_movement_) {
            s.pointer.movement_x = pending_movement_x_;
            s.pointer.movement_y = pending_movement_y_;
            s.pointer.has_movement_delta = true;
        }
        pending_movement_x_ = 0.0f;
        pending_movement_y_ = 0.0f;
        pending_has_movement_ = false;
        // At most one motion sample is ever dispatched at a time (the
        // coalescer holds a single run), so the first is the only one.
        return;
    }
}

std::size_t HostDragCoalescer::deliver_all_(std::vector<PointerSample>& samples,
                                            const Deliver& deliver) {
    apply_accumulated_movement_(samples);
    for (const auto& s : samples) {
        ++stats_.delivered_samples;
        PULP_TRACE_COUNTER("state", "delivered_drag_samples",
                           stats_.delivered_samples);
        if (deliver) deliver(s);
    }
    return samples.size();
}

void HostDragCoalescer::set_frame_driver_running(bool running,
                                                 const Deliver& deliver) {
    if (frame_driver_running_ == running) return;
    // Clear the opt-in BEFORE flushing, so a sample arriving during teardown
    // takes the fail-safe path rather than joining a batch nobody is left to
    // release.
    frame_driver_running_ = running;
    if (!running) {
        auto pending = coalescer_.flush_frame();
        deliver_all_(pending, deliver);
        stats_.merged_samples = coalescer_.total_merged();
    }
}

HostDragCoalescer::SubmitResult HostDragCoalescer::submit(
    const PointerSample& sample, const Deliver& deliver) {
    ++stats_.raw_samples;
    PULP_TRACE_COUNTER("state", "raw_drag_samples", stats_.raw_samples);

    SubmitResult result;
    if (!frame_driver_running_) {
        // Fail safe — see set_frame_driver_running.
        accumulate_movement_(sample);
        std::vector<PointerSample> immediate{sample};
        result.delivered = deliver_all_(immediate, deliver);
        result.arm_repaint = true;
        return result;
    }

    const bool was_idle = !coalescer_.has_pending();
    auto due = coalescer_.submit(sample);
    // `due` is empty for motion that merged into the held run. It is non-empty
    // when this sample could not merge (different pointer or button) and
    // displaced the held run, or when it is a transition — the coalescer
    // flushes held motion ahead of a transition so a gesture never sees its
    // terminal event before the movement that preceded it.
    //
    // In BOTH of those cases the flushed motion is the OLDER run, so it takes
    // the delta accumulated so far and `sample`'s own delta starts the next
    // one. Ordering the accumulate after the dispatch is what keeps this
    // sample's movement out of the previous run's total.
    if (!due.empty()) result.delivered = deliver_all_(due, deliver);
    accumulate_movement_(sample);
    stats_.merged_samples = coalescer_.total_merged();
    PULP_TRACE_COUNTER("state", "pointer_samples_merged", stats_.merged_samples);
    result.arm_repaint = was_idle;
    return result;
}

std::size_t HostDragCoalescer::flush_frame(const Deliver& deliver) {
    PULP_TRACE_SCOPE_NAMED("state", "pointer_coalescer_flush");
    auto pending = coalescer_.flush_frame();
    ++stats_.flushes;
    PULP_TRACE_COUNTER("state", "pointer_coalescer_flushes", stats_.flushes);
    stats_.merged_samples = coalescer_.total_merged();
    PULP_TRACE_COUNTER("state", "pointer_samples_merged", stats_.merged_samples);
    return deliver_all_(pending, deliver);
}

void HostDragCoalescer::discard() {
    coalescer_.reset();
    pending_movement_x_ = 0.0f;
    pending_movement_y_ = 0.0f;
    pending_has_movement_ = false;
}

}  // namespace pulp::view
