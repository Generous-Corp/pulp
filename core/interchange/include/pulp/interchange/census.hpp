#pragma once

#include <pulp/interchange/concept.hpp>
#include <pulp/timeline/item_id.hpp>
#include <pulp/timeline/model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::interchange {

/// Bounds on how much evidence one census retains.
struct CensusLimits {
    /// Owner ids are an evidence sample, not the whole set. Counts stay exact
    /// after this ceiling is reached, so a manifest over a large document is
    /// bounded without understating how much is affected.
    std::size_t max_owners_per_concept = 64;
};

/// Which interchange concepts a document uses, how often, and where.
///
/// One walker serves every format. Formats differ in what they can carry, not
/// in what a document contains, so the walk happens once and each format's
/// capability table is applied to the same result.
class ConceptCensus {
  public:
    bool contains(Concept concept_value) const noexcept;
    std::uint64_t count(Concept concept_value) const noexcept;

    /// Up to CensusLimits::max_owners_per_concept ids of the items that carry
    /// the concept. May be shorter than count().
    std::span<const timeline::ItemId> owners(Concept concept_value) const noexcept;

    /// Concepts recorded at least once, in vocabulary order.
    std::vector<Concept> present() const;
    bool empty() const noexcept;

    /// Record one sighting. Readers tag constructs with this as they recognize
    /// them; the walker below uses it for documents already in the model.
    void record(Concept concept_value, timeline::ItemId owner, const CensusLimits& limits);

  private:
    struct Entry {
        std::uint64_t count = 0;
        std::vector<timeline::ItemId> owners;
    };
    std::array<Entry, kConceptCount> entries_{};
};

/// Walk a document and record every concept it uses.
///
/// Only concepts the model can express are ever recorded: concept_detectable_in_model
/// is the generated predicate for which those are. Concepts that exist solely in
/// some interchange format have no representation to find here and stay absent.
ConceptCensus census(const timeline::Project& project, const CensusLimits& limits = {});

} // namespace pulp::interchange
