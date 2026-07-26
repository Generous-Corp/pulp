#include "playback_audio_renderer_test_support.hpp"

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

TEST_CASE("SequenceRef persistence is canonical and unknown content downgrades opaquely") {
    auto registry = take(make_builtin_timeline_registry());
    const auto project = nested_note_project();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded->json, registry);
    REQUIRE(decoded);
    auto reencoded = serialize_project(decoded.value(), registry);
    REQUIRE(reencoded);
    REQUIRE(reencoded->json == encoded->json);

    auto unknown = encoded->json;
    const std::string known = "pulp.timeline.content.sequence_ref";
    const std::string replacement = "vendor.timeline.content.unknown_ref";
    const auto offset = unknown.find(known);
    REQUIRE(offset != std::string::npos);
    unknown.replace(offset, known.size(), replacement);
    auto downgraded = deserialize_project(unknown, registry);
    REQUIRE(downgraded);
    auto preserved = serialize_project(downgraded.value(), registry);
    REQUIRE(preserved);
    REQUIRE(preserved->json == unknown);
    auto refused = remap_ids(downgraded.value(), 1'000);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == ModelErrorCode::OpaqueContentCannotRemap);

    auto overflowing = encoded->json;
    const std::string valid_window = "\"sequence_id\":\"10\",\"source_start\":\"0\"";
    const std::string overflowing_window =
        "\"sequence_id\":\"10\",\"source_start\":\"9223372036854775807\"";
    const auto window_offset = overflowing.find(valid_window);
    REQUIRE(window_offset != std::string::npos);
    overflowing.replace(window_offset, valid_window.size(), overflowing_window);
    auto rejected = deserialize_project(overflowing, registry);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::InvalidDuration);
}

TEST_CASE("SequenceRef validation rejects missing references cycles and excessive depth") {
    auto overflowing_clip =
        Clip::create({1}, {0}, {1},
                     SequenceRef{{2}, {std::numeric_limits<std::int64_t>::max()}});
    REQUIRE_FALSE(overflowing_clip);
    REQUIRE(overflowing_clip.error().code == ModelErrorCode::InvalidDuration);

    auto missing_root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 99)})}));
    ProjectInput missing{{1}, "missing", 100, {2}, {}, {missing_root}};
    auto missing_result = Project::create(std::move(missing));
    REQUIRE_FALSE(missing_result);
    REQUIRE(missing_result.error().code == ModelErrorCode::MissingSequenceReference);

    auto a = take(Sequence::create({2}, "a", std::nullopt, {track(3, {nested_clip(4, 10, 0)})}));
    auto b = take(Sequence::create({10}, "b", std::nullopt, {track(11, {nested_clip(12, 2, 0)})}));
    ProjectInput cycle{{1}, "cycle", 100, {2}, {}, {a, b}};
    auto cycle_result = Project::create(std::move(cycle));
    REQUIRE_FALSE(cycle_result);
    REQUIRE(cycle_result.error().code == ModelErrorCode::SequenceReferenceCycle);
    REQUIRE(cycle_result.error().item.valid());
    REQUIRE(cycle_result.error().related_item.valid());

    auto branching_a =
        take(Sequence::create({2}, "branching a", std::nullopt,
                              {track(3, {nested_clip(4, 10, 0), nested_clip(5, 20, 960)})}));
    auto acyclic_b = take(Sequence::create({10}, "acyclic b", std::nullopt, {track(11, {})}));
    auto branching_c = take(
        Sequence::create({20}, "branching c", std::nullopt, {track(21, {nested_clip(22, 2, 0)})}));
    auto branching_cycle = Project::create(
        ProjectInput{{1}, "branching cycle", 100, {2}, {}, {branching_a, acyclic_b, branching_c}});
    REQUIRE_FALSE(branching_cycle);
    REQUIRE(branching_cycle.error().code == ModelErrorCode::SequenceReferenceCycle);
    REQUIRE(branching_cycle.error().item == ItemId{20});
    REQUIRE(branching_cycle.error().related_item == ItemId{2});

    std::vector<Sequence> chain;
    constexpr std::uint64_t chain_size = 2'048;
    for (std::uint64_t index = 0; index < chain_size; ++index) {
        const auto sequence_id = 100 + index * 10;
        std::vector<Clip> clips;
        if (index + 1 < chain_size)
            clips.push_back(nested_clip(sequence_id + 2, sequence_id + 10, 0));
        chain.push_back(take(Sequence::create({sequence_id}, "chain", std::nullopt,
                                              {track(sequence_id + 1, std::move(clips))})));
    }
    ProjectInput deep{{1}, "deep", 100 + chain_size * 10, {100}, {}, std::move(chain)};
    auto deep_result = Project::create(std::move(deep));
    REQUIRE_FALSE(deep_result);
    REQUIRE(deep_result.error().code == ModelErrorCode::SequenceNestingTooDeep);

    const auto chain_project = [](std::uint64_t count) {
        std::vector<Sequence> sequences;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto sequence_id = 1'000 + index * 10;
            std::vector<Clip> clips;
            if (index + 1 < count)
                clips.push_back(nested_clip(sequence_id + 2, sequence_id + 10, 0));
            sequences.push_back(take(Sequence::create({sequence_id}, "boundary", std::nullopt,
                                                      {track(sequence_id + 1, std::move(clips))})));
        }
        return Project::create(
            ProjectInput{{1}, "boundary", 1'000 + count * 10, {1'000}, {}, std::move(sequences)});
    };
    REQUIRE(chain_project(9));
    auto nine_edges = chain_project(10);
    REQUIRE_FALSE(nine_edges);
    REQUIRE(nine_edges.error().code == ModelErrorCode::SequenceNestingTooDeep);
    auto boundary = take(chain_project(9));
    auto too_deep_insert = take(Sequence::create({2'000}, "new parent", std::nullopt,
                                                 {track(2'001, {nested_clip(2'002, 1'000, 0)})}));
    auto append_result =
        reduce_transaction(boundary, transaction(1, 1, {InsertSequence{too_deep_insert}}));
    REQUIRE_FALSE(append_result);
    REQUIRE(append_result.error().model_error);
    REQUIRE(append_result.error().model_error->code == ModelErrorCode::SequenceNestingTooDeep);

    auto edge_child = take(Sequence::create({10}, "child", TickDuration{100}, {track(11, {})}));
    auto edge_root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 100)})}));
    const auto valid =
        take(Project::create(ProjectInput{{1}, "edge", 100, {2}, {}, {edge_root, edge_child}}));
    auto cycle_clip = nested_clip(20, 2, 0, 100);
    auto cycle_insert =
        reduce_transaction(valid, transaction(1, 1, {InsertClip{{10}, {11}, cycle_clip}}));
    REQUIRE_FALSE(cycle_insert);
    REQUIRE(cycle_insert.error().model_error);
    REQUIRE(cycle_insert.error().model_error->code == ModelErrorCode::SequenceReferenceCycle);
}

