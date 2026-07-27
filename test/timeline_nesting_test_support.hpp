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

NoteContent note_content(std::uint64_t note_id, std::int64_t start = 120,
                         std::int64_t duration = 240) {
    return take(NoteContent::create({NoteEvent{{note_id}, {start}, {duration}, 40'000, 64, 0}}));
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
