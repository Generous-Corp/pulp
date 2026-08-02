#include "timeline_document_oracle.hpp"

#include <pulp/timeline/schema_registry.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <variant>

namespace pulp::test::timeline_fuzz {
namespace {

using timeline::DecodeLimits;
using timeline::PersistenceErrorCode;

/// Built-in schemas, constructed once.
///
/// The registry is immutable and shared, so a fuzz loop pays for it once
/// instead of once per input; registry construction would otherwise dominate
/// the measured cost of parsing a small document.
const timeline::SchemaRegistry& builtin_registry() {
    static const timeline::SchemaRegistry registry = [] {
        auto built = timeline::make_builtin_timeline_registry();
        // The built-in set is fixed at compile time and does not depend on the
        // input, so a failure here is a broken build rather than a finding.
        // Result::value() reads the union without checking, so the guard is
        // explicit: fuzzing every input against an unbuilt registry would
        // report a clean run while testing nothing.
        if (!built.has_value()) {
            std::fputs("timeline fuzz oracle: built-in schema registry failed to build\n", stderr);
            std::abort();
        }
        return std::move(built).value();
    }();
    return registry;
}

Finding quota_finding(FindingKind kind, EntryPoint entry, std::string_view axis,
                      std::uint64_t observed, std::uint64_t ceiling, std::string detail) {
    Finding finding;
    finding.kind = kind;
    finding.entry = entry;
    finding.axis = std::string(axis);
    finding.observed = observed;
    finding.ceiling = ceiling;
    finding.detail = std::move(detail);
    return finding;
}

std::uint64_t clip_note_count(const timeline::Clip& clip) {
    const auto* midi = std::get_if<timeline::MidiContent>(&clip.content());
    return midi == nullptr ? 0 : static_cast<std::uint64_t>(midi->notes().size());
}

void census_clip_midi(const timeline::Clip& clip, StructureCensus& census) {
    const auto* midi = std::get_if<timeline::MidiContent>(&clip.content());
    if (midi == nullptr) {
        return;
    }
    for (const auto& lane : midi->lanes()) {
        ++census.midi_lanes;
        census.midi_lane_points += static_cast<std::uint64_t>(lane.points.size());
    }
}

void census_track(const timeline::Track& track, StructureCensus& census) {
    ++census.tracks;
    for (const auto& clip : track.clips()) {
        ++census.clips;
        census.notes += clip_note_count(clip);
        census_clip_midi(clip, census);
    }
    census.device_placements += static_cast<std::uint64_t>(track.device_chain().size());
    for (const auto& lane : track.automation_lanes()) {
        ++census.automation_lanes;
        census.automation_points += static_cast<std::uint64_t>(lane.curve().points().size());
    }
    for (const auto& lane : track.take_lanes()) {
        ++census.take_lanes;
        census.takes += static_cast<std::uint64_t>(lane.takes().size());
        census.take_comp_segments += static_cast<std::uint64_t>(lane.comp_segments().size());
    }
}

void census_sequence(const timeline::Sequence& sequence, StructureCensus& census) {
    ++census.sequences;
    for (const auto& track : sequence.tracks()) {
        census_track(track, census);
    }
    census.markers += static_cast<std::uint64_t>(sequence.markers().size());
    census.regions += static_cast<std::uint64_t>(sequence.regions().size());
    for (const auto& scene : sequence.scenes()) {
        ++census.scenes;
        census.slots += static_cast<std::uint64_t>(scene.slots.size());
    }
    census.chord_scale_events +=
        static_cast<std::uint64_t>(sequence.chord_scale_lane().events().size());
    census.groove_steps += static_cast<std::uint64_t>(sequence.groove().steps().size());
}

ParseOutcome reduce(const runtime::Result<timeline::Project, timeline::PersistenceError>& result) {
    ParseOutcome outcome;
    outcome.accepted = result.has_value();
    if (!outcome.accepted) {
        outcome.code = result.error().code;
        outcome.actual = result.error().actual;
        outcome.limit = result.error().limit;
        outcome.path = result.error().path;
    }
    return outcome;
}

ParseOutcome
reduce(const runtime::Result<timeline::ProjectSnapshotSummary, timeline::PersistenceError>& result) {
    ParseOutcome outcome;
    outcome.accepted = result.has_value();
    if (!outcome.accepted) {
        outcome.code = result.error().code;
        outcome.actual = result.error().actual;
        outcome.limit = result.error().limit;
        outcome.path = result.error().path;
    }
    return outcome;
}

ParseOutcome reduce(
    const runtime::Result<std::vector<timeline::Command>, timeline::PersistenceError>& result) {
    ParseOutcome outcome;
    outcome.accepted = result.has_value();
    if (!outcome.accepted) {
        outcome.code = result.error().code;
        outcome.actual = result.error().actual;
        outcome.limit = result.error().limit;
        outcome.path = result.error().path;
    }
    return outcome;
}

/// One parse on one surface, reduced to an outcome and an optional census.
struct ParseObservation {
    ParseOutcome outcome;
    StructureCensus census;
    bool has_census = false;
};

ParseObservation observe(std::string_view input, EntryPoint entry, const DecodeLimits& limits) {
    ParseObservation observation;
    switch (entry) {
    case EntryPoint::Project: {
        auto result = timeline::deserialize_project(input, builtin_registry(), limits);
        observation.outcome = reduce(result);
        if (result.has_value()) {
            observation.census = census_of(result.value());
            observation.has_census = true;
        }
        break;
    }
    case EntryPoint::ProjectSummary: {
        auto result = timeline::peek_project_summary(input, builtin_registry(), limits);
        observation.outcome = reduce(result);
        if (result.has_value()) {
            observation.census = census_of(result.value().counts);
            observation.has_census = true;
        }
        break;
    }
    case EntryPoint::Commands: {
        auto result = timeline::deserialize_commands(input, builtin_registry(), limits);
        observation.outcome = reduce(result);
        break;
    }
    }
    return observation;
}

} // namespace

std::string_view describe(FindingKind kind) noexcept {
    switch (kind) {
    case FindingKind::None:
        return "none";
    case FindingKind::QuotaAdmission:
        return "quota-admission";
    case FindingKind::QuotaMisattribution:
        return "quota-misattribution";
    case FindingKind::QuotaNotEnforced:
        return "quota-not-enforced";
    case FindingKind::PathDivergence:
        return "path-divergence";
    case FindingKind::Nondeterminism:
        return "nondeterminism";
    case FindingKind::Amplification:
        return "amplification";
    case FindingKind::Deadline:
        return "deadline";
    }
    return "unknown";
}

std::string_view describe(EntryPoint entry) noexcept {
    switch (entry) {
    case EntryPoint::Project:
        return "deserialize_project";
    case EntryPoint::ProjectSummary:
        return "peek_project_summary";
    case EntryPoint::Commands:
        return "deserialize_commands";
    }
    return "unknown";
}

std::string format_finding(const Finding& finding) {
    std::string text(describe(finding.kind));
    text += " on ";
    text += describe(finding.entry);
    if (!finding.axis.empty()) {
        text += " axis=" + finding.axis;
    }
    text += " observed=" + std::to_string(finding.observed);
    text += " ceiling=" + std::to_string(finding.ceiling);
    if (!finding.detail.empty()) {
        text += " -- " + finding.detail;
    }
    return text;
}

std::uint64_t StructureCensus::total() const noexcept {
    return assets + sequences + tracks + clips + notes + device_placements + automation_lanes +
           automation_points + take_lanes + takes + take_comp_segments + markers + regions +
           scenes + slots + chord_scale_events + groove_steps + midi_lanes + midi_lane_points;
}

const std::vector<QuotaAxis>& quota_axes() {
    static const std::vector<QuotaAxis> axes = {
        {"assets", &StructureCensus::assets, &DecodeLimits::max_assets},
        {"sequences", &StructureCensus::sequences, &DecodeLimits::max_sequences},
        {"tracks", &StructureCensus::tracks, &DecodeLimits::max_tracks},
        {"clips", &StructureCensus::clips, &DecodeLimits::max_clips},
        {"notes", &StructureCensus::notes, &DecodeLimits::max_notes},
        {"device_placements", &StructureCensus::device_placements,
         &DecodeLimits::max_device_placements},
        {"automation_lanes", &StructureCensus::automation_lanes,
         &DecodeLimits::max_automation_lanes},
        {"automation_points", &StructureCensus::automation_points,
         &DecodeLimits::max_automation_points},
        {"take_lanes", &StructureCensus::take_lanes, &DecodeLimits::max_take_lanes},
        {"takes", &StructureCensus::takes, &DecodeLimits::max_takes},
        {"take_comp_segments", &StructureCensus::take_comp_segments,
         &DecodeLimits::max_take_comp_segments},
        {"markers", &StructureCensus::markers, &DecodeLimits::max_markers},
        {"regions", &StructureCensus::regions, &DecodeLimits::max_regions},
        {"scenes", &StructureCensus::scenes, &DecodeLimits::max_scenes},
        {"slots", &StructureCensus::slots, &DecodeLimits::max_slots},
        {"chord_scale_events", &StructureCensus::chord_scale_events,
         &DecodeLimits::max_chord_scale_events},
        {"groove_steps", &StructureCensus::groove_steps, &DecodeLimits::max_groove_steps},
        {"midi_lanes", &StructureCensus::midi_lanes, &DecodeLimits::max_midi_lanes},
        {"midi_lane_points", &StructureCensus::midi_lane_points,
         &DecodeLimits::max_midi_lane_points},
    };
    return axes;
}

StructureCensus census_of(const timeline::Project& project) {
    StructureCensus census;
    census.assets = static_cast<std::uint64_t>(project.assets().size());
    for (const auto& sequence : project.sequences()) {
        census_sequence(sequence, census);
    }
    return census;
}

StructureCensus census_of(const timeline::ProjectSnapshotCounts& counts) {
    StructureCensus census;
    census.assets = counts.assets;
    census.sequences = counts.sequences;
    census.tracks = counts.tracks;
    census.clips = counts.clips;
    census.notes = counts.notes;
    census.device_placements = counts.device_placements;
    census.automation_lanes = counts.automation_lanes;
    census.automation_points = counts.automation_points;
    census.take_lanes = counts.take_lanes;
    census.takes = counts.takes;
    census.take_comp_segments = counts.take_comp_segments;
    census.markers = counts.markers;
    census.regions = counts.regions;
    census.scenes = counts.scenes;
    census.slots = counts.slots;
    census.chord_scale_events = counts.chord_scale_events;
    census.groove_steps = counts.groove_steps;
    census.midi_lanes = counts.midi_lanes;
    census.midi_lane_points = counts.midi_lane_points;
    return census;
}

ParseOutcome observe_outcome(std::string_view input, EntryPoint entry,
                             const DecodeLimits& limits) {
    return observe(input, entry, limits).outcome;
}

bool is_structural_rejection(PersistenceErrorCode code) noexcept {
    switch (code) {
    case PersistenceErrorCode::InvalidJson:
    case PersistenceErrorCode::InvalidUtf8:
        // The tokenizer refused the bytes; no Timeline structure was reached.
        return false;
    default:
        return true;
    }
}

Finding judge_quota_admission(const StructureCensus& census, const DecodeLimits& limits,
                              EntryPoint entry) {
    for (const auto& axis : quota_axes()) {
        const auto observed = census.*(axis.count);
        const auto ceiling = static_cast<std::uint64_t>(limits.*(axis.ceiling));
        if (observed > ceiling) {
            return quota_finding(FindingKind::QuotaAdmission, entry, axis.name, observed, ceiling,
                                 "admitted a document whose structure exceeds a declared quota");
        }
    }
    return {};
}

Finding judge_tightened_outcome(std::string_view axis, std::uint64_t observed,
                                std::uint64_t tightened_ceiling, const ParseOutcome& outcome,
                                EntryPoint entry) {
    if (outcome.accepted) {
        return quota_finding(FindingKind::QuotaNotEnforced, entry, axis, observed,
                             tightened_ceiling,
                             "ceiling lowered below the document's own count still admitted it");
    }
    if (outcome.code != PersistenceErrorCode::LimitExceeded) {
        // A document that parsed under the baseline limits, re-parsed with a
        // single ceiling lowered, can only newly fail on that quota. Any other
        // category means the rejection is not attributable to the quota and
        // the quota's own enforcement remains unproven.
        return quota_finding(FindingKind::QuotaMisattribution, entry, axis, observed,
                             tightened_ceiling,
                             "lowered ceiling rejected with a non-quota error category");
    }
    if (outcome.actual <= outcome.limit) {
        return quota_finding(FindingKind::QuotaMisattribution, entry, axis, outcome.actual,
                             outcome.limit, "quota rejection does not describe an overrun");
    }
    return {};
}

Finding judge_path_divergence(const StructureCensus& walked, const StructureCensus& scanned) {
    for (const auto& axis : quota_axes()) {
        const auto from_model = walked.*(axis.count);
        const auto from_scan = scanned.*(axis.count);
        if (from_model != from_scan) {
            return quota_finding(FindingKind::PathDivergence, EntryPoint::ProjectSummary, axis.name,
                                 from_model, from_scan,
                                 "decoded model and structural scan disagree on an axis");
        }
    }
    return {};
}

Finding inspect(std::string_view input, EntryPoint entry, const InspectOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    const auto baseline = observe(input, entry, options.limits);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    if (elapsed > options.deadline) {
        Finding finding;
        finding.kind = FindingKind::Deadline;
        finding.entry = entry;
        finding.observed =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        finding.ceiling = static_cast<std::uint64_t>(options.deadline.count());
        finding.detail = "single input exceeded the per-input wall-clock budget";
        return finding;
    }

    if (!baseline.outcome.accepted) {
        // Rejection is the correct response to malformed input. The only thing
        // to check is that the rejection is one the quota contract can express.
        if (baseline.outcome.code == PersistenceErrorCode::LimitExceeded &&
            baseline.outcome.actual <= baseline.outcome.limit) {
            return quota_finding(FindingKind::QuotaMisattribution, entry, "", baseline.outcome.actual,
                                 baseline.outcome.limit,
                                 "quota rejection does not describe an overrun");
        }
        return {};
    }

    if (baseline.has_census) {
        if (auto finding = judge_quota_admission(baseline.census, options.limits, entry)) {
            return finding;
        }

        const auto items = baseline.census.total();
        const auto budget = static_cast<std::uint64_t>(input.size()) * options.max_items_per_byte;
        if (items > budget) {
            return quota_finding(FindingKind::Amplification, entry, "items", items, budget,
                                 "accepted structure is disproportionate to the input size");
        }
    }

    // A decoded Project must also survive the structural scan, and the two
    // must agree: the scan reads the source bytes and the walk reads the
    // constructed model, so a counter that is never incremented on one path
    // shows up here rather than being confirmed by itself.
    if (entry == EntryPoint::Project && baseline.has_census) {
        const auto scan = observe(input, EntryPoint::ProjectSummary, options.limits);
        if (!scan.outcome.accepted) {
            Finding finding;
            finding.kind = FindingKind::PathDivergence;
            finding.entry = EntryPoint::ProjectSummary;
            finding.detail = "document decoded to a Project but failed the structural scan";
            return finding;
        }
        if (auto finding = judge_path_divergence(baseline.census, scan.census)) {
            return finding;
        }
    }

    if (options.check_determinism) {
        const auto repeat = observe(input, entry, options.limits);
        if (!(repeat.outcome == baseline.outcome)) {
            Finding finding;
            finding.kind = FindingKind::Nondeterminism;
            finding.entry = entry;
            finding.detail = "the same bytes produced two different outcomes";
            return finding;
        }
    }

    if (options.check_quota_enforcement) {
        return sweep_quota_enforcement(input, entry, options).finding;
    }

    return {};
}

EnforcementSweep sweep_quota_enforcement(std::string_view input, EntryPoint entry,
                                         const InspectOptions& options) {
    EnforcementSweep sweep;
    const auto baseline = observe(input, entry, options.limits);
    if (!baseline.outcome.accepted || !baseline.has_census) {
        return sweep;
    }

    for (const auto& axis : quota_axes()) {
        const auto observed = baseline.census.*(axis.count);
        if (observed == 0) {
            // Nothing to bound: a ceiling of zero over zero items is not a
            // violation, so this axis carries no evidence for this document.
            continue;
        }
        ++sweep.axes_exercised;
        auto tightened = options.limits;
        tightened.*(axis.ceiling) = static_cast<std::size_t>(observed - 1);
        const auto retried = observe(input, entry, tightened);
        sweep.finding =
            judge_tightened_outcome(axis.name, observed, observed - 1, retried.outcome, entry);
        if (sweep.finding) {
            return sweep;
        }
    }
    return sweep;
}

Finding inspect_all(std::string_view input, const InspectOptions& options) {
    for (const auto entry :
         {EntryPoint::Project, EntryPoint::ProjectSummary, EntryPoint::Commands}) {
        if (auto finding = inspect(input, entry, options)) {
            return finding;
        }
    }
    return {};
}

} // namespace pulp::test::timeline_fuzz
