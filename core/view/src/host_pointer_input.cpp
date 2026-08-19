#include "pulp/view/host_pointer_input.hpp"

#include <cstdio>
#include <cstddef>

namespace pulp::view {

void HostPointerInput::begin_flush_driver() {
    driver_active_ = true;
    // Undriven motion from BEFORE this driver existed is not the bug — a view
    // can legitimately receive input in a window it has not yet started a link
    // for, and a driver that stopped for a detach and restarted is a new
    // question. Zero here so `undriven_motion_samples()` only ever answers
    // "motion bypassed the coalescer while THIS driver was running", which is
    // the condition `flush_for_frame` reports on.
    undriven_motion_ = 0;
}

void HostPointerInput::end_flush_driver(const Deliver& deliver) {
    driver_active_ = false;
    flush_now(deliver);
}

bool HostPointerInput::submit(const PointerSample& sample, const Deliver& deliver) {
    if (sample.is_motion()) ++motion_samples_;
    if (!driver_active_) {
        // Fail safe. This is the pre-coalescing behavior exactly: dispatch the
        // sample now. Holding it would mean waiting for a flush that is not
        // coming.
        //
        // Anything already held (a driver that stopped mid-gesture) goes first,
        // so withdrawing the driver cannot reorder a gesture.
        if (coalescer_.has_pending()) flush_now(deliver);
        if (sample.is_motion()) ++undriven_motion_;
        if (deliver) deliver(sample);
        return false;
    }

    const bool was_idle = !coalescer_.has_pending();
    // submit() returns whatever must not wait: empty for mergeable motion, and
    // for a transition (or a sample that cannot merge into the held one — a
    // different button or pointer id) the held motion first, then the sample.
    for (const auto& due : coalescer_.submit(sample)) {
        if (deliver) deliver(due);
    }
    return was_idle && coalescer_.has_pending();
}

void HostPointerInput::flush_for_frame(const Deliver& deliver) {
    // Frames are being driven. If motion has been taking the undriven
    // fail-safe path while that was true, the opt-in never reached the input
    // path — coalescing is silently off and this host is running at its
    // pre-fix cost. Say so once: it is a one-branch check on a path that
    // already exists, and the alternative is what this bug did the first time,
    // which is to be discoverable only by measuring the same binary twice.
    if (undriven_motion_ > 0 && !logged_undriven_) {
        logged_undriven_ = true;
        undriven_detected_ = true;
        std::fprintf(stderr,
                     "[pulp-view] pointer coalescing is NOT engaged: %zu motion "
                     "sample(s) dispatched immediately while frames are being "
                     "driven. A frame driver is running but never called "
                     "begin_flush_driver(), so drags cost one full dispatch per "
                     "OS event.\n",
                     undriven_motion_);
    }
    flush_now(deliver);
}

bool HostPointerInput::format_stats(char* out, std::size_t out_size) const {
    if (!out || out_size == 0) return false;
    if (motion_samples_ == 0) {
        std::snprintf(out, out_size, "no pointer motion reached this view");
        return false;
    }
    // Report RECEIVED and MERGED as two independently-sourced numbers, not one
    // number and a derived twin. `motion_samples_` is counted here on the way
    // in; `total_merged()` comes from the coalescer's own bookkeeping. They can
    // disagree, and the disagreement is the whole signal:
    //
    //   driver active   -> merged > 0, ratio > 1.0  (coalescing is doing work)
    //   no driver       -> merged == 0, ratio == 1.0 (every sample dispatched)
    //
    // A counter whose two halves can never differ proves only that the code
    // ran, which is the same as proving nothing.
    const std::size_t merged = coalescer_.total_merged();
    const std::size_t dispatched = motion_samples_ - merged;
    std::snprintf(out, out_size,
                  "merged %zu of %zu samples -> %zu dispatches (%.2f samples per "
                  "dispatch) driver=%s",
                  merged, motion_samples_, dispatched,
                  dispatched ? (double)motion_samples_ / (double)dispatched : 0.0,
                  driver_active_ ? "active" : "NONE");
    return true;
}

void HostPointerInput::flush_now(const Deliver& deliver) {
    for (const auto& s : coalescer_.flush_frame()) {
        if (deliver) deliver(s);
    }
}

}  // namespace pulp::view