TEST_CASE("SequenceRef remap preserves or explicitly translates sibling references") {
    const auto project = nested_note_project();
    const auto* root = project.find_sequence({2});
    REQUIRE(root);
    ItemIdAllocator allocator{200};
    auto preserved = remap_ids(*root, allocator);
    REQUIRE(preserved);
    const auto& preserved_ref =
        std::get<SequenceRef>(preserved->sequence.tracks()[0].clips()[0].content());
    REQUIRE(preserved_ref.sequence_id == ItemId{10});

    struct Context {
        ItemId mapped;
    } context{{777}};
    ExternalIdFixup sequence_fixup{
        &context, [](void* raw, ItemId) noexcept -> runtime::Result<ItemId, ModelError> {
            return runtime::Ok(static_cast<Context*>(raw)->mapped);
        }};
    ItemIdAllocator translated_allocator{300};
    auto translated = remap_ids(*root, translated_allocator, RemapIdFixups{{}, sequence_fixup});
    REQUIRE(translated);
    REQUIRE(
        std::get<SequenceRef>(translated->sequence.tracks()[0].clips()[0].content()).sequence_id ==
        ItemId{777});
}

TEST_CASE("Sequence reference cache updates without losing shared targets") {
    auto child = take(Sequence::create({10}, "child", TickDuration{100}, {}));
    auto first = track(3, {nested_clip(4, 10, 0, 100)});
    auto second = track(5, {nested_clip(6, 10, 0, 100)});
    auto root = take(Sequence::create({2}, "root", std::nullopt, {first, second}));
    REQUIRE(root.outgoing_sequence_refs().size() == 1);
    REQUIRE(root.outgoing_sequence_refs()[0] == ItemId{10});

    auto cleared_first_clip = take(first.find_clip({4})->with_content(EmptyContent{}));
    auto cleared_first = take(first.replace_clip(std::move(cleared_first_clip)));
    auto one_remaining = take(root.replace_track(std::move(cleared_first)));
    REQUIRE(one_remaining.outgoing_sequence_refs().size() == 1);
    REQUIRE(one_remaining.outgoing_sequence_refs()[0] == ItemId{10});

    auto cleared_second_clip = take(second.find_clip({6})->with_content(EmptyContent{}));
    auto cleared_second = take(second.replace_clip(std::move(cleared_second_clip)));
    auto none_remaining = take(one_remaining.replace_track(std::move(cleared_second)));
    REQUIRE(none_remaining.outgoing_sequence_refs().empty());

    auto project = take(Project::create(ProjectInput{{1}, "cache", 100, {2}, {}, {root, child}}));
    auto retargeted = reduce_transaction(
        project, transaction(1, 1, {SetClipSequenceRef{{2}, {3}, {4}, {{10}, {0}}, {{10}, {10}}}}));
    REQUIRE(retargeted);
    REQUIRE(retargeted->project.find_sequence({2})->outgoing_sequence_refs().size() == 1);
    REQUIRE(retargeted->project.find_sequence({2})->outgoing_sequence_refs()[0] == ItemId{10});
}

