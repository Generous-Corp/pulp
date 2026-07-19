#include "timeline_phase1_example_test_support.hpp"

TEST_CASE("timeline audio player rejects WAV beyond bounded decode limits") {
    const auto wav = make_timeline_audio_validation_wav(64);
    audio::WavDecodeLimits limits;
    limits.max_frames = 63;
    limits.max_channels = 2;
    limits.max_output_bytes = 1024;
    TimelineAudioPlayerProcessor processor(wav, limits);
    REQUIRE_FALSE(processor.source_valid());
}

TEST_CASE("timeline audio player runs through headless host with varying blocks") {
    format::HeadlessHost host(create_validation_timeline_audio_player);
    host.prepare(48'000.0, 128, 0, 2);
    REQUIRE(host.valid());
    auto* processor = host.processor_as<TimelineAudioPlayerProcessor>();
    REQUIRE(processor);
    REQUIRE(processor->source_valid());
    REQUIRE(processor->engine_prepared());

    double energy = 0.0;
    for (const std::size_t frames : {1u, 17u, 64u, 128u}) {
        StereoBlock block(frames);
        auto output = block.output();
        auto input = block.input();
        host.process(output, input);
        energy += block.energy();
        REQUIRE(processor->last_transport().frame_count == frames);
    }
    REQUIRE(energy > 0.0);
}

TEST_CASE("timeline audio player projects a loop into two exact graph ranges") {
    const auto wav = make_timeline_audio_validation_wav(512);
    TimelineAudioPlayerProcessor processor(wav);
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    REQUIRE(processor.set_loop_samples(true, 0, 256) == playback::TransportError::None);
    REQUIRE(processor.seek_samples(240) == playback::TransportError::None);
    StereoBlock block(32);
    process_direct(processor, block);
    REQUIRE(processor.last_transport().range_count == 2);
    REQUIRE(processor.last_transport().ranges[0].frame_count == 16);
    REQUIRE(processor.last_transport().ranges[1].sample_offset == 16);
    REQUIRE(block.energy() > 0.0);
}

TEST_CASE("timeline audio player graph process is allocation free after prepare") {
    const auto wav = make_timeline_audio_validation_wav(512);
    TimelineAudioPlayerProcessor processor(wav);
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    StereoBlock block(64);
    process_direct(processor, block);
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        process_direct(processor, block);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("timeline step sequencer preserves frozen grid state and renders audibly") {
    format::HeadlessHost host(create_timeline_step_sequencer);
    host.prepare(48'000.0, 128, 0, 2);
    REQUIRE(host.valid());
    auto* processor = host.processor_as<TimelineStepSequencerProcessor>();
    REQUIRE(processor);
    REQUIRE(processor->engine_prepared());

    const auto& published = processor->channel().ui_read_latest_snapshot();
    REQUIRE(published.epoch == 1);
    REQUIRE(published.patterns[0].length == 16);
    REQUIRE(published.patterns[0].lanes[0][0].enabled());

    double energy = 0.0;
    for (const std::size_t frames : {1u, 31u, 64u, 128u}) {
        StereoBlock block(frames);
        auto output = block.output();
        auto input = block.input();
        host.process(output, input);
        energy += block.energy();
        REQUIRE(processor->last_transport().frame_count == frames);
    }
    REQUIRE(energy > 0.0);
    const auto playhead = processor->channel().ui_read_playhead();
    REQUIRE(playhead.playing == 1);
}

TEST_CASE("timeline step sequencer loop flushes voices and never leaves stuck notes") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    constexpr std::int64_t kBarSamples = 96'000;
    REQUIRE(processor.seek_samples(kBarSamples - 32) == playback::TransportError::None);
    StereoBlock wrap(64);
    process_direct(processor, wrap);
    REQUIRE(processor.last_transport().range_count == 2);
    REQUIRE(wrap.energy(32) > 0.0);
    REQUIRE(processor.has_active_notes());

    REQUIRE(processor.set_playing(false) == playback::TransportError::None);
    StereoBlock stopped(64);
    process_direct(processor, stopped);
    REQUIRE_FALSE(processor.has_active_notes());
    REQUIRE(stopped.energy() == 0.0);
}

TEST_CASE("timeline step sequencer graph process is allocation free after prepare") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    StereoBlock block(64);
    process_direct(processor, block);
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        process_direct(processor, block);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("timeline step sequencer fully reprepares for a new device rate") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    auto changed = prepare_context(64);
    changed.sample_rate = 44'100.0;
    processor.prepare(changed);
    REQUIRE(processor.engine_prepared());
    REQUIRE(processor.last_transport().sample_rate == (timebase::RationalRate{44'100, 1}));
    StereoBlock block(64);
    process_direct(processor, block);
    REQUIRE(block.energy() > 0.0);
}

TEST_CASE("timeline step channel edits persist recompile and deterministically change render") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    REQUIRE(processor.persistent_project());
    const auto* clip = processor.persistent_project()
                           ->find_sequence({3})
                           ->find_track({4})
                           ->find_clip({5});
    REQUIRE(clip);
    const auto* registered = std::get_if<timeline::RegisteredContent>(&clip->content());
    REQUIRE(registered);
    REQUIRE(registered->schema().type_name == kStepPatternSchemaName);
    const auto before_payload = registered->canonical_payload_json();

    StereoBlock baseline(128);
    process_direct(processor, baseline);
    const auto baseline_left = baseline.left;
    REQUIRE(submit_pitch_edit(processor, 12));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.pattern_snapshot().engine_sequence == 1);
    REQUIRE(processor.pattern_snapshot().patterns[0].lanes[0][0].pitch_offset == 12);
    const auto applied = processor.channel().ui_try_pop_applied();
    REQUIRE(applied);
    REQUIRE(applied->kind == state::AppliedEditKind::StepRangeChanged);
    REQUIRE(applied->client_sequence == 1);
    REQUIRE(applied->snapshot_epoch == 1);
    REQUIRE(processor.pattern_snapshot().epoch == 1);
    REQUIRE(processor.channel().ui_resync_required_epoch() == 1);
    const auto published = processor.channel().ui_read_latest_snapshot();
    REQUIRE(published.engine_sequence == 0);
    REQUIRE(published.patterns[0].lanes[0][0].pitch_offset == 0);

    clip = processor.persistent_project()
               ->find_sequence({3})
               ->find_track({4})
               ->find_clip({5});
    registered = std::get_if<timeline::RegisteredContent>(&clip->content());
    REQUIRE(registered);
    REQUIRE(registered->canonical_payload_json() != before_payload);
    auto serialized = timeline::serialize_project(*processor.persistent_project(),
                                                  processor.pattern_registry());
    REQUIRE(serialized);
    auto restored = timeline::deserialize_project(serialized.value().json,
                                                  processor.pattern_registry());
    REQUIRE(restored);
    const auto& restored_content = restored.value()
                                       .find_sequence({3})
                                       ->find_track({4})
                                       ->find_clip({5})
                                       ->content();
    const auto& restored_registered = std::get<timeline::RegisteredContent>(restored_content);
    REQUIRE(restored_registered.value_as<StepPatternDocument>()
                ->snapshot.patterns[0].lanes[0][0].pitch_offset == 12);

    REQUIRE(processor.seek_samples(0) == playback::TransportError::None);
    StereoBlock changed(128);
    process_direct(processor, changed);
    REQUIRE(changed.energy() > 0.0);
    REQUIRE(changed.left != baseline_left);

    TimelineStepSequencerProcessor replay;
    replay.prepare(prepare_context());
    REQUIRE(submit_pitch_edit(replay, 12));
    REQUIRE(replay.apply_pending_edits_and_recompile());
    REQUIRE(replay.seek_samples(0) == playback::TransportError::None);
    StereoBlock replayed(128);
    process_direct(replay, replayed);
    REQUIRE(replayed.left == changed.left);
    REQUIRE(replayed.right == changed.right);
}

TEST_CASE("timeline step channel rejects edits outside the persisted active extent") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    state::StepEditCommand command;
    command.client_sequence = 9;
    command.kind = state::StepEditKind::SwitchPattern;
    command.payload.switch_pattern.pattern = 1;
    REQUIRE(processor.channel().ui_try_submit(command));
    REQUIRE_FALSE(processor.apply_pending_edits_and_recompile());
    const auto rejected = processor.channel().ui_try_pop_applied();
    REQUIRE(rejected);
    REQUIRE(rejected->kind == state::AppliedEditKind::CommandRejected);
    REQUIRE(rejected->client_sequence == 9);
    REQUIRE(rejected->payload.reject_reason == 2);
    REQUIRE(processor.pattern_snapshot().active_pattern == 0);
    REQUIRE(processor.engine_prepared());
}

TEST_CASE("timeline step channel rejects cells beyond active pattern length") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    REQUIRE(processor.persistent_project());
    const auto* clip = processor.persistent_project()
                           ->find_sequence({3})
                           ->find_track({4})
                           ->find_clip({5});
    REQUIRE(clip);
    const auto* registered = std::get_if<timeline::RegisteredContent>(&clip->content());
    REQUIRE(registered);
    const auto before_payload = registered->canonical_payload_json();
    const auto before_cell = processor.pattern_snapshot().patterns[0].lanes[0][31];

    state::StepEditCommand command;
    command.client_sequence = 10;
    command.kind = state::StepEditKind::SetCell;
    command.payload.set_cell.pattern = 0;
    command.payload.set_cell.lane = 0;
    command.payload.set_cell.step = 31;
    command.payload.set_cell.cell = before_cell;
    command.payload.set_cell.cell.flags = state::StepCell::kEnabledBit;
    command.payload.set_cell.cell.pitch_offset = 24;
    REQUIRE(processor.channel().ui_try_submit(command));
    REQUIRE_FALSE(processor.apply_pending_edits_and_recompile());
    const auto rejected = processor.channel().ui_try_pop_applied();
    REQUIRE(rejected);
    REQUIRE(rejected->kind == state::AppliedEditKind::CommandRejected);
    REQUIRE(rejected->client_sequence == 10);
    REQUIRE(rejected->payload.reject_reason == 2);
    const auto& after_cell = processor.pattern_snapshot().patterns[0].lanes[0][31];
    REQUIRE(after_cell.flags == before_cell.flags);
    REQUIRE(after_cell.pitch_offset == before_cell.pitch_offset);

    clip = processor.persistent_project()
               ->find_sequence({3})
               ->find_track({4})
               ->find_clip({5});
    registered = std::get_if<timeline::RegisteredContent>(&clip->content());
    REQUIRE(registered);
    REQUIRE(registered->canonical_payload_json() == before_payload);

    TimelineStepSequencerProcessor reference;
    reference.prepare(prepare_context());
    REQUIRE(reference.engine_prepared());
    REQUIRE(reference.seek_samples(0) == playback::TransportError::None);
    StereoBlock before(128);
    process_direct(reference, before);

    REQUIRE(processor.seek_samples(0) == playback::TransportError::None);
    StereoBlock after(128);
    process_direct(processor, after);
    REQUIRE(after.left == before.left);
    REQUIRE(after.right == before.right);
}
