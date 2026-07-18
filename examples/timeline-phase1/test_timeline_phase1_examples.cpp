#include "timeline_audio_player.hpp"
#include "timeline_step_sequencer.hpp"

#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/format/headless.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::examples::timeline_phase1;

namespace {

struct StereoBlock {
    explicit StereoBlock(std::size_t frames)
        : left(frames), right(frames), silent_left(frames), silent_right(frames),
          output_ptrs{left.data(), right.data()},
          input_ptrs{silent_left.data(), silent_right.data()} {}

    audio::BufferView<float> output() {
        return {output_ptrs.data(), output_ptrs.size(), left.size()};
    }
    audio::BufferView<const float> input() const {
        return {input_ptrs.data(), input_ptrs.size(), silent_left.size()};
    }
    double energy(std::size_t first = 0) const {
        double result = 0.0;
        for (std::size_t frame = first; frame < left.size(); ++frame)
            result += std::abs(left[frame]) + std::abs(right[frame]);
        return result;
    }

    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> silent_left;
    std::vector<float> silent_right;
    std::array<float*, 2> output_ptrs;
    std::array<const float*, 2> input_ptrs;
};

format::PrepareContext prepare_context(int maximum_block = 128) {
    return {.sample_rate = 48'000.0,
            .max_buffer_size = maximum_block,
            .input_channels = 0,
            .output_channels = 2};
}

void process_direct(format::Processor& processor, StereoBlock& block) {
    auto output = block.output();
    auto input = block.input();
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    processor.process(output, input, midi_in, midi_out, {});
}

bool submit_pitch_edit(TimelineStepSequencerProcessor& processor,
                       std::int8_t pitch_offset) {
    state::StepEditCommand command;
    command.client_sequence = 1;
    command.transaction_id = 1;
    command.kind = state::StepEditKind::SetCell;
    command.payload.set_cell.pattern = 0;
    command.payload.set_cell.lane = 0;
    command.payload.set_cell.step = 0;
    command.payload.set_cell.cell = processor.pattern_snapshot().patterns[0].lanes[0][0];
    command.payload.set_cell.cell.pitch_offset = pitch_offset;
    return processor.channel().ui_try_submit(command);
}

bool cells_equal(const state::StepCell& left, const state::StepCell& right) {
    return left.flags == right.flags && left.velocity == right.velocity &&
           left.probability == right.probability &&
           left.pitch_offset == right.pitch_offset &&
           left.gate_ticks == right.gate_ticks && left.ratchet == right.ratchet &&
           left.reserved == right.reserved;
}

state::StepEditCommand length_command(state::ClientSequence sequence,
                                      std::uint8_t length) {
    state::StepEditCommand command;
    command.client_sequence = sequence;
    command.kind = state::StepEditKind::SetPatternLength;
    command.payload.set_pattern_length.pattern = 0;
    command.payload.set_pattern_length.length = length;
    return command;
}

state::StepEditCommand clear_command(state::ClientSequence sequence,
                                     state::ClearScope scope,
                                     std::uint8_t pattern = 0,
                                     std::uint8_t lane = 0,
                                     std::uint8_t step = 0) {
    state::StepEditCommand command;
    command.client_sequence = sequence;
    command.transaction_id = static_cast<state::EditTransactionId>(sequence + 100);
    command.kind = state::StepEditKind::Clear;
    command.payload.clear = {scope, pattern, lane, step};
    return command;
}

std::optional<timeline::Project>
project_with_step_pattern(timeline::RegisteredContent content) {
    constexpr auto duration =
        timebase::TickDuration{static_cast<std::int64_t>(state::kStepCount) *
                               (timebase::kTicksPerQuarter / 4)};
    auto clip = timeline::Clip::create({5}, {0}, duration, std::move(content));
    if (!clip)
        return std::nullopt;
    std::vector<timeline::Clip> clips;
    clips.push_back(std::move(clip).value());
    auto track = timeline::Track::create({4}, "Step pattern", std::move(clips));
    if (!track)
        return std::nullopt;
    std::vector<timeline::Track> tracks;
    tracks.push_back(std::move(track).value());
    auto sequence = timeline::Sequence::create({3}, "Step pattern", duration,
                                               std::move(tracks));
    if (!sequence)
        return std::nullopt;
    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Step pattern codec test";
    input.next_item_id = 6;
    input.root_sequence_id = {3};
    input.sequences.push_back(std::move(sequence).value());
    auto project = timeline::Project::create(std::move(input));
    if (!project)
        return std::nullopt;
    return std::move(project).value();
}

bool replace_once(std::string& text, std::string_view before,
                  std::string_view after) {
    const auto found = text.find(before);
    if (found == std::string::npos)
        return false;
    text.replace(found, before.size(), after);
    return true;
}

} // namespace

