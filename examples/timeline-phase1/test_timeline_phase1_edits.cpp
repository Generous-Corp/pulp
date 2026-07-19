#include "timeline_phase1_example_test_support.hpp"

TEST_CASE("timeline step mixed batch rolls back document program and render") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    const auto* project_before = processor.persistent_project();
    REQUIRE(project_before);
    const auto* clip_before =
        project_before->find_sequence({3})->find_track({4})->find_clip({5});
    const auto* content_before =
        std::get_if<timeline::RegisteredContent>(&clip_before->content());
    REQUIRE(content_before);
    const auto payload_before = content_before->canonical_payload_json();
    const auto document_before = processor.pattern_snapshot();
    const auto* tempo_map_before = processor.last_transport().tempo_map;
    TimelineStepSequencerProcessor render_reference;
    render_reference.prepare(prepare_context());
    REQUIRE(render_reference.engine_prepared());
    REQUIRE(render_reference.seek_samples(0) == playback::TransportError::None);
    StereoBlock render_before(128);
    process_direct(render_reference, render_before);

    state::StepEditCommand valid;
    valid.client_sequence = 51;
    valid.transaction_id = 151;
    valid.kind = state::StepEditKind::SetCell;
    valid.payload.set_cell = {0, 0, 0, document_before.patterns[0].lanes[0][0]};
    valid.payload.set_cell.cell.pitch_offset = 7;

    state::StepEditCommand out_of_extent;
    out_of_extent.client_sequence = 52;
    out_of_extent.transaction_id = 152;
    out_of_extent.kind = state::StepEditKind::SwitchPattern;
    out_of_extent.payload.switch_pattern.pattern = 1;

    state::StepEditCommand invalid_final;
    invalid_final.client_sequence = 53;
    invalid_final.transaction_id = 153;
    invalid_final.kind = state::StepEditKind::SetCell;
    invalid_final.payload.set_cell = {
        0, 0, 15, document_before.patterns[0].lanes[0][15]};
    invalid_final.payload.set_cell.cell.flags = state::StepCell::kEnabledBit;
    invalid_final.payload.set_cell.cell.velocity = 100;
    invalid_final.payload.set_cell.cell.gate_ticks = 65535;

    REQUIRE(processor.channel().ui_try_submit(valid));
    REQUIRE(processor.channel().ui_try_submit(out_of_extent));
    REQUIRE(processor.channel().ui_try_submit(invalid_final));
    REQUIRE_FALSE(processor.apply_pending_edits_and_recompile());

    const std::array expected_reasons{3u, 2u, 3u};
    const std::array expected_clients{51u, 52u, 53u};
    const std::array expected_transactions{151u, 152u, 153u};
    for (std::size_t index = 0; index < expected_reasons.size(); ++index) {
        const auto echo = processor.channel().ui_try_pop_applied();
        REQUIRE(echo);
        REQUIRE(echo->kind == state::AppliedEditKind::CommandRejected);
        REQUIRE(echo->engine_sequence == index + 1);
        REQUIRE(echo->snapshot_epoch == 1);
        REQUIRE(echo->client_sequence == expected_clients[index]);
        REQUIRE(echo->transaction_id == expected_transactions[index]);
        REQUIRE(echo->payload.reject_reason == expected_reasons[index]);
    }
    REQUIRE_FALSE(processor.channel().ui_try_pop_applied());

    REQUIRE(processor.persistent_project() == project_before);
    const auto* clip_after = processor.persistent_project()
                                 ->find_sequence({3})->find_track({4})->find_clip({5});
    const auto* content_after =
        std::get_if<timeline::RegisteredContent>(&clip_after->content());
    REQUIRE(content_after);
    REQUIRE(content_after->canonical_payload_json() == payload_before);
    REQUIRE(cells_equal(processor.pattern_snapshot().patterns[0].lanes[0][0],
                        document_before.patterns[0].lanes[0][0]));
    REQUIRE(cells_equal(processor.pattern_snapshot().patterns[0].lanes[0][15],
                        document_before.patterns[0].lanes[0][15]));
    REQUIRE(processor.pattern_snapshot().active_pattern == document_before.active_pattern);
    REQUIRE(processor.pattern_snapshot().engine_sequence == 3);
    REQUIRE(processor.pattern_snapshot().epoch == 2);
    REQUIRE(processor.channel().ui_resync_required_epoch() == 2);
    const auto resync = processor.channel().ui_read_latest_snapshot();
    REQUIRE(resync.epoch == 2);
    REQUIRE(resync.engine_sequence == 3);
    REQUIRE(cells_equal(resync.patterns[0].lanes[0][0],
                        document_before.patterns[0].lanes[0][0]));
    REQUIRE(processor.last_transport().tempo_map == tempo_map_before);

    REQUIRE(processor.seek_samples(0) == playback::TransportError::None);
    StereoBlock render_after(128);
    process_direct(processor, render_after);
    REQUIRE(render_after.left == render_before.left);
    REQUIRE(render_after.right == render_before.right);
}
TEST_CASE("timeline step lane randomize preserves save load expand equivalence") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());

    REQUIRE(processor.channel().ui_try_submit(length_command(11, state::kStepCount)));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.channel().ui_try_pop_applied());

    state::StepEditCommand hidden;
    hidden.client_sequence = 12;
    hidden.kind = state::StepEditKind::SetCell;
    hidden.payload.set_cell.pattern = 0;
    hidden.payload.set_cell.lane = 0;
    hidden.payload.set_cell.step = 31;
    hidden.payload.set_cell.cell.flags = state::StepCell::kEnabledBit;
    hidden.payload.set_cell.cell.velocity = 117;
    hidden.payload.set_cell.cell.pitch_offset = 19;
    hidden.payload.set_cell.cell.gate_ticks = 12;
    REQUIRE(processor.channel().ui_try_submit(hidden));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.channel().ui_try_pop_applied());
    const auto hidden_before = processor.pattern_snapshot().patterns[0].lanes[0][31];

    REQUIRE(processor.channel().ui_try_submit(length_command(13, 16)));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.channel().ui_try_pop_applied());

    state::StepEditCommand randomize;
    randomize.client_sequence = 14;
    randomize.kind = state::StepEditKind::RandomizeLane;
    randomize.payload.randomize_lane.pattern = 0;
    randomize.payload.randomize_lane.lane = 0;
    randomize.payload.randomize_lane.seed = 0xC0FFEEu;
    randomize.payload.randomize_lane.density = 127;
    randomize.payload.randomize_lane.min_velocity = 72;
    randomize.payload.randomize_lane.max_velocity = 110;
    REQUIRE(processor.channel().ui_try_submit(randomize));
    REQUIRE(processor.apply_pending_edits_and_recompile());

    const auto randomize_echo = processor.channel().ui_try_pop_applied();
    REQUIRE(randomize_echo);
    REQUIRE(randomize_echo->kind == state::AppliedEditKind::StepRangeChanged);
    REQUIRE(randomize_echo->dirty.step_count == 16);
    REQUIRE(randomize_echo->payload.step_range.step_count == 16);
    REQUIRE(cells_equal(processor.pattern_snapshot().patterns[0].lanes[0][31],
                        hidden_before));

    auto saved = timeline::serialize_project(*processor.persistent_project(),
                                             processor.pattern_registry());
    REQUIRE(saved);
    auto loaded = timeline::deserialize_project(saved.value().json,
                                                processor.pattern_registry());
    REQUIRE(loaded);
    const auto* loaded_clip = loaded.value()
                                  .find_sequence({3})
                                  ->find_track({4})
                                  ->find_clip({5});
    REQUIRE(loaded_clip);
    const auto* loaded_registered =
        std::get_if<timeline::RegisteredContent>(&loaded_clip->content());
    REQUIRE(loaded_registered);
    auto loaded_pattern = loaded_registered->value_as<StepPatternDocument>()->snapshot;
    REQUIRE(cells_equal(loaded_pattern.patterns[0].lanes[0][31], hidden_before));

    TimelineStepSequencerProcessor restored;
    restored.prepare(prepare_context());
    REQUIRE(restored.load_persistent_project(loaded.value()));
    REQUIRE(restored.pattern_snapshot().epoch == 2);
    REQUIRE(restored.channel().ui_resync_required_epoch() == 2);
    REQUIRE(cells_equal(restored.pattern_snapshot().patterns[0].lanes[0][31],
                        hidden_before));

    auto expand = length_command(15, state::kStepCount);
    REQUIRE(processor.channel().ui_try_submit(expand));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    const auto expand_echo = processor.channel().ui_try_pop_applied();
    REQUIRE(expand_echo);
    REQUIRE(expand_echo->kind == state::AppliedEditKind::PatternLengthChanged);
    REQUIRE(restored.channel().ui_try_submit(expand));
    REQUIRE(restored.apply_pending_edits_and_recompile());
    REQUIRE(restored.channel().ui_try_pop_applied());

    const auto* live_clip = processor.persistent_project()
                                ->find_sequence({3})
                                ->find_track({4})
                                ->find_clip({5});
    REQUIRE(live_clip);
    const auto* live_registered =
        std::get_if<timeline::RegisteredContent>(&live_clip->content());
    REQUIRE(live_registered);
    const auto* restored_clip = restored.persistent_project()
                                    ->find_sequence({3})
                                    ->find_track({4})
                                    ->find_clip({5});
    REQUIRE(restored_clip);
    const auto* restored_registered =
        std::get_if<timeline::RegisteredContent>(&restored_clip->content());
    REQUIRE(restored_registered);
    REQUIRE(restored_registered->canonical_payload_json() ==
            live_registered->canonical_payload_json());

    constexpr std::int64_t kStep31Sample = 31 * 6'000;
    REQUIRE(processor.seek_samples(kStep31Sample) == playback::TransportError::None);
    REQUIRE(restored.seek_samples(kStep31Sample) == playback::TransportError::None);
    StereoBlock uninterrupted_render(128);
    StereoBlock restored_render(128);
    process_direct(processor, uninterrupted_render);
    process_direct(restored, restored_render);
    REQUIRE(uninterrupted_render.energy() > 0.0);
    REQUIRE(restored_render.left == uninterrupted_render.left);
    REQUIRE(restored_render.right == uninterrupted_render.right);
    StereoBlock uninterrupted_next(128);
    StereoBlock restored_next(128);
    process_direct(processor, uninterrupted_next);
    process_direct(restored, restored_next);
    REQUIRE(restored_next.left == uninterrupted_next.left);
    REQUIRE(restored_next.right == uninterrupted_next.right);
    REQUIRE(processor.channel().ui_read_playhead().active_step == 31);
    REQUIRE(restored.channel().ui_read_playhead().active_step == 31);
}

