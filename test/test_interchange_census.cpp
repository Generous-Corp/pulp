#include <catch2/catch_test_macros.hpp>

#include <pulp/interchange/census.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace pulp::interchange;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

template <typename T> T take_value(pulp::runtime::Result<T, ModelError> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

ContentHash content_hash(char digit = 'a') {
    return *ContentHash::from_hex(std::string(64, digit));
}

MediaAsset asset(ItemId id, AssetStoragePolicy policy, char digit) {
    return MediaAsset{id, "audio.wav", 1'000, {48'000, 1}, content_hash(digit), policy, {}, {}, {}};
}

// A document that uses every concept the walker can observe, so a census over it
// is a complete statement of what the model can express today.
Project rich_project() {
    auto note_clip = take_value(
        Clip::create({5}, {200}, {100}, take_value(MidiContent::create({{{8}, {20}, {10}, 0x8000, 64, 1}}))));
    auto media_clip = take_value(Clip::create({4}, {0}, {100}, MediaRef{{11}, {25}, 100},
                                              ClipPlaybackProperties{0.5f, 16, 32}));
    auto nested_clip =
        take_value(Clip::create({13}, {400}, {100}, SequenceRef{{7}, {0}}));
    auto absolute = take_value(
        Clip::create_absolute({9}, {0}, 480, {48'000, 1}, MediaRef{{12}, {0}, 480}));

    // A track anchors all of its clips the same way, so the sample-anchored
    // clip needs its own.
    auto musical_track =
        take_value(Track::create({6}, "musical", {note_clip, media_clip, nested_clip}));
    auto absolute_track = take_value(Track::create({10}, "absolute", {absolute}));
    auto lane = take_value(ChordScaleLane::create({}));
    auto sequence = take_value(Sequence::create(SequenceInput{
        .id = {3},
        .name = "sequence",
        .musical_duration = TickDuration{4'000},
        .tracks = {musical_track, absolute_track},
        .chord_scale_lane = std::move(lane),
        .scenes = {Scene{{16}, "launcher", {Slot{{17}, {4}, launch_immediate(), {}}}}},
    }));
    auto other = take_value(Sequence::create({7}, "other", TickDuration{100}, {}));

    return take_value(Project::create(ProjectInput{{1},
                                                   "project",
                                                   20,
                                                   {3},
                                                   {asset({11}, AssetStoragePolicy::External, 'a'),
                                                    asset({12}, AssetStoragePolicy::Embedded, 'b')},
                                                   {sequence, other}}));
}

bool has(const std::vector<Concept>& present, Concept concept_value) {
    return std::find(present.begin(), present.end(), concept_value) != present.end();
}

} // namespace

TEST_CASE("a census records what a document uses, and only that", "[interchange]") {
    const Project project = rich_project();
    const ConceptCensus counted = census(project);
    const std::vector<Concept> present = counted.present();

    SECTION("clip anchors, content, gain, and fades are each observed separately") {
        REQUIRE(counted.count(Concept::ClipMusical) == 3);
        REQUIRE(counted.count(Concept::ClipAbsolute) == 1);
        REQUIRE(counted.count(Concept::ClipNote) == 1);
        REQUIRE(counted.count(Concept::ClipMedia) == 2);
        REQUIRE(counted.count(Concept::ClipMediaWindow) == 2);
        REQUIRE(counted.count(Concept::SequenceNested) == 1);
        REQUIRE(counted.count(Concept::ClipGain) == 1);
        REQUIRE(counted.count(Concept::ClipFades) == 1);
    }

    SECTION("assets are counted by identity and by whether their bytes travel") {
        REQUIRE(counted.count(Concept::AssetSealedHash) == 2);
        REQUIRE(counted.count(Concept::AssetReferencedMedia) == 1);
        REQUIRE(counted.count(Concept::AssetEmbeddedMedia) == 1);
    }

    SECTION("a constant tempo is not a tempo map") {
        REQUIRE(counted.contains(Concept::TempoSingle));
        REQUIRE_FALSE(counted.contains(Concept::TempoMap));
        REQUIRE(counted.contains(Concept::MeterSingle));
        REQUIRE_FALSE(counted.contains(Concept::MeterMap));
    }

    SECTION("structure the document does have is present") {
        REQUIRE(has(present, Concept::TrackFlat));
        REQUIRE(has(present, Concept::SequenceMultiple));
        REQUIRE(counted.count(Concept::ClipLaunch) == 1);
    }

    SECTION("concepts the document does not use are absent") {
        // Marker and TimecodeOrigin ARE expressible in the model; this fixture
        // simply carries none. They belong here rather than in the
        // model-cannot-express section below.
        REQUIRE_FALSE(counted.contains(Concept::Marker));
        REQUIRE_FALSE(counted.contains(Concept::TimecodeOrigin));
        REQUIRE_FALSE(counted.contains(Concept::ClipNoteModifier));
        REQUIRE_FALSE(counted.contains(Concept::TakeLane));
        REQUIRE_FALSE(counted.contains(Concept::TrackFreeze));
        REQUIRE_FALSE(counted.contains(Concept::DevicePlacement));
        REQUIRE_FALSE(counted.contains(Concept::AutomationDeviceParam));
        REQUIRE_FALSE(counted.contains(Concept::ContentOpaque));
    }

    SECTION("a census never claims a concept the model cannot express") {
        // The walker can only find what the document can hold. Concepts that
        // exist solely in some interchange format must never appear, or a plan
        // would report losing something that was never there.
        for (Concept concept_value : present)
            REQUIRE(concept_detectable_in_model(concept_value));
        REQUIRE_FALSE(counted.contains(Concept::Unknown));
        REQUIRE_FALSE(counted.contains(Concept::ClipCrossfade));
        REQUIRE_FALSE(counted.contains(Concept::MixerTrackGain));
    }

    SECTION("owners name the items the concept was found on") {
        const auto owners = counted.owners(Concept::ClipGain);
        REQUIRE(owners.size() == 1);
        REQUIRE(owners[0] == ItemId{4});
        const auto launch_owners = counted.owners(Concept::ClipLaunch);
        REQUIRE(launch_owners.size() == 1);
        REQUIRE(launch_owners[0] == ItemId{16});
    }
}

TEST_CASE("media windows are census-visible only when they select an asset subrange",
          "[interchange][census]") {
    auto full = take_value(Clip::create({4}, {0}, {100}, MediaRef{{11}, {0}, 1'000}));
    auto offset = take_value(Clip::create({5}, {100}, {100}, MediaRef{{11}, {25}, 975}));
    auto partial = take_value(Clip::create({6}, {200}, {100}, MediaRef{{11}, {0}, 500}));
    auto track = take_value(Track::create({7}, "media", {full, offset, partial}));
    auto sequence =
        take_value(Sequence::create({3}, "sequence", TickDuration{400}, {track}));
    const Project project = take_value(Project::create(ProjectInput{
        {1}, "project", 20, {3}, {asset({11}, AssetStoragePolicy::External, 'a')}, {sequence}}));

    const ConceptCensus counted = census(project);
    REQUIRE(counted.count(Concept::ClipMedia) == 3);
    REQUIRE(counted.count(Concept::ClipMediaWindow) == 2);
    const auto owners = counted.owners(Concept::ClipMediaWindow);
    REQUIRE(owners.size() == 2);
    REQUIRE(owners[0] == ItemId{5});
    REQUIRE(owners[1] == ItemId{6});
}

TEST_CASE("a census bounds the evidence it keeps without understating it", "[interchange]") {
    ConceptCensus counted;
    const CensusLimits limits{3};
    for (std::uint64_t id = 1; id <= 10; ++id)
        counted.record(Concept::ClipMedia, ItemId{id}, limits);

    // The count is what the document actually contains; the owner list is a
    // sample. A manifest over a huge document stays bounded but stays honest.
    REQUIRE(counted.count(Concept::ClipMedia) == 10);
    REQUIRE(counted.owners(Concept::ClipMedia).size() == 3);
}

TEST_CASE("an empty document still states its tempo and meter", "[interchange]") {
    auto sequence = take_value(Sequence::create({3}, "sequence", TickDuration{100}, {}));
    const Project project =
        take_value(Project::create(ProjectInput{{1}, "project", 10, {3}, {}, {sequence}}));
    const ConceptCensus counted = census(project);

    REQUIRE_FALSE(counted.empty());
    REQUIRE_FALSE(counted.contains(Concept::TrackFlat));
    REQUIRE_FALSE(counted.contains(Concept::SequenceMultiple));
    REQUIRE(counted.contains(Concept::TempoSingle));
}

TEST_CASE("a chord/scale context lane is recorded by the census", "[interchange][census]") {
    auto follower = take_value(Track::create({3}, "track", {}));
    auto lane = take_value(ChordScaleLane::create(
        {ChordScaleEvent{{0}, ChordQuality::Minor7, 9, ScaleMode::Dorian, 9}}));
    auto sequence =
        take_value(Sequence::create({2}, "root", std::nullopt, std::nullopt, {follower}, {}, {},
                                    std::move(lane)));
    auto project =
        take_value(Project::create(ProjectInput{{1}, "project", 4, {2}, {}, {std::move(sequence)}}));

    const auto recorded = census(project);
    REQUIRE(recorded.contains(Concept::ContextChordScale));
    REQUIRE(recorded.count(Concept::ContextChordScale) == 1);
    REQUIRE(recorded.owners(Concept::ContextChordScale)[0] == ItemId{2});

    // An empty lane is the absence of harmony, not a concept an export loses.
    auto without = take_value(Sequence::create({2}, "root", std::nullopt, {follower}));
    auto plain =
        take_value(Project::create(ProjectInput{{1}, "project", 4, {2}, {}, {std::move(without)}}));
    REQUIRE_FALSE(census(plain).contains(Concept::ContextChordScale));
}

TEST_CASE("a census records authored groove state even when it currently sounds straight",
          "[interchange][census]") {
    GrooveTemplateInput authored;
    authored.name = "saved for later";
    authored.swing = kTripletSwing;
    authored.timing_strength = 500;
    auto groove = take_value(GrooveTemplate::create(std::move(authored)));
    REQUIRE(groove.states_no_feel());
    REQUIRE_FALSE(groove.is_canonical_default());

    auto sequence = take_value(Sequence::create(
        SequenceInput{.id = {2}, .name = "root", .musical_duration = TickDuration{100},
                      .groove = std::move(groove)}));
    auto project = take_value(
        Project::create(ProjectInput{{1}, "project", 3, {2}, {}, {std::move(sequence)}}));

    const auto recorded = census(project);
    REQUIRE(recorded.contains(Concept::ContextGroove));
    REQUIRE(recorded.count(Concept::ContextGroove) == 1);
    REQUIRE(recorded.owners(Concept::ContextGroove)[0] == ItemId{2});
}

TEST_CASE("a census records per-note modifiers so an export cannot drop them silently",
          "[interchange]") {
    NoteModifier chance;
    chance.note_id = {8};
    chance.probability = 0x4000;
    auto content = take_value(MidiContent::create({{{8}, {20}, {10}, 0x8000, 64, 1}}, {chance}, 7));
    auto clip = take_value(Clip::create({5}, {200}, {100}, std::move(content)));
    auto track = take_value(Track::create({6}, "musical", {clip}));
    auto sequence = take_value(Sequence::create({2}, "root", TickDuration{1'000}, {track}));
    const Project project = take_value(
        Project::create(ProjectInput{{1}, "modified", 100, {2}, {}, {sequence}}));

    const ConceptCensus counted = census(project);
    REQUIRE(counted.count(Concept::ClipNote) == 1);
    REQUIRE(counted.count(Concept::ClipNoteModifier) == 1);
    REQUIRE(counted.owners(Concept::ClipNoteModifier).size() == 1);
    REQUIRE(counted.owners(Concept::ClipNoteModifier)[0] == ItemId{5});
    REQUIRE(concept_detectable_in_model(Concept::ClipNoteModifier));

    // The same notes without a modifier record only the note concept, so the
    // new row cannot be a constant that fires on every note clip.
    auto plain_content = take_value(MidiContent::create({{{8}, {20}, {10}, 0x8000, 64, 1}}));
    auto plain_clip = take_value(Clip::create({5}, {200}, {100}, std::move(plain_content)));
    auto plain_track = take_value(Track::create({6}, "musical", {plain_clip}));
    auto plain_sequence = take_value(Sequence::create({2}, "root", TickDuration{1'000},
                                                      {plain_track}));
    const Project plain = take_value(
        Project::create(ProjectInput{{1}, "plain", 100, {2}, {}, {plain_sequence}}));
    const ConceptCensus plain_census = census(plain);
    REQUIRE(plain_census.count(Concept::ClipNote) == 1);
    REQUIRE_FALSE(plain_census.contains(Concept::ClipNoteModifier));

    // A seed is authored modifier state even when every note currently uses
    // neutral defaults. Formats that cannot carry it must still disclose the
    // loss rather than silently changing future probability decisions.
    auto seeded_content =
        take_value(MidiContent::create({{{8}, {20}, {10}, 0x8000, 64, 1}}, {}, 7));
    auto seeded_clip =
        take_value(Clip::create({5}, {200}, {100}, std::move(seeded_content)));
    auto seeded_track = take_value(Track::create({6}, "musical", {seeded_clip}));
    auto seeded_sequence = take_value(
        Sequence::create({2}, "root", TickDuration{1'000}, {seeded_track}));
    const Project seeded = take_value(
        Project::create(ProjectInput{{1}, "seeded", 100, {2}, {}, {seeded_sequence}}));
    REQUIRE(census(seeded).count(Concept::ClipNoteModifier) == 1);
}

TEST_CASE("a census records track mixer state and the lanes that automate it",
          "[interchange]") {
    // Without this the export loss manifest for a mixed session would claim
    // nothing was lost while dropping its levels — a lying manifest, which is
    // worse than a refusal.
    const auto lane = [](ItemId id, TrackMixerParameter parameter) {
        auto curve = AutomationCurve::create(
            {AutomationPoint{{id.value + 1}, {0}, 0.5f, AutomationInterpolation::Continuous,
                             0.0f}});
        REQUIRE(curve.has_value());
        auto created =
            AutomationLane::create(id, TrackMixerTarget{parameter}, std::move(curve).value());
        REQUIRE(created.has_value());
        return std::move(created).value();
    };
    auto track = take_value(Track::create(
        TrackInput{.id = {6},
                   .name = "mixed",
                   .automation_lanes = {lane({20}, TrackMixerParameter::Gain),
                                        lane({30}, TrackMixerParameter::Pan)},
                   .mixer = TrackMixer{0.5f, -0.25f}}));
    auto sequence = take_value(Sequence::create({3}, "sequence", TickDuration{100}, {track}));
    const ConceptCensus counted = census(
        take_value(Project::create(ProjectInput{{1}, "project", 100, {3}, {}, {sequence}})));

    REQUIRE(counted.contains(Concept::MixerTrackGain));
    REQUIRE(counted.contains(Concept::MixerTrackPan));
    REQUIRE(counted.contains(Concept::AutomationTrackGain));
    REQUIRE(counted.contains(Concept::AutomationTrackPan));
    REQUIRE(counted.owners(Concept::AutomationTrackGain)[0] == ItemId{20});
    REQUIRE_FALSE(counted.contains(Concept::AutomationDeviceParam));

    // A track that was never touched must not report mixer state, or every
    // document would claim to carry levels it does not have.
    auto plain = take_value(Track::create({6}, "plain", {}));
    auto plain_sequence = take_value(Sequence::create({3}, "sequence", TickDuration{100}, {plain}));
    const ConceptCensus untouched = census(take_value(
        Project::create(ProjectInput{{1}, "project", 100, {3}, {}, {plain_sequence}})));
    REQUIRE_FALSE(untouched.contains(Concept::MixerTrackGain));
    REQUIRE_FALSE(untouched.contains(Concept::MixerTrackPan));
}

namespace {

// A document carrying exactly the annotations the census used to be blind to:
// two markers, one region, and a session timecode origin.
Project annotated_project() {
    auto sequence = take_value(Sequence::create(SequenceInput{
        .id = {3},
        .name = "annotated",
        .musical_duration = TickDuration{4'000},
        .markers = {SequenceMarker{{20}, "verse", TickPosition{0}, {}},
                    SequenceMarker{{21}, "chorus", TickPosition{1'000}, {}}},
        .regions = {SequenceRegion{{22}, "bridge", TickPosition{2'000}, TickDuration{500}, {}}},
    }));
    ProjectInput input{{1}, "annotated", 40, {3}, {}, {sequence}};
    input.session_start = SessionStart{{48'000}, {48'000, 1}};
    return take_value(Project::create(std::move(input)));
}

} // namespace

TEST_CASE("a census records markers, regions, and the session timecode origin",
          "[interchange]") {
    // These are shipped model features. A census that cannot see them makes
    // plan_export report is_lossless() on a document that would lose every
    // named location and its wall-clock origin — a manifest that lies is worse
    // than a refusal, so the walker must observe them.
    const Project project = annotated_project();
    const ConceptCensus counted = census(project);

    // Markers and regions share one concept: the vocabulary defines it as "a
    // named point or range on the timeline", so two markers plus one region is
    // three occurrences of the same concept, not two concepts.
    REQUIRE(counted.contains(Concept::Marker));
    REQUIRE(counted.count(Concept::Marker) == 3);
    REQUIRE(counted.contains(Concept::TimecodeOrigin));
    REQUIRE(counted.count(Concept::TimecodeOrigin) == 1);

    // Owners name the items the concept was found on, so a loss manifest can
    // point at what would be dropped.
    const auto owners = counted.owners(Concept::Marker);
    REQUIRE(owners.size() == 3);

    // Both concepts must now declare themselves model-detectable, or the
    // census invariant ("never claim a concept the model cannot express")
    // would fail on this very document.
    REQUIRE(concept_detectable_in_model(Concept::Marker));
    REQUIRE(concept_detectable_in_model(Concept::TimecodeOrigin));
    for (Concept concept_value : counted.present())
        REQUIRE(concept_detectable_in_model(concept_value));
}

TEST_CASE("a census names empty clips and continuous tempo ramps", "[interchange]") {
    auto empty = take_value(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto note_content = take_value(MidiContent::create(
        {NoteEvent{{6}, TickPosition{0}, TickDuration{100}, 40'000, 60, 0}}));
    auto note = take_value(Clip::create({7}, {100}, {100}, std::move(note_content)));
    auto track =
        take_value(Track::create({5}, "empty", {std::move(empty), std::move(note)}));
    auto sequence = take_value(Sequence::create({3}, "root", TickDuration{1'000},
                                                {std::move(track)}));
    const std::array tempo_points{
        TempoPoint{TickPosition{0}, 100.0, TempoCurve::LinearInTicks},
        TempoPoint{TickPosition{1'000}, 140.0, TempoCurve::Constant}};
    auto tempo = TempoMap::create(tempo_points);
    REQUIRE(tempo);
    ProjectInput input{{1}, "ramped", 10, {3}, {}, {std::move(sequence)}};
    input.tempo_map = std::move(tempo.value());
    const ConceptCensus counted = census(take_value(Project::create(std::move(input))));

    REQUIRE(counted.count(Concept::ClipEmpty) == 1);
    REQUIRE(counted.owners(Concept::ClipEmpty).size() == 1);
    REQUIRE(counted.owners(Concept::ClipEmpty)[0] == ItemId{4});
    REQUIRE(counted.count(Concept::TempoRamp) == 1);
    REQUIRE(counted.owners(Concept::TempoRamp).size() == 1);
    REQUIRE(counted.owners(Concept::TempoRamp)[0] == ItemId{1});
    REQUIRE(counted.count(Concept::ClipNoteVelocityQuantized) == 1);
    REQUIRE(counted.owners(Concept::ClipNoteVelocityQuantized)[0] == ItemId{7});
    REQUIRE(counted.contains(Concept::TempoMap));
}

TEST_CASE("a census names a zero velocity that cannot be a sounding SMF Note On",
          "[interchange]") {
    auto notes = take_value(MidiContent::create(
        {NoteEvent{{6}, TickPosition{0}, TickDuration{100}, 0, 60, 0}}));
    auto clip = take_value(Clip::create({7}, {0}, {100}, std::move(notes)));
    auto track = take_value(Track::create({5}, "silent", {std::move(clip)}));
    auto sequence = take_value(Sequence::create({3}, "root", TickDuration{1'000},
                                                {std::move(track)}));
    const auto project = take_value(Project::create(
        ProjectInput{{1}, "zero velocity", 10, {3}, {}, {std::move(sequence)}}));

    const ConceptCensus counted = census(project);
    REQUIRE(counted.count(Concept::ClipNoteVelocityQuantized) == 1);
    REQUIRE(counted.owners(Concept::ClipNoteVelocityQuantized).size() == 1);
    REQUIRE(counted.owners(Concept::ClipNoteVelocityQuantized)[0] == ItemId{7});
}
