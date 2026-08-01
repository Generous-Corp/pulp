#include "timeline_nesting_test_support.hpp"
#include "../core/playback/src/sequence_content_lowerer.hpp"

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
    const auto index = CompileInvalidationIndex::build(nested, {2}, CompileContextRegistry{});
    auto lowered = resolve_dirty_tracks(nested, {2}, dirty, index);
    REQUIRE_FALSE(lowered.all);
    REQUIRE(lowered.tracks == std::vector<ItemId>{{3}, {5}});

    const DirtySet child_metadata({DirtyItem{{30}, {}, {10}, DirtyFlags::Marker},
                                   DirtyItem{{10}, {}, {10}, DirtyFlags::Context}},
                                  {{{10}, CompileContextKind::ChordScale}});
    const auto metadata_lowered = resolve_dirty_tracks(nested, {2}, child_metadata, index);
    REQUIRE_FALSE(metadata_lowered.all);
    REQUIRE(metadata_lowered.tracks.empty());

    const DirtySet root_metadata({DirtyItem{{31}, {}, {2}, DirtyFlags::Marker}});
    const auto root_metadata_lowered = resolve_dirty_tracks(nested, {2}, root_metadata, index);
    REQUIRE_FALSE(root_metadata_lowered.all);
    REQUIRE(root_metadata_lowered.tracks.empty());

    const auto notes_and_metadata =
        static_cast<DirtyFlags>(static_cast<std::uint16_t>(DirtyFlags::Notes) |
                                static_cast<std::uint16_t>(DirtyFlags::Marker));
    const DirtySet combined({DirtyItem{{13}, {11}, {10}, notes_and_metadata}});
    REQUIRE(resolve_dirty_tracks(nested, {2}, combined, index).tracks ==
            std::vector<ItemId>{{3}, {5}});
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
    request.max_expanded_clips = 2;
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
    request.max_expanded_clips = 3;
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
    request.max_expanded_clips = 3;
    REQUIRE(scan_compiler.submit(request));
    REQUIRE(scan_compiler.status().has_error);
    REQUIRE(scan_compiler.status().last_error.code == CompileErrorCode::ExpansionBudgetExceeded);
    REQUIRE(scan_compiler.status().last_error.item == ItemId{14});
    REQUIRE_FALSE(scan_store.has_value());

    auto skipped_notes = take(MidiContent::create(
        {NoteEvent{{20}, {20}, {10}, 40'000, 64, 0}, NoteEvent{{21}, {40}, {10}, 40'000, 64, 0}}));
    auto note_scan_clip = take(Clip::create({12}, {0}, {100}, std::move(skipped_notes)));
    auto note_scan_child = take(Sequence::create({10}, "note scan child", TickDuration{100},
                                                 {track(11, {note_scan_clip})}));
    auto note_scan_root =
        take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 0, 10)})}));
    auto note_scan_limited = take(Project::create(
        ProjectInput{{1}, "note scan limited", 100, {2}, {}, {note_scan_root, note_scan_child}}));
    PlaybackProgramStore note_scan_store;
    InlineExecutor note_scan_executor;
    PlaybackProgramCompiler note_scan_compiler(note_scan_store, note_scan_executor,
                                               std::chrono::microseconds(0));
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

    auto invalid_notes = take(MidiContent::create({NoteEvent{{20}, {90}, {20}, 40'000, 64, 0}}));
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

TEST_CASE("Audio clip limits do not cap non-audio sequence expansion") {
    auto first = take(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto second = take(Clip::create({5}, {100}, {100}, note_content(6, 0, 50)));
    auto root = take(Sequence::create({2}, "root", TickDuration{200}, {track(3, {first, second})}));
    auto project =
        shared(take(Project::create(ProjectInput{{1}, "non-audio limits", 100, {2}, {}, {root}})));

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_limits.max_clips = 1;
    request.max_expanded_clips = 2;

    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(store.has_value());
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
    auto content = take(MidiContent::create(std::move(notes)));
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

TEST_CASE("Nested conforming audio refuses partial source windows") {
    std::vector<float> source(24'000, 1.0f);
    const auto data = audio_data({source});
    const auto assets = pool({{{50}, data}});
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));

    const std::array windows{
        std::pair{kTicksPerQuarter / 4, 3 * kTicksPerQuarter / 4}, // left trim only
        std::pair{0LL, 3 * kTicksPerQuarter / 4},                  // right trim only
        std::pair{kTicksPerQuarter / 4, kTicksPerQuarter / 2},    // both sides
    };
    for (const auto conform : {TimeConform::Resample, TimeConform::Stretch}) {
        for (const auto [source_start, duration] : windows) {
            auto child_media = musical_media_clip(12, 0, kTicksPerQuarter, 50, source.size(), {},
                                                   conform);
            auto child = take(Sequence::create({10}, "child", TickDuration{kTicksPerQuarter},
                                               {track(11, {child_media})}));
            auto root = take(Sequence::create(
                {2}, "root", std::nullopt,
                {track(3, {nested_clip(4, 10, 0, duration, source_start)})}));
            auto project = shared(take(Project::create(
                ProjectInput{{1}, "partial nested conform", 100, {2},
                             {{50, "source", source.size(), {48'000, 1}, hash}}, {root, child}})));

            PlaybackProgramStore store;
            InlineExecutor executor;
            PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
            ProgramCompileRequest request;
            request.project = std::move(project);
            request.sequence_id = {2};
            request.tempo_map = map_120();
            request.document_revision = 1;
            request.dirty.all = true;
            request.audio_assets = assets;
            REQUIRE(compiler.submit(std::move(request)));
            REQUIRE(compiler.status().has_error);
            REQUIRE(compiler.status().last_error.code ==
                    CompileErrorCode::NestedSequenceUnsupported);
            REQUIRE(compiler.status().last_error.item == ItemId{12});
            REQUIRE_FALSE(store.has_value());
        }
    }
}

TEST_CASE("Complete nested media preserves authored time conform intent") {
    const auto hash = *ContentHash::from_hex(std::string(64, 'a'));
    for (const auto conform : {TimeConform::Resample, TimeConform::Stretch}) {
        auto child_media =
            musical_media_clip(12, 0, kTicksPerQuarter, 50, 24'000, {}, conform);
        auto child = take(Sequence::create({10}, "child", TickDuration{kTicksPerQuarter},
                                           {track(11, {child_media})}));
        auto root = take(Sequence::create(
            {2}, "root", std::nullopt,
            {track(3, {nested_clip(4, 10, 0, kTicksPerQuarter, 0)})}));
        auto project = take(Project::create(
            ProjectInput{{1}, "complete nested conform", 100, {2},
                         {{50, "source", 24'000, {48'000, 1}, hash}}, {root, child}}));
        const auto tempo = map_120();
        SequenceContentLowerer lowerer(project, *tempo, 100, 100);
        std::vector<LoweredClip> lowered;
        const auto begun = lowerer.begin_track(*project.find_sequence({2})->find_track({3}), lowered);
        REQUIRE_FALSE(begun.error);
        for (;;) {
            const auto step = lowerer.step();
            REQUIRE_FALSE(step.error);
            if (step.complete)
                break;
        }
        REQUIRE(lowered.size() == 2);
        REQUIRE(lowered[0].clip.id() == ItemId{4});
        REQUIRE(lowered[0].clip.time_conform() == TimeConform::None);
        REQUIRE(lowered[1].clip.time_conform() == conform);
    }
}

namespace {

// A child sequence whose single note clip carries modifiers and an authored
// seed, referenced once from the root. `child_note_start`/`child_note_duration`
// let a case place the note so that clipping keeps it, trims it, or removes it
// entirely.
Project modified_nested_project(std::vector<NoteModifier> modifiers, std::uint64_t seed,
                                std::int64_t child_note_start = 120,
                                std::int64_t child_note_duration = 240,
                                std::int64_t reference_source_start = 0,
                                std::int64_t reference_duration = 960) {
    auto notes = take(MidiContent::create(
        {NoteEvent{{13}, {child_note_start}, {child_note_duration}, 40'000, 64, 0},
         NoteEvent{{15}, {600}, {120}, 40'000, 67, 0}},
        std::move(modifiers), seed));
    auto child_clip = take(Clip::create({12}, {0}, {960}, std::move(notes)));
    auto child = take(Sequence::create({10}, "child", TickDuration{960}, {track(11, {child_clip})}));
    auto root = take(Sequence::create(
        {2}, "root", std::nullopt,
        {track(3, {nested_clip(4, 10, 480, reference_duration, reference_source_start)})}));
    ProjectInput input;
    input.id = {1};
    input.name = "modified-nested";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

NoteModifier certain_ratchet(std::uint64_t note_id, std::uint16_t ratchets) {
    NoteModifier modifier;
    modifier.note_id = {note_id};
    modifier.ratchet_count = ratchets;
    return modifier;
}

} // namespace

TEST_CASE("A nested note clip keeps its modifiers and authored seed") {
    // Ratchet is authored rather than drawn, so it is observable in the compiled
    // event stream without depending on a probability draw: a note that ratchets
    // three times emits three on/off pairs instead of one.
    const auto project =
        modified_nested_project({certain_ratchet(13, 3)}, 0xC0FFEEULL);
    auto program = compile(shared(project));
    const auto events = program->find_track({3})->arrangement_note_events();

    // Note 13 ratcheted x3 plus note 15 unmodified = 4 note-ons.
    std::size_t note_ons = 0;
    for (const auto& event : events)
        if (event.kind == NoteProgramEventKind::On)
            ++note_ons;
    REQUIRE(note_ons == 4);
}

TEST_CASE("A nested note clip drops modifiers whose notes were trimmed away") {
    // The reference starts one tick after note 13 ends, so note 13 is entirely
    // outside the audible window and is removed by clipping. Its modifier must
    // be dropped with it: MidiContent::create refuses a modifier that names an
    // absent note, so passing the companion array through unfiltered would turn
    // this nested sequence into a compile error rather than a valid lowering.
    const auto project = modified_nested_project(
        {certain_ratchet(13, 3)}, 0xC0FFEEULL, 0, 240, /*reference_source_start=*/480);
    auto program = compile(shared(project));
    const auto track = program->find_track({3});
    REQUIRE(track != nullptr);

    // Note 15 survives at source tick 600; note 13 and its modifier are gone.
    std::size_t note_ons = 0;
    for (const auto& event : track->arrangement_note_events())
        if (event.kind == NoteProgramEventKind::On)
            ++note_ons;
    REQUIRE(note_ons == 1);
}

TEST_CASE("A nested note clip carries its modifiers through a partial clip") {
    // Note 13 spans the reference boundary, so clipping shortens it but keeps
    // its identity — and therefore its modifier, because a clipped note retains
    // its id.
    const auto project = modified_nested_project({certain_ratchet(13, 2)}, 7,
                                                 /*child_note_start=*/400,
                                                 /*child_note_duration=*/400,
                                                 /*reference_source_start=*/500);
    auto program = compile(shared(project));
    std::size_t note_ons = 0;
    for (const auto& event : program->find_track({3})->arrangement_note_events())
        if (event.kind == NoteProgramEventKind::On)
            ++note_ons;
    // Note 13 survives partially and still ratchets twice; note 15 survives once.
    REQUIRE(note_ons == 3);
}

namespace {

// A nested project whose CHILD track carries the supplied mixer state. A default
// TrackMixer is transparent, so the same builder produces both the refusal case
// and its positive control.
Project nested_project_with_child_mixer(TrackMixer child_mixer) {
    auto child_clip = take(Clip::create({12}, {0}, {960}, note_content(13)));
    TrackInput child_input;
    child_input.id = {11};
    child_input.name = "child";
    child_input.clips = {child_clip};
    child_input.mixer = child_mixer;
    auto child_track = take(Track::create(std::move(child_input)));
    auto child = take(Sequence::create({10}, "child", TickDuration{960}, {child_track}));
    auto root = take(Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10)})}));
    ProjectInput input;
    input.id = {1};
    input.name = "nested-mixer";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

CompileError compile_error_for(const Project& project) {
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = shared(project);
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(std::move(request)));
    const auto status = compiler.status();
    REQUIRE(status.has_error);
    return status.last_error;
}

} // namespace

TEST_CASE("A nested child track carrying a non-transparent mixer is refused") {
    // Flattening would fold the child into the parent with nowhere to put its
    // fader, so its gain would silently stop applying. The compiler must refuse
    // and name the offending track rather than play it at unity.
    const auto gained = nested_project_with_child_mixer(TrackMixer{0.5f, 0.0f});
    const auto gain_error = compile_error_for(gained);
    REQUIRE(gain_error.code == CompileErrorCode::NestedSequenceUnsupported);
    REQUIRE(gain_error.item == ItemId{11});

    // Pan is refused on the same grounds as gain.
    const auto panned = nested_project_with_child_mixer(TrackMixer{1.0f, -0.5f});
    const auto pan_error = compile_error_for(panned);
    REQUIRE(pan_error.code == CompileErrorCode::NestedSequenceUnsupported);
    REQUIRE(pan_error.item == ItemId{11});
}

TEST_CASE("A nested child track with a transparent mixer still lowers") {
    // The positive control for the refusal above: without it, a guard that
    // rejected every nested track would pass the refusal test just as well.
    const auto project = nested_project_with_child_mixer(TrackMixer{});
    auto program = compile(shared(project));
    const auto events = program->find_track({3})->arrangement_note_events();
    REQUIRE_FALSE(events.empty());
}

namespace {

// One controller stream a clip can carry beside its notes. The address is a
// plain channel-voice controller, and the identities stay clear of the note
// identity the builders below use.
MidiExpressionLane controller_lane() {
    return MidiExpressionLane{
        {20}, MidiLaneAddress{0, 0, 11, 0, 74}, {{{21}, {0}, 0}, {{22}, {240}, 0xffffffff}}};
}

// A flat track holding one MIDI clip, with or without a controller lane, so the
// refusal and its control differ in exactly the lane.
Project flat_note_project(std::vector<MidiExpressionLane> lanes) {
    auto content = take(MidiContent::create({NoteEvent{{13}, {120}, {240}, 40'000, 64, 0}}, {}, 0,
                                            std::move(lanes)));
    auto clip = take(Clip::create({12}, {0}, {960}, std::move(content)));
    auto root = take(Sequence::create({2}, "root", TickDuration{960}, {track(3, {clip})}));
    ProjectInput input;
    input.id = {1};
    input.name = "flat notes";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.sequences = {root};
    return take(Project::create(std::move(input)));
}

// A lane-bearing child clip the placement trims: the reference admits the first
// half of the child, so the retained window ends before the child does.
Project trimmed_nested_lane_project() {
    auto content = take(MidiContent::create({NoteEvent{{13}, {120}, {240}, 40'000, 64, 0}}, {}, 0,
                                            {controller_lane()}));
    auto child_clip = take(Clip::create({12}, {0}, {960}, std::move(content)));
    auto child =
        take(Sequence::create({10}, "child", TickDuration{960}, {track(11, {child_clip})}));
    auto root = take(
        Sequence::create({2}, "root", std::nullopt, {track(3, {nested_clip(4, 10, 480, 480)})}));
    ProjectInput input;
    input.id = {1};
    input.name = "trimmed nested lane";
    input.next_item_id = 100;
    input.root_sequence_id = {2};
    input.sequences = {root, child};
    return take(Project::create(std::move(input)));
}

} // namespace

TEST_CASE("A clip carrying expression lanes is refused instead of compiled without them") {
    // The compiler reads only the notes, so compiling this clip would publish a
    // program whose controllers silently stopped existing. The refusal names the
    // clip that carries them, and nothing reaches the store: a published
    // lane-less program is the loss this guard exists to prevent, so the empty
    // store is the assertion that matters as much as the code.
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = shared(flat_note_project({controller_lane()}));
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    REQUIRE(compiler.submit(std::move(request)));
    const auto status = compiler.status();
    REQUIRE(status.has_error);
    REQUIRE(status.last_error.code == CompileErrorCode::MidiExpressionLaneUnsupported);
    REQUIRE(status.last_error.item == ItemId{12});
    REQUIRE_FALSE(store.has_value());
}

TEST_CASE("A clip with no expression lanes still compiles") {
    // The positive control for the refusal above: a guard that rejected every
    // MIDI clip would pass the refusal test just as well.
    const auto program = compile(shared(flat_note_project({})));
    const auto events = program->find_track({3})->arrangement_note_events();
    REQUIRE(events.size() == 2);
}

TEST_CASE("Trimming a lane-bearing nested clip keeps its own refusal") {
    // Lowering refuses the trim before the compiler ever sees the clip, and it
    // says something different: the inherited value at a trim boundary is
    // undefined, which outlives the missing lane support the code above names.
    const auto error = compile_error_for(trimmed_nested_lane_project());
    REQUIRE(error.code == CompileErrorCode::TrimmedMidiLaneUnsupported);
    REQUIRE(error.item == ItemId{12});
}
