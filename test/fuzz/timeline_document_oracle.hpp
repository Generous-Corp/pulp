#pragma once

/// Oracles for untrusted Timeline document parsing.
///
/// A parser rejecting malformed input is correct behaviour, so "did not crash"
/// is far too weak an oracle for a surface whose defining property is that it
/// admits documents only within declared quotas. What this header defines is
/// the set of properties that must hold for *every* input, well-formed or not,
/// so that a violation is a finding and a rejection is not.
///
/// The judgements are deliberately separated from the parse calls that feed
/// them: `judge_*` are pure functions over an already-observed outcome. That
/// separation is what makes the oracle falsifiable — a test can hand a
/// `judge_*` a synthetic over-quota outcome and require that it fires, without
/// needing a real defect in the parser. An oracle that can only be exercised
/// by a real bug reports "no findings" forever whether or not it works.

#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::test::timeline_fuzz {

/// Untrusted-document parse surface under test.
enum class EntryPoint : std::uint8_t {
    /// `deserialize_project` — full decode to a validated Project.
    Project,
    /// `peek_project_summary` — structural scan that never constructs a Project.
    ProjectSummary,
    /// `deserialize_commands` — the command-envelope array applied by agents.
    Commands,
};

/// Property that an input violated.
enum class FindingKind : std::uint8_t {
    /// No property was violated.
    None,
    /// A document was accepted whose measured structure exceeds a declared quota.
    QuotaAdmission,
    /// A LimitExceeded rejection did not describe an actual overrun.
    QuotaMisattribution,
    /// A quota tightened below the document's own measured count still admitted it.
    QuotaNotEnforced,
    /// Two parse paths disagreed about the same document.
    PathDivergence,
    /// The same bytes produced two different outcomes.
    Nondeterminism,
    /// Accepted structure is disproportionate to the input size.
    Amplification,
    /// A single input exceeded the per-input wall-clock budget.
    Deadline,
};

/// One violated property, with the evidence that identifies it.
struct Finding {
    /// Property that was violated.
    FindingKind kind = FindingKind::None;
    /// Parse surface on which it was observed.
    EntryPoint entry = EntryPoint::Project;
    /// Quota axis name, for quota-related kinds.
    std::string axis;
    /// Measured count or size.
    std::uint64_t observed = 0;
    /// Ceiling that applied.
    std::uint64_t ceiling = 0;
    /// Human-readable evidence.
    std::string detail;

    /// Returns whether a property was violated.
    explicit operator bool() const noexcept {
        return kind != FindingKind::None;
    }
};

/// Returns a stable name for a finding kind.
std::string_view describe(FindingKind kind) noexcept;
/// Returns a stable name for a parse surface.
std::string_view describe(EntryPoint entry) noexcept;
/// Renders a finding as a single diagnostic line.
std::string format_finding(const Finding& finding);

/// Structural item counts measured by walking a decoded Project.
///
/// Field-for-field parallel to `timeline::ProjectSnapshotCounts`, which is
/// computed by an independent scan of the source bytes. Comparing the two is
/// what makes the differential oracle meaningful: a census derived from the
/// same counters the parser used to enforce its quotas could not detect a
/// counter that was never incremented.
struct StructureCensus {
    std::uint64_t assets = 0;             ///< Media assets.
    std::uint64_t sequences = 0;          ///< Sequences.
    std::uint64_t tracks = 0;             ///< Tracks across all sequences.
    std::uint64_t clips = 0;              ///< Clips across all tracks.
    std::uint64_t notes = 0;              ///< Note events across MIDI content.
    std::uint64_t device_placements = 0;  ///< Device placements across all tracks.
    std::uint64_t automation_lanes = 0;   ///< Automation lanes across all tracks.
    std::uint64_t automation_points = 0;  ///< Automation points across all lanes.
    std::uint64_t modulators = 0;         ///< Modulators across all tracks.
    std::uint64_t macro_controls = 0;     ///< Macro controls across all tracks.
    std::uint64_t modulation_routes = 0;  ///< Modulation routes across all tracks.
    std::uint64_t take_lanes = 0;         ///< Take lanes across all tracks.
    std::uint64_t takes = 0;              ///< Takes across all take lanes.
    std::uint64_t take_comp_segments = 0; ///< Selected comp segments across all lanes.
    std::uint64_t markers = 0;            ///< Sequence markers.
    std::uint64_t regions = 0;            ///< Sequence regions.
    std::uint64_t scenes = 0;             ///< Launch scenes.
    std::uint64_t slots = 0;              ///< Launch slots across all scenes.
    std::uint64_t chord_scale_events = 0; ///< Chord/scale context events.
    std::uint64_t groove_steps = 0;       ///< Groove-template steps.
    std::uint64_t midi_lanes = 0;         ///< Controller/expression lanes.
    std::uint64_t midi_lane_points = 0;   ///< Controller points across all lanes.

    /// Returns the total counted items, used for the amplification ratio.
    std::uint64_t total() const noexcept;

    constexpr bool operator==(const StructureCensus&) const noexcept = default;
};