TEST_CASE("timeline step cell and lane clears publish exact active extent echoes") {
    TimelineStepSequencerProcessor cell_processor;
    cell_processor.prepare(prepare_context());
    auto cell_clear = clear_command(21, state::ClearScope::Cell, 0, 0, 0);
    REQUIRE(cell_processor.channel().ui_try_submit(cell_clear));
    REQUIRE(cell_processor.apply_pending_edits_and_recompile());
    const auto cell_echo = cell_processor.channel().ui_try_pop_applied();
    REQUIRE(cell_echo);
    REQUIRE(cell_echo->kind == state::AppliedEditKind::StepRangeChanged);
    REQUIRE(cell_echo->dirty.kind == state::DirtyKind::Cell);
    REQUIRE(cell_echo->payload.step_range.step_count == 1);
    REQUIRE(cell_echo->client_sequence == 21);
    REQUIRE(cell_echo->transaction_id == 121);
    REQUIRE(cell_processor.pattern_snapshot().epoch == 1);
    REQUIRE_FALSE(cell_processor.pattern_snapshot().patterns[0].lanes[0][0].enabled());
    REQUIRE(cell_processor.seek_samples(0) == playback::TransportError::None);
    StereoBlock cleared_start(128);
    process_direct(cell_processor, cleared_start);
    REQUIRE(cleared_start.energy() == 0.0);

    TimelineStepSequencerProcessor lane_processor;
    lane_processor.prepare(prepare_context());
    REQUIRE(lane_processor.channel().ui_try_submit(
        length_command(22, state::kStepCount)));
    REQUIRE(lane_processor.apply_pending_edits_and_recompile());
    REQUIRE(lane_processor.channel().ui_try_pop_applied());
    state::StepEditCommand hidden;
    hidden.client_sequence = 23;
    hidden.kind = state::StepEditKind::SetCell;
    hidden.payload.set_cell.pattern = 0;
    hidden.payload.set_cell.lane = 0;
    hidden.payload.set_cell.step = 31;
    hidden.payload.set_cell.cell.flags = state::StepCell::kEnabledBit;
    hidden.payload.set_cell.cell.velocity = 109;
    hidden.payload.set_cell.cell.gate_ticks = 12;
    REQUIRE(lane_processor.channel().ui_try_submit(hidden));
    REQUIRE(lane_processor.apply_pending_edits_and_recompile());
    REQUIRE(lane_processor.channel().ui_try_pop_applied());
    const auto hidden_tail = lane_processor.pattern_snapshot().patterns[0].lanes[0][31];
    REQUIRE(lane_processor.channel().ui_try_submit(length_command(24, 16)));
    REQUIRE(lane_processor.apply_pending_edits_and_recompile());
    REQUIRE(lane_processor.channel().ui_try_pop_applied());

    auto lane_clear = clear_command(25, state::ClearScope::Lane, 0, 0, 255);
    REQUIRE(lane_processor.channel().ui_try_submit(lane_clear));
    REQUIRE(lane_processor.apply_pending_edits_and_recompile());
    const auto lane_echo = lane_processor.channel().ui_try_pop_applied();
    REQUIRE(lane_echo);
    REQUIRE(lane_echo->kind == state::AppliedEditKind::StepRangeChanged);
    REQUIRE(lane_echo->dirty.kind == state::DirtyKind::Lane);
    REQUIRE(lane_echo->dirty.step_count == 16);
    REQUIRE(lane_echo->payload.step_range.step_count == 16);
    REQUIRE(lane_echo->client_sequence == 25);
    REQUIRE(lane_echo->transaction_id == 125);
    REQUIRE_FALSE(lane_processor.pattern_snapshot().patterns[0].lanes[0][0].enabled());
    REQUIRE(cells_equal(lane_processor.pattern_snapshot().patterns[0].lanes[0][31],
                        hidden_tail));
    REQUIRE(lane_processor.pattern_snapshot().epoch == 1);
}