TEST_CASE("Eager divergence carries exact ids and undo removes the clone") {
    const auto original = nested_note_project();
    const auto location = *original.locate({4});
    auto session = take(DocumentSession::create(original));
    auto writer = take(session->register_writer());
    const auto transaction_id = writer.allocate_transaction_id();
    const auto clone_command_id = writer.allocate_command_id();
    const auto retarget_command_id = writer.allocate_command_id();
    auto plan = build_diverge_transaction(original, location, transaction_id, {},
                                          clone_command_id, retarget_command_id);
    REQUIRE(plan);
    REQUIRE(plan->commands[0].id == clone_command_id);
    REQUIRE(plan->commands[1].id == retarget_command_id);
    REQUIRE(session->submit(writer, plan.value()));
    Transaction next_edit;
    next_edit.id = writer.allocate_transaction_id();
    next_edit.expected_revision = session->revision();
    const auto next_command_id = writer.allocate_command_id();
    REQUIRE(next_command_id.sequence == retarget_command_id.sequence + 1);
    next_edit.commands.push_back(
        {next_command_id, SetNoteVelocity{{10}, {11}, {12}, {13}, 40'000, 50'000}});
    REQUIRE(session->submit(writer, std::move(next_edit)));
    const auto mapping = std::get<CloneSequence>(plan->commands[0].command).id_remap;
    auto diverged = reduce_transaction(original, plan.value());
    REQUIRE(diverged);
    REQUIRE(diverged->project.next_item_id() == original.next_item_id() + mapping.size());
    const auto& replacement = std::get<SetClipSequenceRef>(plan->commands[1].command).replacement;
    REQUIRE(diverged->project.find_sequence(replacement.sequence_id));
    REQUIRE(std::get<SequenceRef>(
                diverged->project.find_sequence({2})->tracks()[0].clips()[0].content())
                .sequence_id == replacement.sequence_id);

    const auto mapped_note = std::find_if(mapping.begin(), mapping.end(), [](const auto& entry) {
        return entry.first == ItemId{13};
    });
    REQUIRE(mapped_note != mapping.end());
    const auto mapped_id = [&](ItemId old_id) {
        const auto found = std::find_if(mapping.begin(), mapping.end(),
                                        [&](const auto& entry) { return entry.first == old_id; });
        REQUIRE(found != mapping.end());
        return found->second;
    };
    auto edited = reduce_transaction(
        diverged->project,
        transaction(2, 3,
                    {SetNoteVelocity{replacement.sequence_id, mapped_id({11}), mapped_id({12}),
                                     mapped_note->second, 40'000, 50'000}}));
    REQUIRE(edited);
    const auto& source_note =
        std::get<NoteContent>(edited->project.find_sequence({10})->tracks()[0].clips()[0].content())
            .notes()[0];
    REQUIRE(source_note.velocity == 40'000);

    auto undone = reduce_transaction(diverged->project, transaction(3, 4, diverged->inverses));
    REQUIRE(undone);
    REQUIRE_FALSE(undone->project.find_sequence(replacement.sequence_id));
    REQUIRE(
        std::get<SequenceRef>(undone->project.find_sequence({2})->tracks()[0].clips()[0].content())
            .sequence_id == ItemId{10});

    auto corrupt = plan.value();
    auto& corrupt_map = std::get<CloneSequence>(corrupt.commands[0].command).id_remap;
    corrupt_map.back().second = corrupt_map.front().second;
    auto rejected = reduce_transaction(original, corrupt);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
}

TEST_CASE("InsertSequence validates media and accounts for retained payloads") {
    const auto original = nested_note_project();
    auto invalid_clip = take(Clip::create({202}, {0}, {100}, MediaRef{{999}, {0}, 10}));
    auto invalid =
        take(Sequence::create({200}, "invalid", TickDuration{100}, {track(201, {invalid_clip})}));
    auto rejected = reduce_transaction(original, transaction(1, 1, {InsertSequence{invalid}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::MissingAsset);

    const std::string large_name(1u << 20u, 'x');
    auto retained = take(Sequence::create({300}, "retained", TickDuration{100}, std::nullopt, {},
                                          {SequenceMarker{{301}, large_name, {0}}}, {}));
    REQUIRE(retained_size(Command{InsertSequence{retained}}) >=
            large_name.size() + sizeof(SequenceMarker));

    auto harmony = take(ChordScaleLane::create(
        {ChordScaleEvent{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}}));
    auto with_harmony = take(Sequence::create({302}, "harmony", TickDuration{100}, std::nullopt,
                                              {}, {}, {}, std::move(harmony)));
    REQUIRE(retained_size(Command{InsertSequence{with_harmony}}) >=
            sizeof(InsertSequence) + sizeof(Sequence) + sizeof(ChordScaleEvent));

    auto without_harmony =
        take(Sequence::create({302}, "harmony", TickDuration{100}, std::nullopt, {}));
    REQUIRE_FALSE(equivalent(Command{InsertSequence{with_harmony}},
                             Command{InsertSequence{without_harmony}}));
}

TEST_CASE("Nested notes compile like a hand-flattened track and fan out dirty children") {
    const auto nested = nested_note_project(false, 2);
    auto nested_program = compile(shared(nested));
    const auto nested_events = nested_program->find_track({3})->arrangement_note_events();
    REQUIRE(nested_events.size() == 2);
    REQUIRE(nested_events[0].tick == TickPosition{600});
    REQUIRE(nested_events[1].tick == TickPosition{840});

    auto direct_clip = take(Clip::create({4}, {480}, {960}, note_content(13)));
    auto direct_root = take(Sequence::create({2}, "root", std::nullopt, {track(3, {direct_clip})}));
    ProjectInput direct_input{{1}, "direct", 100, {2}, {}, {direct_root}};
    auto direct_program = compile(shared(take(Project::create(std::move(direct_input)))));
    const auto direct_events = direct_program->find_track({3})->arrangement_note_events();
    REQUIRE(direct_events.size() == nested_events.size());
    for (std::size_t index = 0; index < direct_events.size(); ++index) {
        REQUIRE(direct_events[index].sample == nested_events[index].sample);
        REQUIRE(direct_events[index].tick == nested_events[index].tick);
        REQUIRE(direct_events[index].kind == nested_events[index].kind);
        REQUIRE(direct_events[index].pitch == nested_events[index].pitch);
    }

    DirtySet dirty({DirtyItem{{13}, {11}, {10}, DirtyFlags::Notes}});
    auto lowered = lower_dirty_set(nested, {2}, dirty);
    REQUIRE_FALSE(lowered.all);
    REQUIRE(lowered.tracks == std::vector<ItemId>{{3}, {5}});
}

TEST_CASE("Nested compile refuses unsupported child state and expansion overflow") {
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = shared(nested_note_project(true));
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(request));
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::NestedSequenceUnsupported);
    REQUIRE_FALSE(store.has_value());

    PlaybackProgramStore budget_store;
    InlineExecutor budget_executor;
    PlaybackProgramCompiler budget_compiler(budget_store, budget_executor,
                                            std::chrono::microseconds(0));
    request.project = shared(nested_note_project());
    request.document_revision = 2;
    request.max_expanded_note_events = 1;
    REQUIRE(budget_compiler.submit(request));
    REQUIRE(budget_compiler.status().has_error);
    REQUIRE(budget_compiler.status().last_error.code == CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE_FALSE(budget_store.has_value());

    auto first = take(Clip::create({12}, {0}, {100}, EmptyContent{}));
    auto second = take(Clip::create({13}, {100}, {100}, EmptyContent{}));
    auto child =
        take(Sequence::create({10}, "child", TickDuration{200}, {track(11, {first, second})}));
    auto root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 200)})}));
    auto clip_limited =
        take(Project::create(ProjectInput{{1}, "clip limited", 100, {2}, {}, {root, child}}));
    PlaybackProgramStore clip_store;
    InlineExecutor clip_executor;
    PlaybackProgramCompiler clip_compiler(clip_store, clip_executor, std::chrono::microseconds(0));
    request.project = shared(std::move(clip_limited));
    request.document_revision = 3;
    request.max_expanded_note_events = 100;
    request.audio_limits.max_clips = 2;
    REQUIRE(clip_compiler.submit(request));
    REQUIRE(clip_compiler.status().has_error);
    REQUIRE(clip_compiler.status().last_error.code == CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE_FALSE(clip_store.has_value());

    auto empty_leaf = take(Sequence::create({20}, "leaf", TickDuration{100}, {}));
    auto branch = take(
        Sequence::create({10}, "branch", TickDuration{300},
                         {track(11, {nested_clip(12, 20, 0, 100), nested_clip(13, 20, 100, 100),
                                     nested_clip(14, 20, 200, 100)})}));
    auto reference_root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 300)})}));
    auto reference_limited = take(Project::create(ProjectInput{
        {1}, "reference limited", 100, {2}, {}, {reference_root, branch, empty_leaf}}));
    PlaybackProgramStore reference_store;
    InlineExecutor reference_executor;
    PlaybackProgramCompiler reference_compiler(reference_store, reference_executor,
                                               std::chrono::microseconds(0));
    request.project = shared(std::move(reference_limited));
    request.document_revision = 4;
    request.audio_limits.max_clips = 3;
    REQUIRE(reference_compiler.submit(request));
    REQUIRE(reference_compiler.status().has_error);
    REQUIRE(reference_compiler.status().last_error.code ==
            CompileErrorCode::ExpansionBudgetExceeded);

    auto skipped_a = take(Clip::create({12}, {100}, {10}, EmptyContent{}));
    auto skipped_b = take(Clip::create({13}, {120}, {10}, EmptyContent{}));
    auto skipped_c = take(Clip::create({14}, {140}, {10}, EmptyContent{}));
    auto sparse_child = take(Sequence::create({10}, "sparse child", TickDuration{200},
                                              {track(11, {skipped_a, skipped_b, skipped_c})}));
    auto sparse_root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 10)})}));
    auto scan_limited = take(Project::create(
        ProjectInput{{1}, "scan limited", 100, {2}, {}, {sparse_root, sparse_child}}));
    PlaybackProgramStore scan_store;
    InlineExecutor scan_executor;
    PlaybackProgramCompiler scan_compiler(scan_store, scan_executor, std::chrono::microseconds(0));
    request.project = shared(std::move(scan_limited));
    request.document_revision = 5;
    request.audio_limits.max_clips = 3;
    REQUIRE(scan_compiler.submit(request));
    REQUIRE(scan_compiler.status().has_error);
    REQUIRE(scan_compiler.status().last_error.code == CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE(scan_compiler.status().last_error.item == ItemId{14});
    REQUIRE_FALSE(scan_store.has_value());

    auto skipped_notes = take(NoteContent::create(
        {NoteEvent{{20}, {20}, {10}, 40'000, 64, 0},
         NoteEvent{{21}, {40}, {10}, 40'000, 64, 0}}));
    auto note_scan_clip =
        take(Clip::create({12}, {0}, {100}, std::move(skipped_notes)));
    auto note_scan_child = take(Sequence::create(
        {10}, "note scan child", TickDuration{100}, {track(11, {note_scan_clip})}));
    auto note_scan_root =
        take(Sequence::create({2}, "root", std::nullopt,
                              {track(3, {nested_clip(4, 10, 0, 10)})}));
    auto note_scan_limited = take(Project::create(ProjectInput{
        {1}, "note scan limited", 100, {2}, {}, {note_scan_root, note_scan_child}}));
    PlaybackProgramStore note_scan_store;
    InlineExecutor note_scan_executor;
    PlaybackProgramCompiler note_scan_compiler(
        note_scan_store, note_scan_executor, std::chrono::microseconds(0));
    request.project = shared(std::move(note_scan_limited));
    request.document_revision = 6;
    request.max_expanded_note_events = 2;
    request.audio_limits = {};
    REQUIRE(note_scan_compiler.submit(request));
    REQUIRE(note_scan_compiler.status().has_error);
    REQUIRE(note_scan_compiler.status().last_error.code ==
            CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE(note_scan_compiler.status().last_error.item == ItemId{12});
    REQUIRE_FALSE(note_scan_store.has_value());

    auto invalid_notes = take(NoteContent::create({NoteEvent{{20}, {90}, {20}, 40'000, 64, 0}}));
    auto invalid_note_clip = take(Clip::create({12}, {0}, {100}, invalid_notes));
    auto invalid_child =
        take(Sequence::create({10}, "child", TickDuration{100}, {track(11, {invalid_note_clip})}));
    auto invalid_root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 100)})}));
    auto invalid_note_project = take(Project::create(
        ProjectInput{{1}, "invalid nested note", 100, {2}, {}, {invalid_root, invalid_child}}));
    PlaybackProgramStore invalid_note_store;
    InlineExecutor invalid_note_executor;
    PlaybackProgramCompiler invalid_note_compiler(invalid_note_store, invalid_note_executor,
                                                  std::chrono::microseconds(0));
    request.project = shared(std::move(invalid_note_project));
    request.document_revision = 7;
    request.audio_limits = {};
    REQUIRE(invalid_note_compiler.submit(request));
    REQUIRE(invalid_note_compiler.status().has_error);
    REQUIRE(invalid_note_compiler.status().last_error.code == CompileErrorCode::InvalidStructure);

    std::vector<float> audio(24'000, 1.0f);
    const auto audio_data_value = audio_data({audio});
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));
    auto faded_media =
        musical_media_clip(12, 0, kTicksPerQuarter, 50, audio.size(),
                           {.gain_linear = 1.0f,
                            .fade_in_duration = static_cast<std::uint64_t>(kTicksPerQuarter / 2)});
    auto faded_child = take(Sequence::create({10}, "child", TickDuration{kTicksPerQuarter},
                                             {track(11, {faded_media})}));
    auto faded_root = take(Sequence::create(
        {2}, "root", std::nullopt,
        {track(3, {nested_clip(4, 10, 0, 3 * kTicksPerQuarter / 4, kTicksPerQuarter / 4)})}));
    ProjectInput faded_input;
    faded_input.id = {1};
    faded_input.name = "partial fade";
    faded_input.next_item_id = 100;
    faded_input.root_sequence_id = {2};
    faded_input.assets = {{50, "audio", audio.size(), {48'000, 1}, hash}};
    faded_input.sequences = {faded_root, faded_child};
    PlaybackProgramStore fade_store;
    InlineExecutor fade_executor;
    PlaybackProgramCompiler fade_compiler(fade_store, fade_executor, std::chrono::microseconds(0));
    request.project = shared(take(Project::create(std::move(faded_input))));
    request.document_revision = 6;
    request.audio_assets = pool({{{50}, audio_data_value}});
    REQUIRE(fade_compiler.submit(request));
    REQUIRE(fade_compiler.status().has_error);
    REQUIRE(fade_compiler.status().last_error.code == CompileErrorCode::NestedSequenceUnsupported);

    auto truncated_root = take(Sequence::create(
        {2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, kTicksPerQuarter / 4, 0)})}));
    faded_input.id = {1};
    faded_input.name = "opposite partial fade";
    faded_input.next_item_id = 100;
    faded_input.root_sequence_id = {2};
    faded_input.assets = {{50, "audio", audio.size(), {48'000, 1}, hash}};
    faded_input.sequences = {truncated_root, faded_child};
    PlaybackProgramStore truncated_store;
    InlineExecutor truncated_executor;
    PlaybackProgramCompiler truncated_compiler(truncated_store, truncated_executor,
                                               std::chrono::microseconds(0));
    request.project = shared(take(Project::create(std::move(faded_input))));
    request.document_revision = 8;
    REQUIRE(truncated_compiler.submit(request));
    REQUIRE(truncated_compiler.status().has_error);
    REQUIRE(truncated_compiler.status().last_error.code ==
            CompileErrorCode::NestedSequenceUnsupported);
}

TEST_CASE("Incremental nested compilation charges reused expansion") {
    auto project = shared(nested_note_project(false, 2));
    auto tempo = map_120();
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest first;
    first.project = project;
    first.sequence_id = {2};
    first.tempo_map = tempo;
    first.document_revision = 1;
    first.dirty.all = true;
    first.max_expanded_note_events = 4;
    REQUIRE(compiler.submit(first));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(store.has_value());

    auto incremental = first;
    incremental.document_revision = 2;
    incremental.dirty.all = false;
    incremental.dirty.tracks = {{3}};
    incremental.max_expanded_note_events = 2;
    REQUIRE(compiler.submit(std::move(incremental)));
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE(store.read()->document_revision() == 1);
}

TEST_CASE("Incremental nested compilation preserves generated identities") {
    auto project = shared(nested_note_project(false, 2));
    auto tempo = map_120();
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = tempo;
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(request));
    REQUIRE_FALSE(compiler.status().has_error);

    const auto first = store.read();
    const auto* first_track = first->find_track({3});
    const auto* second_track = first->find_track({5});
    REQUIRE(first_track);
    REQUIRE(second_track);
    REQUIRE(first_track->generated_id_count() == 1);
    REQUIRE(second_track->generated_id_count() == 1);
    REQUIRE(first_track->generated_id_start() == 100);
    REQUIRE(second_track->generated_id_start() == 101);
    REQUIRE(first_track->ordered_clip_ids().back() == ItemId{100});
    REQUIRE(second_track->ordered_clip_ids().back() == ItemId{101});

    request.document_revision = 2;
    request.dirty.all = false;
    request.dirty.tracks = {{5}};
    REQUIRE(compiler.submit(request));
    REQUIRE_FALSE(compiler.status().has_error);
    const auto second = store.read();
    REQUIRE(second->find_track({3}) == first_track);
    REQUIRE(second->find_track({5}) != second_track);
    REQUIRE(second->find_track({3})->ordered_clip_ids().back() == ItemId{100});
    REQUIRE(second->find_track({5})->ordered_clip_ids().back() == ItemId{101});

    request.document_revision = 3;
    request.dirty.tracks = {{3}};
    REQUIRE(compiler.submit(request));
    REQUIRE_FALSE(compiler.status().has_error);
    const auto third = store.read();
    REQUIRE(third->find_track({3}) != second->find_track({3}));
    REQUIRE(third->find_track({5}) == second->find_track({5}));
    REQUIRE(third->find_track({3})->ordered_clip_ids().back() == ItemId{100});
    REQUIRE(third->find_track({5})->ordered_clip_ids().back() == ItemId{101});

    auto shortened = reduce_transaction(
        *project,
        transaction(1, 1, {SetClipSequenceRef{{2}, {3}, {4}, {{10}, {0}}, {{10}, {960}}}}));
    REQUIRE(shortened);
    request.project = shared(std::move(shortened->project));
    request.document_revision = 4;
    REQUIRE(compiler.submit(request));
    REQUIRE_FALSE(compiler.status().has_error);
    const auto fourth = store.read();
    REQUIRE(fourth->generated_id_base() == 100);
    REQUIRE(fourth->find_track({3}) != third->find_track({3}));
    REQUIRE(fourth->find_track({3})->generated_id_count() == 0);
    REQUIRE(fourth->find_track({3})->generated_id_start() == 100);
    REQUIRE(fourth->find_track({5})->generated_id_start() == 100);
    REQUIRE(fourth->find_track({5}) != third->find_track({5}));
    REQUIRE(fourth->find_track({5})->ordered_clip_ids().back() == ItemId{100});
}

TEST_CASE("Nested audio trimming rejects projected-start underflow") {
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));
    auto child_clip = take(Clip::create({12}, {-500}, {1'000}, MediaRef{{50}, {0}, 24'000}));
    auto child = take(Sequence::create({10}, "child", std::nullopt, {track(11, {child_clip})}));
    auto root = take(Sequence::create(
        {2}, "root", std::nullopt,
        {track(3, {nested_clip(4, 10, std::numeric_limits<std::int64_t>::min() + 100, 100, 0)})}));
    auto project =
        shared(take(Project::create(ProjectInput{{1},
                                                 "target underflow",
                                                 100,
                                                 {2},
                                                 {{50, "audio", 24'000, {48'000, 1}, hash}},
                                                 {root, child}})));

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::InvalidStructure);
    REQUIRE(compiler.status().last_error.item == ItemId{12});
}

TEST_CASE("Nested audio trimming handles saturated sample distance") {
    constexpr std::int64_t child_start = -4'000'000'000'000'000'000;
    constexpr std::int64_t child_duration = 4'000'000'000'000'001'000;
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));
    auto child_clip =
        take(Clip::create({12}, {child_start}, {child_duration}, MediaRef{{50}, {0}, 24'000}));
    auto child = take(Sequence::create({10}, "child", std::nullopt, {track(11, {child_clip})}));
    auto root = take(
        Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 1'000, 0)})}));
    auto project =
        shared(take(Project::create(ProjectInput{{1},
                                                 "sample saturation",
                                                 100,
                                                 {2},
                                                 {{50, "audio", 24'000, {48'000, 1}, hash}},
                                                 {root, child}})));
    const std::array points{TempoPoint{{0}, 1.0}};
    auto tempo = shared_compiled_tempo_map(points, RationalRate{48'000, 1});

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = std::move(tempo);
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(store.has_value());
    REQUIRE(store.read()->find_track({3})->generated_id_count() == 1);
}

TEST_CASE("Nested note clipping consumes compile slice work units") {
    std::vector<NoteEvent> notes;
    for (std::uint64_t index = 0; index < 128; ++index)
        notes.push_back(
            {{1'000 + index}, {static_cast<std::int64_t>(index * 100)}, {50}, 40'000, 64, 0});
    auto content = take(NoteContent::create(std::move(notes)));
    auto child_clip = take(Clip::create({12}, {0}, {12'800}, content));
    auto child =
        take(Sequence::create({10}, "child", TickDuration{12'800}, {track(11, {child_clip})}));
    auto root = take(
        Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 12'800)})}));
    auto project =
        take(Project::create(ProjectInput{{1}, "slice bounded", 2'000, {2}, {}, {root, child}}));
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = shared(std::move(project));
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(std::move(request)));
    INFO("compile error " << static_cast<int>(compiler.status().last_error.code) << " item "
                          << compiler.status().last_error.item.value);
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(store.has_value());
    // max_work_units is one in InlineExecutor. The clipping pass and the
    // ordinary note compiler must therefore consume separate slices.
    REQUIRE(executor.slice_count > 256);
}

TEST_CASE("Nested audio renders sample-identically to explicit flattening") {
    std::vector<float> ramp(24'000);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame + 1) / static_cast<float>(ramp.size());
    const auto data = audio_data({ramp});
    const auto assets = pool({{{50}, data}});
    CompiledFixture nested(shared(nested_audio_project(data)), map_120(), assets);

    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));
    auto direct_media = musical_media_clip(4, kTicksPerQuarter, kTicksPerQuarter, 50, ramp.size());
    auto direct_project = project_with_tracks({track(3, {direct_media})},
                                              {{50, "ramp", ramp.size(), {48'000, 1}, hash}});
    CompiledFixture flattened(direct_project, map_120(), assets);
    auto nested_program = nested.store.read();
    auto flattened_program = flattened.store.read();
    Output nested_output(1, 256);
    Output flattened_output(1, 256);
    AudioRenderStatus nested_status = AudioRenderStatus::InvalidProgram;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        nested_status = ArrangementAudioRenderer::process(
            *nested_program, snapshot(*nested_program, 256, 24'100), nested_output.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(nested_status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);
    REQUIRE(ArrangementAudioRenderer::process(
                *flattened_program, snapshot(*flattened_program, 256, 24'100),
                flattened_output.view()) == AudioRenderStatus::Rendered);
    REQUIRE(nested_output.storage == flattened_output.storage);
    REQUIRE(nested_output.storage[0][0] != 0.0f);

    Output shifted(1, 256);
    REQUIRE(ArrangementAudioRenderer::process(*flattened_program,
                                              snapshot(*flattened_program, 256, 24'101),
                                              shifted.view()) == AudioRenderStatus::Rendered);
    REQUIRE(shifted.storage != nested_output.storage);
}

TEST_CASE("Nested audio trimming preserves fractional sample-rate conversion offset") {
    std::vector<float> source(4'410);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = static_cast<float>(std::sin(static_cast<double>(frame) * 0.071) * 0.75);
    const auto data = audio_data({source}, 44'100);
    const auto assets = pool({{{50}, data}});
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));

    auto child_media = musical_media_clip(12, 0, 30'000, 50, source.size());
    auto child =
        take(Sequence::create({10}, "child", TickDuration{30'000}, {track(11, {child_media})}));
    auto root =
        take(Sequence::create({2}, "root", std::nullopt,
                              {track(3, {nested_clip(4, 10, kTicksPerQuarter, 15'000, 735)})}));
    auto nested_project =
        shared(take(Project::create(ProjectInput{{1},
                                                 "fractional nested audio",
                                                 100,
                                                 {2},
                                                 {{50, "source", source.size(), {44'100, 1}, hash}},
                                                 {root, child}})));
    CompiledFixture nested(nested_project, map_120(), assets);

    auto direct_media = musical_media_clip(4, 0, 30'000, 50, source.size());
    auto direct_project = project_with_tracks({track(3, {direct_media})},
                                              {{50, "source", source.size(), {44'100, 1}, hash}});
    CompiledFixture direct(direct_project, map_120(), assets);

    auto nested_program = nested.store.read();
    auto direct_program = direct.store.read();
    const auto nested_audio = nested_program->find_track({3})->audio_program()->clips();
    REQUIRE(nested_audio.size() == 1);
    INFO("nested audio id " << nested_audio.front().id.value << " source offset "
                            << nested_audio.front().source_frame_offset);
    REQUIRE(std::abs(nested_audio.front().source_frame_offset - 22.96875) < 1.0e-12);

    Output nested_output(1, 256);
    Output direct_output(1, 256);
    REQUIRE(ArrangementAudioRenderer::process(*nested_program,
                                              snapshot(*nested_program, 256, 24'000),
                                              nested_output.view()) == AudioRenderStatus::Rendered);
    REQUIRE(ArrangementAudioRenderer::process(*direct_program, snapshot(*direct_program, 256, 25),
                                              direct_output.view()) == AudioRenderStatus::Rendered);
    REQUIRE(nested_output.storage == direct_output.storage);
}
