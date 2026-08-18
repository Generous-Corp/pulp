#include "pulp/view/pointer_coalescer.hpp"

namespace pulp::view {

std::vector<PointerSample> PointerCoalescer::take_pending_() {
    std::vector<PointerSample> out;
    if (!has_pending_) {
        last_merged_ = 0;
        return out;
    }
    // The surviving sample keeps the newest position and carries the merged
    // path so a consumer that cannot reconstruct skipped motion still can.
    out.push_back(pending_);
    last_merged_ = merged_in_pending_;
    total_merged_ += merged_in_pending_;
    has_pending_ = false;
    merged_in_pending_ = 0;
    path_.clear();
    return out;
}

std::vector<PointerSample> PointerCoalescer::submit(const PointerSample& sample) {
    if (sample.is_motion()) {
        // Motion from a DIFFERENT pointer or button than the one already held
        // is not the same stream, so merging it would fabricate a path that
        // never happened. Flush what is held and start a new run.
        if (has_pending_
            && (pending_.pointer.pointer_id != sample.pointer.pointer_id
                || pending_.button != sample.button
                || pending_.phase != sample.phase)) {
            auto flushed = take_pending_();
            pending_ = sample;
            path_.clear();
            path_.push_back(sample.position);
            has_pending_ = true;
            merged_in_pending_ = 0;
            return flushed;
        }

        if (has_pending_) ++merged_in_pending_;
        pending_ = sample;              // newest position wins
        path_.push_back(sample.position);
        has_pending_ = true;
        return {};
    }

    // A button transition. Held motion goes first so the gesture never sees
    // its terminal event before the movement that preceded it.
    auto out = take_pending_();
    out.push_back(sample);
    return out;
}

std::vector<PointerSample> PointerCoalescer::flush_frame() {
    return take_pending_();
}

void PointerCoalescer::reset() {
    has_pending_ = false;
    merged_in_pending_ = 0;
    last_merged_ = 0;
    path_.clear();
}

}  // namespace pulp::view