TEST_CASE("timeline step pattern and all clears publish authoritative bulk resyncs") {
    for (const auto scope : {state::ClearScope::Pattern, state::ClearScope::All}) {
        TimelineStepSequencerProcessor processor;
        processor.prepare(prepare_context());
        REQUIRE(processor.channel().ui_try_submit(
            length_command(31, state::kStepCount)));
        REQUIRE(processor.apply_pending_edits_and_recompile());
        REQUIRE(processor.channel().ui_try_pop_applied());
        state::StepEditCommand hidden;
        hidden.client_sequence = 32;
        hidden.kind = state::StepEditKind::SetCell;
        hidden.payload.set_cell.pattern = 0;
        hidden.payload.set_cell.lane = 1;
        hidden.payload.set_cell.step = 31;
        hidden.payload.set_cell.cell.flags = state::StepCell::kEnabledBit;
        hidden.payload.set_cell.cell.velocity = 115;
        hidden.payload.set_cell.cell.gate_ticks = 12;
        REQUIRE(processor.channel().ui_try_submit(hidden));
        REQUIRE(processor.apply_pending_edits_and_recompile());
        REQUIRE(processor.channel().ui_try_pop_applied());
        const auto hidden_tail = processor.pattern_snapshot().patterns[0].lanes[1][31];
        REQUIRE(processor.channel().ui_try_submit(length_command(33, 16)));
        REQUIRE(processor.apply_pending_edits_and_recompile());
        REQUIRE(processor.channel().ui_try_pop_applied());

        auto clear = clear_command(34, scope, 0, 255, 255);
        REQUIRE(processor.channel().ui_try_submit(clear));
        REQUIRE(processor.apply_pending_edits_and_recompile());
        REQUIRE_FALSE(processor.channel().ui_try_pop_applied());
        REQUIRE(processor.pattern_snapshot().engine_sequence == 4);
        REQUIRE(processor.pattern_snapshot().epoch == 2);
        REQUIRE(processor.channel().ui_resync_required_epoch() == 2);
        const auto snapshot = processor.channel().ui_read_latest_snapshot();
        REQUIRE(snapshot.engine_sequence == 4);
        REQUIRE(snapshot.epoch == 2);
        for (std::uint8_t lane = 0; lane < snapshot.active_lane_count; ++lane) {
            for (std::uint8_t step = 0; step < 16; ++step)
                REQUIRE_FALSE(snapshot.patterns[0].lanes[lane][step].enabled());
        }
        REQUIRE(cells_equal(snapshot.patterns[0].lanes[1][31], hidden_tail));

        auto saved = timeline::serialize_project(*processor.persistent_project(),
                                                 processor.pattern_registry());
        REQUIRE(saved);
        auto loaded = timeline::deserialize_project(saved.value().json,
                                                    processor.pattern_registry());
        REQUIRE(loaded);
        TimelineStepSequencerProcessor restored;
        restored.prepare(prepare_context());
        REQUIRE(restored.load_persistent_project(loaded.value()));
        REQUIRE(cells_equal(restored.pattern_snapshot().patterns[0].lanes[1][31],
                            hidden_tail));
        REQUIRE(restored.seek_samples(0) == playback::TransportError::None);
        StereoBlock cleared(128);
        process_direct(restored, cleared);
        REQUIRE(cleared.energy() == 0.0);
    }
}