/// Binds one census axis to the `DecodeLimits` field that bounds it.
///
/// Data-driven so that adding a quota to `DecodeLimits` is one row here rather
/// than an edit spread across the admission, tightening, and divergence checks.
struct QuotaAxis {
    /// Stable axis name used in findings.
    std::string_view name;
    /// Measured count on a census.
    std::uint64_t StructureCensus::*count;
    /// Declared ceiling on the limits object.
    std::size_t timeline::DecodeLimits::*ceiling;
};

/// Returns every census axis paired with its declared ceiling.
const std::vector<QuotaAxis>& quota_axes();

/// Counts a decoded Project's structure by walking the model.
StructureCensus census_of(const timeline::Project& project);
/// Converts an independently scanned summary into the same census shape.
StructureCensus census_of(const timeline::ProjectSnapshotCounts& counts);

/// Judges whether an accepted document's structure fits its declared quotas.
///
/// Fires `QuotaAdmission` when any axis of an admitted document exceeds the
/// ceiling that was in force — a limit exceeded without the typed error the
/// limit exists to produce.
Finding judge_quota_admission(const StructureCensus& census, const timeline::DecodeLimits& limits,
                              EntryPoint entry);

/// Outcome of one parse, reduced to what the oracles need to judge it.
struct ParseOutcome {
    /// Whether the parse admitted the document.
    bool accepted = false;
    /// Failure category when rejected.
    timeline::PersistenceErrorCode code = timeline::PersistenceErrorCode::InvalidJson;
    /// Observed size or count reported with a quota rejection.
    std::uint64_t actual = 0;
    /// Ceiling reported with a quota rejection.
    std::uint64_t limit = 0;
    /// Diagnostic path reported with the rejection.
    std::string path;

    constexpr bool operator==(const ParseOutcome& other) const noexcept {
        return accepted == other.accepted && code == other.code && actual == other.actual &&
               limit == other.limit && path == other.path;
    }
};

/// Judges a re-parse whose ceiling on `axis` was lowered below the document's
/// own measured count.
///
/// The document is known to parse under the baseline limits, and exactly one
/// ceiling was changed, so admission here means that ceiling is declared but
/// not enforced. A `LimitExceeded` whose `actual` does not exceed its `limit`
/// is not evidence of enforcement either, and is reported separately.
Finding judge_tightened_outcome(std::string_view axis, std::uint64_t observed,
                                std::uint64_t tightened_ceiling, const ParseOutcome& outcome,
                                EntryPoint entry);

/// Judges whether the structural scan and the full decode agree.
Finding judge_path_divergence(const StructureCensus& walked, const StructureCensus& scanned);

/// Knobs for one oracle sweep.
struct InspectOptions {
    /// Quotas in force for the baseline parse.
    timeline::DecodeLimits limits{};
    /// Per-input wall-clock budget. Exceeding it is a `Deadline` finding.
    std::chrono::milliseconds deadline{5'000};
    /// Whether to run the per-axis quota-tightening sweep.
    ///
    /// The sweep costs one extra parse per non-empty axis, so a bounded replay
    /// enables it and a high-throughput fuzz loop samples it.
    bool check_quota_enforcement = true;
    /// Whether to run the second parse that proves the outcome is deterministic.
    bool check_determinism = true;
    /// Maximum accepted items per input byte before growth is disproportionate.
    std::uint64_t max_items_per_byte = 64;
};

/// Parses `input` on one surface and reduces the result to its outcome.
///
/// Exposed so a driver can measure how deep into the document model its inputs
/// actually reach. A mutation corpus that the tokenizer rejects in its first
/// bytes produces a green run while exercising almost none of the parser, and
/// nothing in a clean result distinguishes that from real coverage.
ParseOutcome observe_outcome(std::string_view input, EntryPoint entry,
                             const timeline::DecodeLimits& limits = {});

/// Returns whether a rejection came from the document model rather than the
/// tokenizer — that is, whether the input survived far enough to be judged as
/// a Timeline document at all.
bool is_structural_rejection(timeline::PersistenceErrorCode code) noexcept;

/// Result of the per-axis quota-tightening sweep.
struct EnforcementSweep {
    /// First violated property, if any.
    Finding finding;
    /// Axes the document actually populated, and so was re-parsed against.
    ///
    /// Reported because a sweep that skipped every axis proves nothing while
    /// still returning no finding — the count is what separates "enforced" from
    /// "never tested".
    std::size_t axes_exercised = 0;
};

/// Lowers each populated ceiling below the document's own count and requires
/// the document to be rejected.
///
/// The document must already parse under `options.limits`; the caller is
/// responsible for that precondition, since a document that fails the baseline
/// parse carries no counts to tighten against.
EnforcementSweep sweep_quota_enforcement(std::string_view input, EntryPoint entry,
                                         const InspectOptions& options = {});

/// Runs every oracle for one surface over one input, returning the first violation.
Finding inspect(std::string_view input, EntryPoint entry, const InspectOptions& options = {});

/// Runs `inspect` across every parse surface, returning the first violation.
Finding inspect_all(std::string_view input, const InspectOptions& options = {});

} // namespace pulp::test::timeline_fuzz
