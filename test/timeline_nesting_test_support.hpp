#pragma once

#include "playback_audio_renderer_test_support.hpp"

#include <pulp/playback/compile_context_registry.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace {

MidiContent note_content(std::uint64_t note_id, std::int64_t start = 120,
                         std::int64_t duration = 240) {
    return take(MidiContent::create({NoteEvent{{note_id}, {start}, {duration}, 40'000, 64, 0}}));
}

Track track(std::uint64_t id, std::vector<Clip> clips, std::vector<DevicePlacement> devices = {}) {
    TrackInput input;
    input.id = {id};
    input.name = "track";
    input.clips = std::move(clips);
    input.device_chain = std::move(devices);
    return take(Track::create(std::move(input)));
}

Clip nested_clip(std::uint64_t id, std::uint64_t sequence_id, std::int64_t start = 480,
                 std::int64_t duration = 960, std::int64_t source_start = 0) {
    return take(
        Clip::create({id}, {start}, {duration}, SequenceRef{{sequence_id}, {source_start}}));
}

Project nested_note_project(bool child_has_device = false, std::size_t root_reference_count = 1) {
    auto child_clip = take(Clip::create({12}, {0}, {960}, note_content(13)));
    auto child_track = track(11, {child_clip},
                             child_has_device ? std::vector<DevicePlacement>{{{14}}}
                                              : std::vector<DevicePlacement>{});
    auto child = take(Sequence::create({10}, "child", TickDuration{960}, {child_track}));
    std::vector<Track> root_tracks;
    for (std::size_t index = 0; index < root_reference_count; ++index)
        root_tracks.push_back(
            track(3 + 2 * index, {nested_clip(4 + 2 * index, 10, 480 + 1200 * index)}));
    auto root = take(Sequence::create({2}, "root", std::nullopt, std::move(root_tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "nested";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

// Child-track processing a SequenceRef then nests. Named for the same reason
// NestedChildState is: a call site reads as the document it authors, and the
// two constructs stay separable. They must be, because they are refused by
// different codes and a document that carries both would only ever report the
// first.
struct NestedChildProcessing {
    bool device_chain = false;
    bool automation_lane = false;
};

// One child track carrying `processing`, nested by a root exactly as
// nested_note_project nests its own. The lane targets the track's own mixer
// gain rather than a device parameter so the automation case needs no device
// placement to be valid, which is what lets the two constructs be authored
// independently.
Project nested_child_processing_project(NestedChildProcessing processing) {
    TrackInput child_input;
    child_input.id = {11};
    child_input.name = "track";
    child_input.clips.push_back(take(Clip::create({12}, {0}, {960}, note_content(13))));
    if (processing.device_chain)
        child_input.device_chain.push_back(DevicePlacement{{14}});
    if (processing.automation_lane) {
        auto curve = take(AutomationCurve::create(
            {AutomationPoint{{16}, {0}, 1.0f, AutomationInterpolation::Continuous, 0.0f},
             AutomationPoint{{17}, {960}, 0.5f, AutomationInterpolation::Continuous, 0.0f}}));
        child_input.automation_lanes.push_back(take(AutomationLane::create(
            {15}, TrackMixerTarget{TrackMixerParameter::Gain}, std::move(curve))));
    }

    auto child = take(Sequence::create({10}, "child", TickDuration{960},
                                       {take(Track::create(std::move(child_input)))}));
    auto root = take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10)})}));
    ProjectInput input;
    input.id = {1};
    input.name = "nested";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

// Child-track state a SequenceRef then nests. Named rather than positional so
// a call site reads as the document it authors, and so adding a state later
// cannot silently re-target an existing call.
struct NestedChildState {
    bool record_armed = false;
    // A lane that exists and holds a real take, but is not selected.
    bool dormant_take_lane = false;
    bool frozen = false;
    // Selects the lane authored by dormant_take_lane.
    bool active_take_lane = false;
};

// One child track carrying `state`, nested by a root exactly as
// nested_note_project nests its own. The take media is declared as a project
// asset because Project::create validates the reference; no audio data is
// supplied, because nothing resolves a dormant lane to media.
Project nested_child_state_project(NestedChildState state) {
    const auto hash = *ContentHash::from_hex(std::string(64, 'b'));
    const timebase::RationalRate rate{48'000, 1};
    constexpr std::uint64_t kTakeFrames = 4'800;

    TrackInput child_input;
    child_input.id = {11};
    child_input.name = "track";
    child_input.clips.push_back(take(Clip::create({12}, {0}, {960}, note_content(13))));
    child_input.record_armed = state.record_armed;
    if (state.dormant_take_lane || state.active_take_lane) {
        auto recorded = take(Take::create({21}, MediaRef{{60}, {0}, kTakeFrames}, {0}, rate));
        child_input.take_lanes.push_back(take(TakeLane::create({20}, "alt", {recorded})));
    }
    if (state.active_take_lane)
        child_input.active_take_lane_id = {20};
    if (state.frozen)
        child_input.freeze = TrackFreeze{MediaRef{{60}, {0}, kTakeFrames}, {0}, rate, hash};

    auto child = take(Sequence::create({10}, "child", TickDuration{960},
                                       {take(Track::create(std::move(child_input)))}));
    auto root = take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10)})}));
    ProjectInput input;
    input.id = {1};
    input.name = "nested";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.assets = {MediaAsset{.id = {60},
                               .name = "take",
                               .frame_count = kTakeFrames,
                               .sample_rate = rate,
                               .content_hash = hash}};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

