#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace timeline_test;

namespace {

template <typename T, typename E> T value_of(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

// Identity layout. Project 1, sequence 3, asset 20. The sequence is authored
// with tracks 30 then 10 — deliberately not ascending, so an assertion on
// authored order cannot pass by accidentally reading the id-sorted index. The
// inserted track owns 50 through 59, all at or above next_item_id 50.
constexpr ItemId kSequence{3};
constexpr ItemId kAsset{20};
constexpr ItemId kBass{30};
constexpr ItemId kDrums{10};
constexpr ItemId kTrack{50};
constexpr ItemId kDevice{51};
constexpr ItemId kClip{52};
constexpr ItemId kFirstNote{53};
constexpr ItemId kSecondNote{54};
constexpr ItemId kAutomationLane{55};
constexpr ItemId kFirstPoint{56};
constexpr ItemId kSecondPoint{57};
constexpr ItemId kTakeLane{58};
constexpr ItemId kTake{59};

// Every identity the inserted track owns, across all four levels. The two
// lane-parented kinds (automation point, take) are the ones a hand-written
// owned set drops, so they are asserted by name at every step.
constexpr std::array kOwned{kTrack,          kDevice,          kClip,      kFirstNote,
                            kSecondNote,     kAutomationLane,  kFirstPoint,
                            kSecondPoint,    kTakeLane,        kTake};

MediaAsset recorded_asset() {
    return MediaAsset{kAsset, "vocal.wav", 1'000, {48'000, 1}, content_hash(),
                      AssetStoragePolicy::External, {}, {}};
}

Track plain_track(ItemId id, std::string name) {
    return value_of(Track::create(id, std::move(name), {}));
}

// A track that reaches every owned identity kind: a device placement, a note
// clip carrying two notes, an automation lane carrying two points, and a take
// lane carrying one take.
Track authored_track() {
    auto notes = value_of(MidiContent::create(
        {{kFirstNote, {0}, {kTicksPerQuarter / 2}, 12'000, 60, 0},
         {kSecondNote, {kTicksPerQuarter / 2}, {kTicksPerQuarter / 2}, 24'000, 64, 0}}));
    auto clip =
        value_of(Clip::create(kClip, {0}, {kTicksPerQuarter}, std::move(notes)));
    auto curve = value_of(
        AutomationCurve::create({{kFirstPoint, {0}, 0.25f}, {kSecondPoint, {50}, 0.75f}}));
    auto lane = value_of(AutomationLane::create(kAutomationLane,
                                                DeviceParameterTarget{kDevice, 7},
                                                std::move(curve)));
    auto recording = value_of(
        Take::create(kTake, MediaRef{kAsset, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto take_lane = value_of(TakeLane::create(kTakeLane, "comp", {recording}));
    return value_of(Track::create(TrackInput{
        .id = kTrack,
        .name = "vocals",
        .clips = {clip},
        .device_chain = {DevicePlacement{kDevice}},
        .automation_lanes = {lane},
        .take_lanes = {take_lane},
        .mixer = TrackMixer{0.5f, -0.25f},
    }));
}

// The arrangement before the track exists. next_item_id 50 leaves the whole
// owned subtree available for InsertTrack to allocate.
Project arrangement() {
    auto sequence = value_of(Sequence::create(SequenceInput{
        .id = kSequence,
        .name = "sequence",
        .musical_duration = TickDuration{8 * kTicksPerQuarter},
        .tracks = {plain_track(kBass, "bass"), plain_track(kDrums, "drums")},
    }));
    return value_of(Project::create(
        ProjectInput{{1}, "project", 50, kSequence, {recorded_asset()}, {sequence}}));
}

// The same arrangement with the track already placed and a launcher slot
// sourcing one of its clips.
Project launched_arrangement() {
    auto sequence = value_of(Sequence::create(SequenceInput{
        .id = kSequence,
        .name = "sequence",
        .musical_duration = TickDuration{8 * kTicksPerQuarter},
        .tracks = {plain_track(kBass, "bass"), authored_track(), plain_track(kDrums, "drums")},
        .scenes = {Scene{{35}, "verse", SlotList({Slot{{36}, kClip, launch_immediate(), {}}})}},
    }));
    return value_of(Project::create(
        ProjectInput{{1}, "project", 60, kSequence, {recorded_asset()}, {sequence}}));
}

std::vector<ItemId> authored_order(const Project& project) {
    const auto order = project.find_sequence(kSequence)->track_order();
    return {order.begin(), order.end()};
}

std::vector<ItemId> identity_order(const Project& project) {
    std::vector<ItemId> result;
    for (const auto& track : project.find_sequence(kSequence)->tracks())
        result.push_back(track.id());
    return result;
}

void require_owned_active(const Project& project, bool active) {
    for (const auto id : kOwned) {
        const auto located = project.locate(id);
        REQUIRE(located);
        REQUIRE(located->active == active);
    }
}

// Right identities with a wrong coordinate cache survives both undo and redo,
// because reactivation only compares kind and immediate parent. Pinning the
// complete location of each level is what catches it.
void require_owned_coordinates(const Project& project, bool active) {
    REQUIRE(project.locate(kTrack) ==
            ItemLocation{ItemKind::Track, kSequence, kSequence, kTrack, {}, active});
    REQUIRE(project.locate(kDevice) ==
            ItemLocation{ItemKind::DevicePlacement, kTrack, kSequence, kTrack, {}, active});
    REQUIRE(project.locate(kClip) ==
            ItemLocation{ItemKind::Clip, kTrack, kSequence, kTrack, kClip, active});
    REQUIRE(project.locate(kFirstNote) ==
            ItemLocation{ItemKind::Note, kClip, kSequence, kTrack, kClip, active});
    REQUIRE(project.locate(kSecondNote) ==
            ItemLocation{ItemKind::Note, kClip, kSequence, kTrack, kClip, active});
    REQUIRE(project.locate(kAutomationLane) ==
            ItemLocation{ItemKind::AutomationLane, kTrack, kSequence, kTrack, {}, active});
    REQUIRE(project.locate(kFirstPoint) ==
            ItemLocation{ItemKind::AutomationPoint, kAutomationLane, kSequence, kTrack, {},
                         active});
    REQUIRE(project.locate(kSecondPoint) ==
            ItemLocation{ItemKind::AutomationPoint, kAutomationLane, kSequence, kTrack, {},
                         active});
    REQUIRE(project.locate(kTakeLane) ==
            ItemLocation{ItemKind::TakeLane, kTrack, kSequence, kTrack, {}, active});
    REQUIRE(project.locate(kTake) ==
            ItemLocation{ItemKind::Take, kTakeLane, kSequence, kTrack, {}, active});
}

// A committed insert, used as the starting document for the removal cases.
Project seeded() {
    auto session = value_of(DocumentSession::create(arrangement()));
    auto writer = value_of(session->register_writer());
    REQUIRE(session->submit(
        writer, session_transaction(writer, {},
                                    {InsertTrack{kSequence, authored_track(), kDrums}})));
    return *session->snapshot();
}

} // namespace

TEST_CASE("Track insert publishes every owned identity and lands at the authored position") {
    const auto initial = arrangement();
    auto session = value_of(DocumentSession::create(initial));
    auto writer = value_of(session->register_writer());
    auto committed = session->submit(
        writer,
        session_transaction(writer, {}, {InsertTrack{kSequence, authored_track(), kDrums}}));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0] ==
            DirtyItem{kTrack, kTrack, kSequence, DirtyFlags::Structure | DirtyFlags::Added});

    const auto* placed = session->snapshot()->find_sequence(kSequence)->find_track(kTrack);
    REQUIRE(placed);
    REQUIRE(placed->mixer() == TrackMixer{0.5f, -0.25f});
    // Authored order records the requested position; identity order appends, so
    // every index derived from tracks() stays put.
    REQUIRE(authored_order(*session->snapshot()) == std::vector<ItemId>{kBass, kTrack, kDrums});
    REQUIRE(identity_order(*session->snapshot()) == std::vector<ItemId>{kBass, kDrums, kTrack});
    require_owned_coordinates(*session->snapshot(), true);
    REQUIRE(session->snapshot()->next_item_id() == 60);

    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(session->snapshot()->find_sequence(kSequence)->find_track(kTrack));
    require_owned_active(*session->snapshot(), false);
    REQUIRE(authored_order(*session->snapshot()) == std::vector<ItemId>{kBass, kDrums});

    REQUIRE(session->redo(writer));
    require_owned_coordinates(*session->snapshot(), true);
    REQUIRE(authored_order(*session->snapshot()) == std::vector<ItemId>{kBass, kTrack, kDrums});

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    require_owned_coordinates(*replayed, true);
    REQUIRE(authored_order(*replayed) == std::vector<ItemId>{kBass, kTrack, kDrums});
}

TEST_CASE("Track insert is rejected when a target or a referenced asset is missing") {
    const auto initial = arrangement();

    auto missing_sequence = reduce_transaction(
        initial,
        transaction({1}, 1, 1, {}, {InsertTrack{{999}, authored_track(), std::nullopt}}));
    REQUIRE_FALSE(missing_sequence);
    REQUIRE(missing_sequence.error().code == ConflictCode::TargetMissing);

    auto missing_neighbour = reduce_transaction(
        initial, transaction({1}, 1, 1, {}, {InsertTrack{kSequence, authored_track(), {{999}}}}));
    REQUIRE_FALSE(missing_neighbour);
    REQUIRE(missing_neighbour.error().code == ConflictCode::TargetMissing);

    // A take on the inserted track references an asset that is not in the project.
    auto orphan_take =
        value_of(Take::create(kTake, MediaRef{{21}, {0}, 100}, {0}, RationalRate{48'000, 1}));
    auto orphan_lane = value_of(TakeLane::create(kTakeLane, "comp", {orphan_take}));
    auto orphan = value_of(Track::create(TrackInput{
        .id = kTrack,
        .name = "vocals",
        .take_lanes = {orphan_lane},
    }));
    auto missing_asset = reduce_transaction(
        initial, transaction({1}, 1, 1, {}, {InsertTrack{kSequence, orphan, std::nullopt}}));
    REQUIRE_FALSE(missing_asset);
    REQUIRE(missing_asset.error().code == ConflictCode::ModelInvariant);
}

TEST_CASE("Track remove undo redo and replay restore the whole owned subtree") {
    const auto initial = seeded();
    auto session = value_of(DocumentSession::create(initial));
    auto writer = value_of(session->register_writer());
    auto committed =
        session->submit(writer, session_transaction(writer, {}, {RemoveTrack{kSequence, kTrack}}));
    REQUIRE(committed);
    REQUIRE(committed->dirty.items()[0] ==
            DirtyItem{kTrack, kTrack, kSequence, DirtyFlags::Structure | DirtyFlags::Removed});
    REQUIRE_FALSE(session->snapshot()->find_sequence(kSequence)->find_track(kTrack));
    require_owned_active(*session->snapshot(), false);
    REQUIRE(authored_order(*session->snapshot()) == std::vector<ItemId>{kBass, kDrums});

    REQUIRE(session->undo(writer));
    const auto* restored = session->snapshot()->find_sequence(kSequence)->find_track(kTrack);
    REQUIRE(restored);
    REQUIRE(equivalent(Command{InsertTrack{kSequence, *restored, kDrums}},
                       Command{InsertTrack{kSequence, authored_track(), kDrums}}));
    require_owned_coordinates(*session->snapshot(), true);
    // Undo restores the authored position exactly rather than appending.
    REQUIRE(authored_order(*session->snapshot()) == std::vector<ItemId>{kBass, kTrack, kDrums});

    REQUIRE(session->redo(writer));
    REQUIRE_FALSE(session->snapshot()->find_sequence(kSequence)->find_track(kTrack));
    require_owned_active(*session->snapshot(), false);

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE_FALSE(replayed->find_sequence(kSequence)->find_track(kTrack));
    require_owned_active(*replayed, false);
    REQUIRE(authored_order(*replayed) == std::vector<ItemId>{kBass, kDrums});
}

TEST_CASE("Track remove leaves no owned identity available to a later command") {
    auto session = value_of(DocumentSession::create(seeded()));
    auto writer = value_of(session->register_writer());
    REQUIRE(session->submit(writer,
                            session_transaction(writer, {}, {RemoveTrack{kSequence, kTrack}})));

    // Every owned identity is now a tombstone, so re-inserting the same track
    // from a fresh transaction cannot claim any of them.
    auto reuse = session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {InsertTrack{kSequence, authored_track(), kDrums}}));
    REQUIRE_FALSE(reuse);
    REQUIRE(reuse.error().code == ConflictCode::IdentityNotAvailable);
}

TEST_CASE("Track remove is refused while a launcher slot sources one of its clips") {
    const auto initial = launched_arrangement();
    auto session = value_of(DocumentSession::create(initial));
    auto writer = value_of(session->register_writer());
    auto rejected =
        session->submit(writer, session_transaction(writer, {}, {RemoveTrack{kSequence, kTrack}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(session->revision() == DocumentRevision{});
    REQUIRE(session->snapshot()->find_sequence(kSequence)->find_track(kTrack));
    require_owned_active(*session->snapshot(), true);
}

TEST_CASE("Track insert is charged its real owned subtree by the journal byte limit") {
    const auto empty_charge =
        retained_size(Command{InsertTrack{kSequence, plain_track(kTrack, "vocals")}});
    const auto authored_charge =
        retained_size(Command{InsertTrack{kSequence, authored_track()}});
    REQUIRE(authored_charge > empty_charge);
    REQUIRE(authored_charge >
            sizeof(InsertTrack) + 2 * sizeof(NoteEvent) + 2 * sizeof(AutomationPoint) +
                sizeof(Take) + sizeof(DevicePlacement));

    // Measure what the journal actually charges an empty track, then hold that
    // exact budget while swapping in the authored subtree. A retained_size that
    // fell back to sizeof(InsertTrack) would charge both the same and admit
    // both, so the refusal below is what proves the payload is counted.
    SessionLimits limits;
    {
        auto probe = value_of(DocumentSession::create(arrangement()));
        auto writer = value_of(probe->register_writer());
        REQUIRE(probe->submit(
            writer, session_transaction(
                        writer, {},
                        {InsertTrack{kSequence, plain_track(kTrack, "vocals"), kDrums}})));
        limits.journal.max_retained_bytes = probe->journal().retained_bytes();
    }

    SECTION("the measured budget admits an empty track") {
        auto session = value_of(DocumentSession::create(arrangement(), limits));
        auto writer = value_of(session->register_writer());
        REQUIRE(session->submit(
            writer,
            session_transaction(
                writer, {}, {InsertTrack{kSequence, plain_track(kTrack, "vocals"), kDrums}})));
        REQUIRE(session->snapshot()->find_sequence(kSequence)->find_track(kTrack));
    }

    SECTION("the same budget refuses the authored subtree atomically") {
        auto session = value_of(DocumentSession::create(arrangement(), limits));
        auto writer = value_of(session->register_writer());
        auto rejected = session->submit(
            writer, session_transaction(writer, {},
                                        {InsertTrack{kSequence, authored_track(), kDrums}}));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::JournalFull);
        REQUIRE(session->revision() == DocumentRevision{});
        REQUIRE_FALSE(session->snapshot()->find_sequence(kSequence)->find_track(kTrack));
    }
}

TEST_CASE("Track command equivalence compares the whole authored payload") {
    const Command insert{InsertTrack{kSequence, authored_track(), kDrums}};
    REQUIRE(equivalent(insert, Command{InsertTrack{kSequence, authored_track(), kDrums}}));
    REQUIRE_FALSE(equivalent(insert, Command{InsertTrack{kSequence, authored_track(), kBass}}));
    REQUIRE_FALSE(
        equivalent(insert, Command{InsertTrack{kSequence, authored_track(), std::nullopt}}));

    auto renamed = value_of(authored_track().with_mixer(TrackMixer{0.25f, -0.25f}));
    REQUIRE_FALSE(equivalent(insert, Command{InsertTrack{kSequence, renamed, kDrums}}));

    REQUIRE(equivalent(Command{RemoveTrack{kSequence, kTrack}},
                       Command{RemoveTrack{kSequence, kTrack}}));
    REQUIRE_FALSE(equivalent(Command{RemoveTrack{kSequence, kTrack}},
                             Command{RemoveTrack{kSequence, kDrums}}));
}
