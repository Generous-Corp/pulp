#include "fuzz/timeline_document_corpus.hpp"
#include "fuzz/timeline_document_oracle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace pulp::test::timeline_fuzz;
using pulp::timeline::DecodeLimits;
using pulp::timeline::PersistenceErrorCode;

namespace {

std::uint64_t env_number(const char* name, std::uint64_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    return std::strtoull(raw, nullptr, 10);
}

std::vector<CorpusEntry> seed_corpus() {
    return load_seed_corpus(PULP_TIMELINE_FIXTURE_DIR);
}

} // namespace

// The oracle's own falsifiability. Every judgement below is fed an outcome that
// is known to be wrong, and is required to report it. Without these, a
// judgement that silently returned "no finding" for every input would make the
// whole lane green forever, and nothing in a clean fuzz run would reveal it.

TEST_CASE("Timeline quota oracle reports a document admitted over a declared ceiling",
          "[timeline][fuzz]") {
    StructureCensus census;
    census.clips = 12;

    DecodeLimits generous;
    REQUIRE_FALSE(judge_quota_admission(census, generous, EntryPoint::Project));

    DecodeLimits tight;
    tight.max_clips = 11;
    const auto finding = judge_quota_admission(census, tight, EntryPoint::Project);
    REQUIRE(finding);
    REQUIRE(finding.kind == FindingKind::QuotaAdmission);
    REQUIRE(finding.axis == "clips");
    REQUIRE(finding.observed == 12);
    REQUIRE(finding.ceiling == 11);
}

TEST_CASE("Timeline quota oracle reports a ceiling that admits a document it should reject",
          "[timeline][fuzz]") {
    ParseOutcome admitted;
    admitted.accepted = true;
    const auto not_enforced =
        judge_tightened_outcome("clips", 12, 11, admitted, EntryPoint::Project);
    REQUIRE(not_enforced);
    REQUIRE(not_enforced.kind == FindingKind::QuotaNotEnforced);
    REQUIRE(not_enforced.axis == "clips");

    ParseOutcome enforced;
    enforced.accepted = false;
    enforced.code = PersistenceErrorCode::LimitExceeded;
    enforced.actual = 12;
    enforced.limit = 11;
    REQUIRE_FALSE(judge_tightened_outcome("clips", 12, 11, enforced, EntryPoint::Project));
}

TEST_CASE("Timeline quota oracle reports a rejection that does not describe an overrun",
          "[timeline][fuzz]") {
    ParseOutcome wrong_category;
    wrong_category.accepted = false;
    wrong_category.code = PersistenceErrorCode::InvalidJson;
    const auto miscategorized =
        judge_tightened_outcome("clips", 12, 11, wrong_category, EntryPoint::Project);
    REQUIRE(miscategorized);
    REQUIRE(miscategorized.kind == FindingKind::QuotaMisattribution);

    ParseOutcome no_overrun;
    no_overrun.accepted = false;
    no_overrun.code = PersistenceErrorCode::LimitExceeded;
    no_overrun.actual = 3;
    no_overrun.limit = 11;
    const auto unsupported =
        judge_tightened_outcome("clips", 12, 11, no_overrun, EntryPoint::Project);
    REQUIRE(unsupported);
    REQUIRE(unsupported.kind == FindingKind::QuotaMisattribution);
}

TEST_CASE("Timeline quota oracle reports disagreement between the scan and the decoded model",
          "[timeline][fuzz]") {
    StructureCensus walked;
    walked.notes = 4;
    StructureCensus scanned = walked;
    REQUIRE_FALSE(judge_path_divergence(walked, scanned));

    scanned.notes = 3;
    const auto finding = judge_path_divergence(walked, scanned);
    REQUIRE(finding);
    REQUIRE(finding.kind == FindingKind::PathDivergence);
    REQUIRE(finding.axis == "notes");
}

TEST_CASE("Timeline quota axes cover every declared structural ceiling", "[timeline][fuzz]") {
    // A ceiling with no axis is a quota nothing measures, which the tightening
    // sweep would then never exercise. The count is asserted so that adding a
    // structural ceiling to DecodeLimits without a matching axis fails here
    // rather than silently narrowing the sweep.
    REQUIRE(quota_axes().size() == 19);

    DecodeLimits limits;
    for (const auto& axis : quota_axes()) {
        REQUIRE_FALSE(axis.name.empty());
        REQUIRE(limits.*(axis.ceiling) > 0);
    }
}

// The harness against real documents. A seeded fixture must produce no finding:
// a harness that reports noise on the corpus it ships with is a harness nobody
// reads the output of.