namespace pulp::format {
struct StandaloneRenderTestAccess {
    static void ensure_processor(StandaloneApp& app) {
        if (!app.processor_) {
            app.processor_ = app.factory_();
            app.processor_->set_state_store(&app.store_);
            app.processor_->define_parameters(app.store_);
        }
    }
    static void prepare(StandaloneApp& app) { app.prepare_render_state(); }
    static void render(StandaloneApp& app,
                       const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output,
                       const audio::CallbackContext& context) {
        app.render_audio_block(input, output, context);
    }
};
} // namespace pulp::format

static_assert(TimelineExampleEngine::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(TimelineAudioPlayerProcessor::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(TimelineStepSequencerProcessor::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);

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

TEST_CASE("timeline step pattern codec is canonical at maximum bounded extent") {
    auto registry = make_step_pattern_registry();
    REQUIRE(registry);
    state::Snapshot maximum;
    maximum.schema_version = kStepPatternSchemaVersion;
    maximum.active_pattern = state::kPatternCount - 1;
    maximum.active_lane_count = state::kLaneCount;
    maximum.active_pattern_count = state::kPatternCount;
    for (std::uint8_t pattern = 0; pattern < state::kPatternCount; ++pattern) {
        maximum.patterns[pattern].length =
            pattern % 2 == 0 ? state::kStepCount : 0;
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane) {
            for (std::uint8_t step = 0; step < state::kStepCount; ++step) {
                auto& cell = maximum.patterns[pattern].lanes[lane][step];
                cell.flags = static_cast<std::uint8_t>((pattern + lane + step) & 0xffu);
                cell.velocity = static_cast<std::uint8_t>((pattern + lane + step) % 128u);
                cell.probability = static_cast<std::uint8_t>(127u - (step % 128u));
                const auto pitch_ordinal =
                    (static_cast<unsigned>(pattern) * state::kLaneCount + lane + step) %
                    256u;
                cell.pitch_offset = static_cast<std::int8_t>(
                    static_cast<int>(pitch_ordinal) - 128);
                cell.gate_ticks = static_cast<std::uint16_t>(
                    (static_cast<unsigned>(pattern) * 997u + lane * 37u + step) & 0xffffu);
                cell.ratchet = static_cast<std::uint8_t>((lane + step) & 0xffu);
                cell.reserved = 0;
            }
        }
    }
    REQUIRE(step_pattern_snapshot_is_canonical(maximum));
    auto registered = make_registered_step_pattern(maximum, *registry);
    REQUIRE(registered);
    const auto canonical = registered->canonical_payload_json();
    auto project = project_with_step_pattern(std::move(*registered));
    REQUIRE(project);
    auto encoded = timeline::serialize_project(*project, *registry);
    REQUIRE(encoded);
    auto decoded = timeline::deserialize_project(encoded.value().json, *registry);
    REQUIRE(decoded);
    const auto* clip = decoded.value().find_sequence({3})->find_track({4})->find_clip({5});
    REQUIRE(clip);
    const auto* content = std::get_if<timeline::RegisteredContent>(&clip->content());
    REQUIRE(content);
    REQUIRE(content->canonical_payload_json() == canonical);
    const auto* roundtrip = content->value_as<StepPatternDocument>();
    REQUIRE(roundtrip);
    REQUIRE(roundtrip->snapshot.active_pattern == maximum.active_pattern);
    REQUIRE(roundtrip->snapshot.active_lane_count == state::kLaneCount);
    REQUIRE(roundtrip->snapshot.active_pattern_count == state::kPatternCount);
    for (std::uint8_t pattern = 0; pattern < state::kPatternCount; ++pattern) {
        REQUIRE(roundtrip->snapshot.patterns[pattern].length ==
                maximum.patterns[pattern].length);
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane)
            for (std::uint8_t step = 0; step < state::kStepCount; ++step)
                REQUIRE(cells_equal(roundtrip->snapshot.patterns[pattern].lanes[lane][step],
                                    maximum.patterns[pattern].lanes[lane][step]));
    }
}