std::shared_ptr<const Project> shared(Project project) {
    return std::make_shared<const Project>(std::move(project));
}

std::shared_ptr<const PlaybackProgram> compile(std::shared_ptr<const Project> project,
                                               std::uint64_t max_events = 1'000'000) {
    static std::vector<std::unique_ptr<PlaybackProgramStore>> stores;
    static std::vector<std::unique_ptr<InlineExecutor>> executors;
    static std::vector<std::unique_ptr<PlaybackProgramCompiler>> compilers;
    auto store = std::make_unique<PlaybackProgramStore>();
    auto executor = std::make_unique<InlineExecutor>();
    auto compiler =
        std::make_unique<PlaybackProgramCompiler>(*store, *executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.sample_rate = request.tempo_map->sample_rate();
    request.document_revision = 1;
    request.dirty.all = true;
    request.max_expanded_note_events = max_events;
    auto submitted = compiler->submit(std::move(request));
    REQUIRE(submitted);
    REQUIRE_FALSE(compiler->status().has_error);
    auto guard = store->read();
    REQUIRE(guard);
    auto result =
        std::shared_ptr<const PlaybackProgram>(guard.operator->(), [](const PlaybackProgram*) {});
    // Keep the store and compiler alive for the non-owning test view.
    stores.push_back(std::move(store));
    executors.push_back(std::move(executor));
    compilers.push_back(std::move(compiler));
    return result;
}

Transaction transaction(std::uint64_t transaction_sequence, std::uint64_t first_command_sequence,
                        std::vector<Command> commands) {
    Transaction result;
    result.id = {{1}, transaction_sequence};
    result.expected_revision = {};
    for (auto& command : commands)
        result.commands.push_back({{{1}, first_command_sequence++}, std::move(command)});
    return result;
}

Project nested_audio_project(const std::shared_ptr<const audio::AudioFileData>& data) {
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));
    auto child_media = musical_media_clip(12, 0, kTicksPerQuarter, 50, data->num_frames());
    auto child = take(Sequence::create({10}, "child", TickDuration{kTicksPerQuarter},
                                       {track(11, {child_media})}));
    auto root = take(
        Sequence::create({2}, "root", std::nullopt,
                         {track(3, {nested_clip(4, 10, kTicksPerQuarter, kTicksPerQuarter)})}));
    ProjectInput input;
    input.id = {1};
    input.name = "nested audio";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.assets = {{50, "ramp", data->num_frames(), {48'000, 1}, hash}};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

} // namespace
