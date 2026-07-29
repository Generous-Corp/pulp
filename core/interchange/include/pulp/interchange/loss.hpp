#pragma once

#include <pulp/interchange/capability.hpp>
#include <pulp/interchange/concept.hpp>
#include <pulp/timeline/item_id.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace pulp::interchange {

/// One concept a document uses that an export cannot carry intact.
struct LossEntry {
    Concept concept_value = Concept::Unknown;
    /// Drop or Degrade. Full and RoundtripOnly concepts never appear here.
    ExportLevel level = ExportLevel::Drop;
    LossClass loss_class = LossClass::Dropped;
    /// The concept a degrade lands on. Empty when the concept is dropped.
    std::optional<Concept> degraded_to;
    /// How many times the document uses the concept. Exact.
    std::uint64_t count = 0;
    /// A bounded sample of the affected items, per CensusLimits.
    std::vector<timeline::ItemId> owners;
    /// The format's own account of what is lost, from its capability table.
    std::string_view detail;
};

/// Everything one export costs the document.
///
/// A refused import protects the reader automatically; a lossy export produces
/// a file the receiving application opens successfully with data quietly gone,
/// and will never mention it. The manifest is the only place that says so, so
/// it is an artifact of the export rather than a diagnostic printed alongside.
class LossManifest {
  public:
    std::span<const LossEntry> entries() const noexcept { return entries_; }
    bool empty() const noexcept { return entries_.empty(); }
    const LossEntry* find(Concept concept_value) const noexcept;
    void add(LossEntry entry);

  private:
    std::vector<LossEntry> entries_;
};

/// Stable id for the lossy export level written into manifests.
constexpr std::string_view export_level_id(ExportLevel level) noexcept {
    switch (level) {
    case ExportLevel::Drop:
        return "drop";
    case ExportLevel::Degrade:
        return "degrade";
    case ExportLevel::RoundtripOnly:
        return "roundtrip-only";
    case ExportLevel::Full:
        return "full";
    }
    return "drop";
}

/// Stable id for a loss class, for manifests written into an artifact.
constexpr std::string_view loss_class_id(LossClass loss_class) noexcept {
    switch (loss_class) {
    case LossClass::Dropped:
        return "dropped";
    case LossClass::Degraded:
        return "degraded";
    case LossClass::Flattened:
        return "flattened";
    case LossClass::Approximated:
        return "approximated";
    }
    return "dropped";
}

} // namespace pulp::interchange