TEST_CASE("timeline step clear rejects only fields relevant to each scope") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    const std::array rejected{
        clear_command(41, state::ClearScope::Cell, 0, 0, 31),
        clear_command(42, state::ClearScope::Lane, 0, 4, 255),
        clear_command(43, state::ClearScope::Pattern, 1, 255, 255),
    };
    for (const auto& command : rejected) {
        REQUIRE(processor.channel().ui_try_submit(command));
        REQUIRE_FALSE(processor.apply_pending_edits_and_recompile());
        const auto echo = processor.channel().ui_try_pop_applied();
        REQUIRE(echo);
        REQUIRE(echo->kind == state::AppliedEditKind::CommandRejected);
        REQUIRE(echo->client_sequence == command.client_sequence);
        REQUIRE(echo->transaction_id == command.transaction_id);
        REQUIRE(echo->payload.reject_reason == 2);
    }

    const auto all = clear_command(44, state::ClearScope::All, 255, 255, 255);
    REQUIRE(processor.channel().ui_try_submit(all));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE_FALSE(processor.channel().ui_try_pop_applied());
    REQUIRE(processor.pattern_snapshot().engine_sequence == 4);
    REQUIRE(processor.pattern_snapshot().epoch == 2);
    REQUIRE(processor.channel().ui_resync_required_epoch() == 2);
}

