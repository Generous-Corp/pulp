#include <catch2/catch_test_macros.hpp>

#include <pulp/interchange/census.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
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
        Clip::create({5}, {200}, {100}, take_value(NoteContent::create({{{8}, {20}, {10}, 0x8000, 64, 1}}))));
    auto media_clip = take_value(Clip::create({4}, {0}, {100}, MediaRef{{11}, {25}, 100},
                                              ClipPlaybackProperties{0.5f, 16, 32}));
    auto absolute = take_value(
        Clip::create_absolute({9}, {0}, 480, {48'000, 1}, MediaRef{{12}, {0}, 480}));

    // A track anchors all of its clips the same way, so the sample-anchored
    // clip needs its own.
    auto musical_track = take_value(Track::create({6}, "musical", {note_clip, media_clip}));
    auto absolute_track = take_value(Track::create({10}, "absolute", {absolute}));
    auto lane = take_value(ChordScaleLane::create({}));
    auto sequence = take_value(Sequence::create(SequenceInput{
        .id = {3},
        .name = "sequence",
        .musical_duration = TickDuration{4'000},
        .tracks = {musical_track, absolute_track},
        .chord_scale_lane = std::move(lane),
        .scenes = {Scene{{13}, "launcher", {Slot{{14}, {4}, launch_immediate(), {}}}}},
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
        REQUIRE(counted.count(Concept::ClipMusical) == 2);
        REQUIRE(counted.count(Concept::ClipAbsolute) == 1);
        REQUIRE(counted.count(Concept::ClipNote) == 1);
        REQUIRE(counted.count(Concept::ClipMedia) == 2);
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
        REQUIRE_FALSE(counted.contains(Concept::Marker));
        REQUIRE_FALSE(counted.contains(Concept::ClipCrossfade));
        REQUIRE_FALSE(counted.contains(Concept::MixerTrackGain));
    }

    SECTION("owners name the items the concept was found on") {
        const auto owners = counted.owners(Concept::ClipGain);
        REQUIRE(owners.size() == 1);
        REQUIRE(owners[0] == ItemId{4});
        const auto launch_owners = counted.owners(Concept::ClipLaunch);
        REQUIRE(launch_owners.size() == 1);
        REQUIRE(launch_owners[0] == ItemId{13});
    }
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
