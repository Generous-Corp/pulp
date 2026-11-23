#pragma once
#include "interaction_event.hpp"
#include <functional>
#include <vector>

namespace pulp::runtime {

/// Deterministic, in-memory interaction stream recorder used by platform adapters.
class InteractionRecorder {
public:
    void record(const InteractionBatch<>& batch) { batches_.push_back(batch); }
    void clear() noexcept { batches_.clear(); }
    template <typename Fn> void replay(Fn&& fn) const {
        for (const auto& batch : batches_) fn(batch);
    }
    std::size_t size() const noexcept { return batches_.size(); }
private:
    std::vector<InteractionBatch<>> batches_;
};

} // namespace pulp::runtime
