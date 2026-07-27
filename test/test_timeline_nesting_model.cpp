#include "timeline_nesting_test_support.hpp"

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
        Clip::create({1}, {0}, {1}, SequenceRef{{2}, {std::numeric_limits<std::int64_t>::max()}});
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
    auto plan = build_diverge_transaction(original, location, transaction_id, {}, clone_command_id,
                                          retarget_command_id);
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
    auto with_harmony = take(Sequence::create({302}, "harmony", TickDuration{100}, std::nullopt, {},
                                              {}, {}, std::move(harmony)));
    REQUIRE(retained_size(Command{InsertSequence{with_harmony}}) >=
            sizeof(InsertSequence) + sizeof(Sequence) + sizeof(ChordScaleEvent));

    auto without_harmony =
        take(Sequence::create({302}, "harmony", TickDuration{100}, std::nullopt, {}));
    REQUIRE_FALSE(equivalent(Command{InsertSequence{with_harmony}},
                             Command{InsertSequence{without_harmony}}));
}
