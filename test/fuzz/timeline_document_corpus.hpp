#pragma once

/// Seed corpus loading and deterministic mutation for Timeline document fuzzing.
///
/// The mutator is seeded and reproducible on purpose. A finding is only useful
/// if the bytes that produced it can be regenerated, and a fuzz lane that
/// cannot hand a developer the exact failing input is a lane that gets muted
/// the first time it fails. Every generated case is identified by its
/// `(seed, index)` pair, which is sufficient to reconstruct it byte for byte.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::test::timeline_fuzz {

/// One corpus entry with the identity needed to reproduce it.
struct CorpusEntry {
    /// Reproduction label: a fixture path, or `mutant/<seed>/<index>`.
    std::string label;
    /// Raw bytes handed to the parser.
    std::string bytes;
};

/// Loads the fixture documents enumerated by `test/fixtures/timeline/corpus.index`.
///
/// Only entries the index declares as parseable timeline JSON are returned;
/// payload blobs and foreign-format interchange inputs are declared in the
/// index precisely so they can be excluded here rather than silently skipped.
///
/// @param fixture_dir Root of the timeline fixture tree.
/// @return Seed documents in index order.
std::vector<CorpusEntry> load_seed_corpus(const std::string& fixture_dir);

/// Deterministic byte-level mutator over a seed corpus.
///
/// The operations are chosen for the failure modes a bounded document parser
/// actually has — nesting depth, container-count amplification, numeric edges,
/// truncation, and UTF-8 escape corruption — rather than uniform random bit
/// flips, which spend nearly all their budget producing inputs the JSON
/// tokenizer rejects in its first few bytes.
class CorpusMutator {
  public:
    /// Constructs a mutator over `seeds` driven by `seed`.
    CorpusMutator(std::vector<CorpusEntry> seeds, std::uint64_t seed);

    /// Returns the mutant at `index`, which depends only on the seed and index.
    CorpusEntry generate(std::uint64_t index) const;

    /// Returns whether any seed document was loaded.
    bool empty() const noexcept {
        return seeds_.empty();
    }

  private:
    std::vector<CorpusEntry> seeds_;
    std::uint64_t seed_ = 0;
};

} // namespace pulp::test::timeline_fuzz