TEST_CASE("Timeline seed corpus parses without a fuzz finding", "[timeline][fuzz]") {
    const auto corpus = seed_corpus();
    REQUIRE_FALSE(corpus.empty());

    InspectOptions options;
    for (const auto& entry : corpus) {
        const auto finding = inspect_all(entry.bytes, options);
        INFO("seed " << entry.label << ": " << format_finding(finding));
        REQUIRE_FALSE(finding);
    }
}

TEST_CASE("Timeline seed corpus enforces every structural ceiling it populates",
          "[timeline][fuzz]") {
    // The tightening sweep is the oracle that distinguishes a declared quota
    // from an enforced one, so it is asserted directly against the corpus
    // rather than only as part of the mutation replay.
    const auto corpus = seed_corpus();
    REQUIRE_FALSE(corpus.empty());

    std::size_t axes_exercised = 0;
    for (const auto& entry : corpus) {
        const auto sweep = sweep_quota_enforcement(entry.bytes, EntryPoint::Project);
        INFO("seed " << entry.label << ": " << format_finding(sweep.finding));
        REQUIRE_FALSE(sweep.finding);
        axes_exercised += sweep.axes_exercised;
    }

    // Every axis skipped is an axis this corpus does not prove is enforced. The
    // floor is asserted so that a corpus regression which empties the documents
    // shows up as a failure rather than as a sweep that quietly tests nothing.
    REQUIRE(axes_exercised >= 20);
}

TEST_CASE("Timeline document fuzzing finds no crash or quota or divergence finding",
          "[timeline][fuzz]") {
    const auto corpus = seed_corpus();
    REQUIRE_FALSE(corpus.empty());

    const auto seed = env_number("PULP_TIMELINE_FUZZ_SEED", 0x5EED'0000'0000'0001ULL);
    const auto cases = env_number("PULP_TIMELINE_FUZZ_CASES", 2'000);

    const CorpusMutator mutator(corpus, seed);
    InspectOptions options;
    // The per-axis tightening sweep multiplies the parse count by the number of
    // populated axes, which is affordable over the corpus but not over every
    // mutant. Mutants carry the crash, determinism, divergence, and admission
    // oracles; the sweep runs on the corpus above and in the scheduled lane.
    options.check_quota_enforcement = false;

    for (std::uint64_t index = 0; index < cases; ++index) {
        const auto mutant = mutator.generate(index);
        const auto finding = inspect_all(mutant.bytes, options);
        INFO("reproduce with PULP_TIMELINE_FUZZ_SEED="
             << seed << " case " << mutant.label << ": " << format_finding(finding));
        REQUIRE_FALSE(finding);
    }
}

TEST_CASE("Timeline fuzz mutants reach the document model rather than only the tokenizer",
          "[timeline][fuzz]") {
    // Exploration depth is the property a clean fuzz run cannot report on its
    // own: inputs the tokenizer rejects in their first bytes produce exactly
    // the same green result as inputs that exercise every structural quota.
    // Asserting a floor is what stops a mutator regression from silently
    // turning this lane into a JSON-syntax test.
    const auto corpus = seed_corpus();
    REQUIRE_FALSE(corpus.empty());

    const CorpusMutator mutator(corpus, 0x5EED'0000'0000'0001ULL);
    constexpr std::uint64_t kSampled = 2'000;

    std::size_t accepted = 0;
    std::size_t structural = 0;
    for (std::uint64_t index = 0; index < kSampled; ++index) {
        const auto mutant = mutator.generate(index);
        const auto outcome = observe_outcome(mutant.bytes, EntryPoint::Project);
        if (outcome.accepted) {
            ++accepted;
        } else if (is_structural_rejection(outcome.code)) {
            ++structural;
        }
    }

    INFO("accepted=" << accepted << " structural=" << structural << " of " << kSampled);
    REQUIRE(accepted + structural >= kSampled / 10);
    REQUIRE(accepted > 0);
}

TEST_CASE("Timeline fuzz mutants are reproducible from their seed and index", "[timeline][fuzz]") {
    const auto corpus = seed_corpus();
    REQUIRE_FALSE(corpus.empty());

    const CorpusMutator first(corpus, 12'345);
    const CorpusMutator second(corpus, 12'345);
    const CorpusMutator other(corpus, 12'346);

    bool any_differs = false;
    for (std::uint64_t index = 0; index < 32; ++index) {
        REQUIRE(first.generate(index).bytes == second.generate(index).bytes);
        REQUIRE(first.generate(index).label == second.generate(index).label);
        any_differs = any_differs || first.generate(index).bytes != other.generate(index).bytes;
    }
    // A mutator whose output ignored its seed would satisfy the equality above
    // while exploring exactly one document forever.
    REQUIRE(any_differs);
}
