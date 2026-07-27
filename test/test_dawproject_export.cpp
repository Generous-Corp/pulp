#include <catch2/catch_test_macros.hpp>

#include <pulp/dawproject/dawproject_export.hpp>
#include <pulp/interchange/census.hpp>
#include <pulp/interchange/export_plan.hpp>
#include <pulp/timeline/dawproject_import.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::timeline;
using namespace pulp::timebase;
using pulp::interchange::Concept;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

constexpr std::int64_t kQuarter = kTicksPerQuarter;

/// A document inside the writer's bounded subset: flat tracks, beat-anchored
/// clips, inline notes, one tempo, one meter. Nothing here is lost, so the plan
/// must report lossless and run_export must proceed without consent.
Project note_project() {
    auto notes = take(NoteContent::create({
        NoteEvent{{20}, {0}, {kQuarter}, 40'000, 60, 0},
        NoteEvent{{21}, {2 * kQuarter}, {kQuarter}, 52'000, 67, 1},
    }));
    auto clip = take(Clip::create({10}, {kQuarter}, {4 * kQuarter}, std::move(notes)));
    auto track = take(Track::create({5}, "lead", {clip}));
    auto sequence =
        take(Sequence::create({3}, "arrangement", TickDuration{16 * kQuarter}, {track}));
    ProjectInput input{{1}, "exported", 40, {3}, {}, {sequence}};
    input.tempo_map = take(TempoMap::create(std::array{TempoPoint{{0}, 140.0}}));
    input.meter_map = take(MeterMap::create(std::array{MeterPoint{{0}, {3, 4}}}));
    return take(Project::create(std::move(input)));
}

std::string export_xml(const Project& project, const std::vector<Concept>& accept = {}) {
    const auto plan = interchange::plan_export(project, interchange::Format::DawProject);
    interchange::ExportOptions options;
    options.accepted_losses = accept;
    auto artifacts = interchange::run_export(plan, options, dawproject::writer(project));
    REQUIRE(artifacts.has_value());
    for (const auto& artifact : artifacts.value().artifacts)
        if (artifact.name == "project.xml")
            return std::string(artifact.bytes.begin(), artifact.bytes.end());
    FAIL("no project.xml artifact");
    return {};
}

struct FlatNote {
    std::int64_t start = 0;
    std::int64_t duration = 0;
    int pitch = 0;
    int channel = 0;
    bool operator==(const FlatNote&) const = default;
};

/// Note identity is not preserved across the format (DAWproject notes carry no
/// id), so compare the musical facts a listener would hear rather than ids.
std::vector<FlatNote> flatten_notes(const Project& project) {
    std::vector<FlatNote> out;
    const auto* sequence = project.find_sequence(project.root_sequence_id());
    REQUIRE(sequence != nullptr);
    for (const Track& track : sequence->tracks())
        for (const Clip& clip : track.clips())
            if (const auto* notes = std::get_if<NoteContent>(&clip.content()))
                for (const NoteEvent& note : notes->notes())
                    out.push_back({clip.start().value + note.start.value, note.duration.value,
                                   note.pitch, note.channel});
    std::sort(out.begin(), out.end(), [](const FlatNote& a, const FlatNote& b) {
        return std::tie(a.start, a.pitch) < std::tie(b.start, b.pitch);
    });
    return out;
}

} // namespace

TEST_CASE("a bounded document exports and re-imports with its music intact",
          "[interchange][dawproject][export]") {
    const Project original = note_project();

    const auto plan = interchange::plan_export(original, interchange::Format::DawProject);
    REQUIRE(plan.is_lossless());
    REQUIRE(plan.required_consent().empty());

    const std::string xml = export_xml(original);
    REQUIRE(xml.find("<Project") != std::string::npos);
    // Printed so a reviewer (and the Bitwig smoke) can see exactly what the
    // writer emits without rebuilding a harness.
    INFO("project.xml:\n" << xml);
    CHECK(!xml.empty());

    const Project reimported = take(import_dawproject_xml(xml));

    // The musical content survives the round trip.
    REQUIRE(flatten_notes(reimported) == flatten_notes(original));
    REQUIRE(reimported.tempo_map().points().front().bpm ==
            original.tempo_map().points().front().bpm);
    REQUIRE(reimported.meter_map().points().front().signature ==
            original.meter_map().points().front().signature);

    // And the census agrees concept-for-concept, which is the check that a
    // writer bug cannot hide behind an assertion about one field.
    const auto before = interchange::census(original);
    const auto after = interchange::census(reimported);
    for (Concept concept_value : before.present())
        REQUIRE(after.contains(concept_value));
    REQUIRE(after.count(Concept::ClipNote) == before.count(Concept::ClipNote));
    REQUIRE(after.count(Concept::TrackFlat) == before.count(Concept::TrackFlat));
}

TEST_CASE("the round-trip comparison detects a mutated export",
          "[interchange][dawproject][export]") {
    // NEGATIVE CONTROL for the test above. A round-trip assertion can pass
    // vacuously if the comparison is too loose -- if this mutation still
    // compared equal, the previous test would prove nothing.
    const Project original = note_project();
    std::string xml = export_xml(original);

    const auto note = xml.find("<Note ");
    REQUIRE(note != std::string::npos);
    const auto note_end = xml.find("/>", note);
    REQUIRE(note_end != std::string::npos);
    xml.erase(note, note_end + 2 - note);

    const Project mutated = take(import_dawproject_xml(xml));
    REQUIRE(flatten_notes(mutated) != flatten_notes(original));
}

TEST_CASE("an export refuses a loss the caller has not accepted",
          "[interchange][dawproject][export]") {
    // Markers are model-observable and this writer does not emit them, so a
    // marker-bearing document must not export silently.
    auto sequence = take(Sequence::create(SequenceInput{
        .id = {3},
        .name = "annotated",
        .musical_duration = TickDuration{16 * kQuarter},
        .tracks = {take(Track::create({5}, "lead", {}))},
        .markers = {SequenceMarker{{30}, "verse", TickPosition{0}, {}}},
    }));
    ProjectInput input{{1}, "annotated", 40, {3}, {}, {sequence}};
    const Project project = take(Project::create(std::move(input)));

    const auto plan = interchange::plan_export(project, interchange::Format::DawProject);
    REQUIRE_FALSE(plan.is_lossless());
    const auto consent = plan.required_consent();
    REQUIRE(std::find(consent.begin(), consent.end(), Concept::Marker) != consent.end());

    // Without consent: refused before the writer is ever called.
    interchange::ExportOptions none;
    auto refused = interchange::run_export(plan, none, dawproject::writer(project));
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().code == interchange::ExportErrorCode::UnacceptedLoss);

    // With consent: it proceeds, and the manifest travels with the artifact
    // naming what was dropped.
    interchange::ExportOptions accepted;
    accepted.accepted_losses = consent;
    auto exported = interchange::run_export(plan, accepted, dawproject::writer(project));
    REQUIRE(exported.has_value());
    bool found_manifest = false;
    for (const auto& artifact : exported.value().artifacts) {
        if (artifact.name != "pulp-loss-manifest.json")
            continue;
        found_manifest = true;
        const std::string json(artifact.bytes.begin(), artifact.bytes.end());
        REQUIRE(json.find("\"marker\"") != std::string::npos);
        REQUIRE(json.find("\"lossless\":false") != std::string::npos);
    }
    REQUIRE(found_manifest);
}