TEST_CASE("timeline step channel snapshots only when applied echoes overflow") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());

    for (std::size_t index = 0; index < state::kAppliedQueueCapacity; ++index) {
        state::StepEditCommand command;
        command.client_sequence = index + 1;
        command.kind = state::StepEditKind::SetCell;
        command.payload.set_cell.pattern = 0;
        command.payload.set_cell.lane = 0;
        command.payload.set_cell.step = 0;
        command.payload.set_cell.cell = processor.pattern_snapshot().patterns[0].lanes[0][0];
        command.payload.set_cell.cell.pitch_offset = static_cast<std::int8_t>(index % 24);
        REQUIRE(processor.channel().ui_try_submit(command));
    }
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.pattern_snapshot().epoch == 1);
    REQUIRE(processor.channel().ui_resync_required_epoch() == 1);

    state::StepEditCommand overflow;
    overflow.client_sequence = state::kAppliedQueueCapacity + 1;
    overflow.kind = state::StepEditKind::SetCell;
    overflow.payload.set_cell.pattern = 0;
    overflow.payload.set_cell.lane = 0;
    overflow.payload.set_cell.step = 0;
    overflow.payload.set_cell.cell = processor.pattern_snapshot().patterns[0].lanes[0][0];
    overflow.payload.set_cell.cell.pitch_offset = 42;
    REQUIRE(processor.channel().ui_try_submit(overflow));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.pattern_snapshot().epoch == 2);
    REQUIRE(processor.channel().ui_resync_required_epoch() == 2);
    const auto recovered = processor.channel().ui_read_latest_snapshot();
    REQUIRE(recovered.epoch == 2);
    REQUIRE(recovered.engine_sequence == state::kAppliedQueueCapacity + 1);
    REQUIRE(recovered.patterns[0].lanes[0][0].pitch_offset == 42);
}
