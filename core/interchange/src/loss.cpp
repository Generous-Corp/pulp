#include <pulp/interchange/loss.hpp>

#include <utility>

namespace pulp::interchange {

const LossEntry* LossManifest::find(Concept concept_value) const noexcept {
    for (const LossEntry& entry : entries_) {
        if (entry.concept_value == concept_value)
            return &entry;
    }
    return nullptr;
}

void LossManifest::add(LossEntry entry) { entries_.push_back(std::move(entry)); }

} // namespace pulp::interchange