TEST_CASE("timeline step pattern codec rejects malformed schema counts cells and padding") {
    TimelineStepSequencerProcessor source;
    source.prepare(prepare_context());
    auto encoded = timeline::serialize_project(*source.persistent_project(),
                                               source.pattern_registry());
    REQUIRE(encoded);
    const std::array mutations{
        std::pair{std::string_view{"\"schema_version\":1"},
                  std::string_view{"\"schema_version\":2"}},
        std::pair{std::string_view{"\"active_lane_count\":4"},
                  std::string_view{"\"active_lane_count\":5"}},
        std::pair{std::string_view{"[1,100,127,\"0\",12,1]"},
                  std::string_view{"[1,100,128,\"0\",12,1]"}},
    };
    for (const auto& [before, after] : mutations) {
        auto malformed = encoded.value().json;
        REQUIRE(replace_once(malformed, before, after));
        REQUIRE_FALSE(timeline::deserialize_project(malformed,
                                                    source.pattern_registry()));
    }

    auto invalid_version = source.pattern_snapshot();
    invalid_version.schema_version = kStepPatternSchemaVersion + 1;
    REQUIRE_FALSE(make_registered_step_pattern(invalid_version,
                                               source.pattern_registry()));

    auto inactive_lane = source.pattern_snapshot();
    inactive_lane.patterns[0].lanes[inactive_lane.active_lane_count][0].flags =
        state::StepCell::kEnabledBit;
    REQUIRE_FALSE(step_pattern_snapshot_is_canonical(inactive_lane));
    REQUIRE_FALSE(make_registered_step_pattern(inactive_lane,
                                               source.pattern_registry()));

    auto mutable_document = std::make_shared<StepPatternDocument>();
    mutable_document->snapshot = source.pattern_snapshot();
    std::shared_ptr<const void> erased = mutable_document;
    auto registered = source.pattern_registry().create_registered_no_owned_ids(
        {kStepPatternSchemaName, kStepPatternSchemaVersion}, std::move(erased),
        2u * 1024u * 1024u);
    REQUIRE(registered);
    mutable_document->snapshot.patterns[1].length = 1;
    auto padded_project = project_with_step_pattern(std::move(registered).value());
    REQUIRE(padded_project);
    TimelineStepSequencerProcessor loader;
    loader.prepare(prepare_context());
    REQUIRE_FALSE(loader.load_persistent_project(*padded_project));

    auto schema_document = std::make_shared<StepPatternDocument>();
    schema_document->snapshot = source.pattern_snapshot();
    std::shared_ptr<const void> schema_erased = schema_document;
    auto schema_registered = source.pattern_registry().create_registered_no_owned_ids(
        {kStepPatternSchemaName, kStepPatternSchemaVersion},
        std::move(schema_erased), 2u * 1024u * 1024u);
    REQUIRE(schema_registered);
    schema_document->snapshot.schema_version = kStepPatternSchemaVersion + 1;
    auto wrong_schema_project =
        project_with_step_pattern(std::move(schema_registered).value());
    REQUIRE(wrong_schema_project);
    REQUIRE_FALSE(loader.load_persistent_project(*wrong_schema_project));
}

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

TEST_CASE("timeline examples render deterministically through standalone callback seam") {
    struct Case {
        format::ProcessorFactory factory;
        bool audio_player;
    };
    const std::array cases{
        Case{create_validation_timeline_audio_player, true},
        Case{create_timeline_step_sequencer, false},
    };
    for (const auto& example : cases) {
        format::StandaloneApp app(example.factory);
        format::StandaloneConfig config;
        config.sample_rate = 48'000.0;
        config.buffer_size = 64;
        config.input_channels = 0;
        config.output_channels = 2;
        config.persist_settings = false;
        config.route_test_signal_to_output = false;
        app.set_config(config);
        format::StandaloneRenderTestAccess::ensure_processor(app);
        format::StandaloneRenderTestAccess::prepare(app);
        REQUIRE(app.processor());
        if (example.audio_player) {
            const auto* processor =
                dynamic_cast<const TimelineAudioPlayerProcessor*>(app.processor());
            REQUIRE(processor);
            REQUIRE(processor->engine_prepared());
        } else {
            const auto* processor =
                dynamic_cast<const TimelineStepSequencerProcessor*>(app.processor());
            REQUIRE(processor);
            REQUIRE(processor->engine_prepared());
        }

        StereoBlock block(64);
        audio::CallbackContext context;
        context.sample_rate = 48'000.0;
        context.buffer_size = 64;
        for (int warmup = 0; warmup < 2; ++warmup) {
            auto output = block.output();
            auto input = block.input();
            format::StandaloneRenderTestAccess::render(app, input, output, context);
            REQUIRE(block.energy() > 0.0);
            context.sample_position += 64;
        }
        std::size_t allocations = 1;
        {
            test::ScopedRtProcessProbe probe;
            auto output = block.output();
            auto input = block.input();
            format::StandaloneRenderTestAccess::render(app, input, output, context);
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        REQUIRE(block.energy() > 0.0);
        REQUIRE(block.left == block.right);
    }
}
