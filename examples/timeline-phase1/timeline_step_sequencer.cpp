#include "timeline_step_sequencer.hpp"

#include <pulp/state/step_edit_reducer.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::examples::timeline_phase1 {
namespace {

template <class T, class E>
std::optional<T> value_or_none(runtime::Result<T, E> result) {
    if (!result)
        return std::nullopt;
    return std::move(result).value();
}

constexpr std::int64_t kStepTicks = timebase::kTicksPerQuarter / 4;
constexpr std::int64_t kBarTicks = timebase::kTicksPerQuarter * 4;

bool command_targets_active_extent(const state::Snapshot& snapshot,
                                   const state::StepEditCommand& command) noexcept {
    const auto pattern_is_active = [&snapshot](std::uint8_t pattern) {
        return pattern < snapshot.active_pattern_count;
    };
    const auto lane_is_active = [&snapshot](std::uint8_t lane) {
        return lane < snapshot.active_lane_count;
    };
    const auto step_is_active = [&snapshot, &pattern_is_active](std::uint8_t pattern,
                                                                std::uint8_t step) {
        if (!pattern_is_active(pattern))
            return false;
        const auto length = snapshot.patterns[pattern].length;
        const auto active_steps = length == 0 ? state::kStepCount : length;
        return step < active_steps;
    };
    switch (command.kind) {
    case state::StepEditKind::SetCell:
        return pattern_is_active(command.payload.set_cell.pattern) &&
               lane_is_active(command.payload.set_cell.lane) &&
               step_is_active(command.payload.set_cell.pattern,
                              command.payload.set_cell.step);
    case state::StepEditKind::Clear:
        return pattern_is_active(command.payload.clear.pattern) &&
               lane_is_active(command.payload.clear.lane) &&
               step_is_active(command.payload.clear.pattern,
                              command.payload.clear.step);
    case state::StepEditKind::RandomizeLane:
        return pattern_is_active(command.payload.randomize_lane.pattern) &&
               lane_is_active(command.payload.randomize_lane.lane);
    case state::StepEditKind::SetPatternLength:
        return pattern_is_active(command.payload.set_pattern_length.pattern);
    case state::StepEditKind::SwitchPattern:
        return pattern_is_active(command.payload.switch_pattern.pattern);
    }
    return false;
}

state::AppliedEdit rejected_edit(const state::StepEditCommand& command,
                                 state::EngineSequence& engine_sequence,
                                 state::Epoch snapshot_epoch,
                                 std::uint32_t reason) noexcept {
    state::AppliedEdit echo;
    echo.engine_sequence = ++engine_sequence;
    echo.snapshot_epoch = snapshot_epoch;
    echo.client_sequence = command.client_sequence;
    echo.transaction_id = command.transaction_id;
    echo.kind = state::AppliedEditKind::CommandRejected;
    echo.payload.reject_reason = reason;
    return echo;
}

std::optional<state::AppliedEdit>
apply_active_extent_step_edit(state::Snapshot& snapshot,
                              const state::StepEditCommand& command,
                              state::EngineSequence& engine_sequence) {
    if (command.kind != state::StepEditKind::RandomizeLane)
        return state::apply_step_edit<state::ReferenceSequencerConfig>(
            snapshot, command, engine_sequence);

    const auto pattern_index = command.payload.randomize_lane.pattern;
    const auto lane_index = command.payload.randomize_lane.lane;
    const auto length = snapshot.patterns[pattern_index].length;
    const auto active_steps = length == 0 ? state::kStepCount : length;

    // The frozen reference reducer intentionally operates over compile-time
    // capacity. This example persists only active extent, so preserve the
    // inactive tail: otherwise randomize -> save/load -> expand reveals state
    // that was never serialized and the two authoritative histories diverge.
    std::array<state::StepCell, state::kStepCount> inactive_tail{};
    for (std::uint8_t step = active_steps; step < state::kStepCount; ++step)
        inactive_tail[step] = snapshot.patterns[pattern_index].lanes[lane_index][step];

    auto echo = state::apply_step_edit<state::ReferenceSequencerConfig>(
        snapshot, command, engine_sequence);
    if (!echo || echo->kind != state::AppliedEditKind::StepRangeChanged)
        return echo;

    for (std::uint8_t step = active_steps; step < state::kStepCount; ++step)
        snapshot.patterns[pattern_index].lanes[lane_index][step] = inactive_tail[step];
    echo->payload.step_range.step_count = active_steps;
    echo->dirty.step_count = active_steps;
    return echo;
}

std::shared_ptr<const timeline::Project>
make_playback_pattern_project(const state::Snapshot& pattern) {
    std::vector<timeline::NoteEvent> notes;
    notes.reserve(64);
    std::uint64_t next_note_id = 10;
    const auto& active = pattern.patterns[pattern.active_pattern];
    const auto step_count = active.length == 0 ? state::kStepCount : active.length;
    for (std::uint8_t step = 0; step < step_count; ++step) {
        for (std::uint8_t lane = 0; lane < pattern.active_lane_count; ++lane) {
            const auto& cell = active.lanes[lane][step];
            if (!cell.enabled())
                continue;
            const auto gate = std::max<std::int64_t>(
                1, (kStepTicks * static_cast<std::int64_t>(cell.gate_ticks)) / 24);
            const auto pitch = std::clamp<int>(
                60 + static_cast<int>(lane) * 4 + static_cast<int>(cell.pitch_offset),
                0, 127);
            notes.push_back({{next_note_id++},
                             {static_cast<std::int64_t>(step) * kStepTicks},
                             {gate},
                             static_cast<std::uint16_t>(
                                 static_cast<std::uint16_t>(cell.velocity) * 0x0202u),
                             static_cast<std::uint8_t>(pitch),
                             0});
        }
    }
    auto content = value_or_none(timeline::NoteContent::create(std::move(notes)));
    if (!content)
        return {};
    auto clip = value_or_none(timeline::Clip::create(
        {5}, {0}, {kBarTicks}, std::move(*content)));
    if (!clip)
        return {};
    auto track = value_or_none(timeline::Track::create(
        {4}, "Step pattern", {std::move(*clip)}));
    if (!track)
        return {};
    auto sequence = value_or_none(timeline::Sequence::create(
        {3}, "One bar pattern", timebase::TickDuration{kBarTicks},
        std::vector<timeline::Track>{std::move(*track)}));
    if (!sequence)
        return {};
    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Timeline step sequencer";
    input.next_item_id = next_note_id;
    input.root_sequence_id = {3};
    input.sequences = {std::move(*sequence)};
    auto project = value_or_none(timeline::Project::create(std::move(input)));
    return project ? std::make_shared<const timeline::Project>(std::move(*project)) : nullptr;
}

std::shared_ptr<const timeline::Project>
make_persistent_pattern_project(const state::Snapshot& pattern,
                                const timeline::SchemaRegistry& registry) {
    auto registered = make_registered_step_pattern(pattern, registry);
    if (!registered)
        return {};
    auto clip = value_or_none(timeline::Clip::create(
        {5}, {0}, {kBarTicks}, std::move(*registered)));
    if (!clip)
        return {};
    auto track = value_or_none(timeline::Track::create(
        {4}, "Step pattern", {std::move(*clip)}));
    if (!track)
        return {};
    auto sequence = value_or_none(timeline::Sequence::create(
        {3}, "One bar pattern", timebase::TickDuration{kBarTicks},
        std::vector<timeline::Track>{std::move(*track)}));
    if (!sequence)
        return {};
    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Timeline step sequencer";
    input.next_item_id = 6;
    input.root_sequence_id = {3};
    input.sequences = {std::move(*sequence)};
    auto project = value_or_none(timeline::Project::create(std::move(input)));
    return project ? std::make_shared<const timeline::Project>(std::move(*project)) : nullptr;
}

} // namespace

TimelineStepSequencerProcessor::TimelineStepSequencerProcessor() {
    auto registry = make_step_pattern_registry();
    if (registry) {
        registry_ = std::move(*registry);
        registry_ready_ = true;
    }
    pattern_.schema_version = 1;
    pattern_.active_pattern = 0;
    pattern_.active_lane_count = 4;
    pattern_.active_pattern_count = 1;
    auto& bar = pattern_.patterns[0];
    bar.length = 16;
    for (std::uint8_t step : {0u, 4u, 8u, 12u}) {
        auto& cell = bar.lanes[(step / 4) % 4][step];
        cell.flags = state::StepCell::kEnabledBit;
        cell.velocity = static_cast<std::uint8_t>(100 + step);
        cell.gate_ticks = 12;
    }
}

format::PluginDescriptor TimelineStepSequencerProcessor::descriptor() const {
    return {.name = "Timeline Step Sequencer",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.timeline-step-sequencer",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0};
}

void TimelineStepSequencerProcessor::prepare(const format::PrepareContext& context) {
    if (context.sample_rate <= 0.0 || context.max_buffer_size <= 0)
        return;
    sample_rate_ = context.sample_rate;
    maximum_block_size_ = static_cast<std::uint32_t>(context.max_buffer_size);
    if (!compile_pattern(pattern_, true))
        return;

    pattern_.engine_sequence = engine_sequence_;
    pattern_.epoch = ++epoch_;
    channel_.audio_publish_snapshot(pattern_);
    channel_.audio_mark_resync_required(pattern_.epoch);
}

bool TimelineStepSequencerProcessor::compile_pattern(const state::Snapshot& snapshot,
                                                     bool replace_engine) {
    if (!registry_ready_ || sample_rate_ <= 0.0 || maximum_block_size_ == 0)
        return false;
    auto persistent = make_persistent_pattern_project(snapshot, registry_);
    auto project = make_playback_pattern_project(snapshot);
    if (!persistent || !project)
        return false;
    const auto integer_rate = static_cast<std::uint64_t>(std::llround(sample_rate_));
    if (integer_rate == 0)
        return false;
    const std::array points{timebase::TempoPoint{{0}, 120.0}};
    auto map = std::make_shared<const timebase::CompiledTempoMap>(
        points, timebase::RationalRate{integer_rate, 1});
    auto assets = playback::DecodedAudioAssetPool::create({});
    if (!assets)
        return false;
    playback::ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {3};
    request.tempo_map = std::move(map);
    request.document_revision = snapshot.engine_sequence + 1;
    request.audio_assets = std::move(assets).value();
    request.audio_limits.max_channels = 2;
    request.audio_limits.max_block_frames = maximum_block_size_;
    if (!replace_engine && engine_.prepared()) {
        if (!engine_.recompile(std::move(request)))
            return false;
    } else {
        if (!engine_.prepare(std::move(request), sample_rate_, maximum_block_size_, true))
            return false;
        const auto bar_end =
            engine_.last_transport().tempo_map->ticks_to_samples({kBarTicks}).value;
        if (engine_.set_loop_samples(true, 0, bar_end) != playback::TransportError::None)
            return false;
    }
    persistent_project_ = std::move(persistent);
    active_pattern_.store(snapshot.active_pattern, std::memory_order_release);
    return true;
}

bool TimelineStepSequencerProcessor::apply_pending_edits_and_recompile() {
    auto candidate = pattern_;
    auto candidate_sequence = engine_sequence_;
    std::vector<state::AppliedEdit> applied;
    applied.reserve(state::kCommandQueueCapacity);
    while (auto command = channel_.audio_try_pop_command()) {
        if (!command_targets_active_extent(candidate, *command)) {
            applied.push_back(rejected_edit(*command, candidate_sequence,
                                            candidate.epoch, 2));
            continue;
        }
        auto echo = apply_active_extent_step_edit(candidate, *command,
                                                  candidate_sequence);
        if (echo)
            applied.push_back(std::move(*echo));
    }
    if (applied.empty())
        return false;

    const auto changed = std::any_of(applied.begin(), applied.end(), [](const auto& edit) {
        return edit.kind != state::AppliedEditKind::CommandRejected;
    });
    if (!changed) {
        engine_sequence_ = candidate_sequence;
        bool echo_lost = false;
        for (const auto& echo : applied)
            echo_lost |= !channel_.audio_try_publish_applied(echo);
        if (echo_lost) {
            pattern_.engine_sequence = engine_sequence_;
            pattern_.epoch = ++epoch_;
            channel_.audio_publish_snapshot(pattern_);
            channel_.audio_mark_resync_required(pattern_.epoch);
        }
        return false;
    }

    candidate.engine_sequence = candidate_sequence;
    if (!compile_pattern(candidate, false)) {
        engine_sequence_ = candidate_sequence;
        for (auto& echo : applied) {
            echo.kind = state::AppliedEditKind::CommandRejected;
            echo.payload.reject_reason = 3;
            (void)channel_.audio_try_publish_applied(echo);
        }
        pattern_.engine_sequence = engine_sequence_;
        pattern_.epoch = ++epoch_;
        channel_.audio_publish_snapshot(pattern_);
        channel_.audio_mark_resync_required(pattern_.epoch);
        return false;
    }
    pattern_ = std::move(candidate);
    engine_sequence_ = candidate_sequence;
    bool echo_lost = false;
    for (const auto& echo : applied)
        echo_lost |= !channel_.audio_try_publish_applied(echo);
    if (echo_lost) {
        pattern_.epoch = ++epoch_;
        channel_.audio_publish_snapshot(pattern_);
        channel_.audio_mark_resync_required(pattern_.epoch);
    }
    return true;
}

void TimelineStepSequencerProcessor::process(audio::BufferView<float>& output,
                                             const audio::BufferView<const float>& input,
                                             midi::MidiBuffer&, midi::MidiBuffer&,
                                             const format::ProcessContext&) {
    engine_.process(output, input);
    const auto& transport = engine_.last_transport();
    state::PlayheadState playhead;
    playhead.block_index = static_cast<std::uint32_t>(transport.block_index);
    playhead.playing = transport.is_playing ? 1 : 0;
    playhead.active_pattern = active_pattern_.load(std::memory_order_acquire);
    if (transport.range_count != 0) {
        const auto tick = transport.ranges[0].timeline_tick_start.value;
        const auto wrapped = ((tick % kBarTicks) + kBarTicks) % kBarTicks;
        playhead.active_step = static_cast<std::uint8_t>(wrapped / kStepTicks);
        playhead.sample_time = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, transport.ranges[0].timeline_sample_start.value));
        playhead.ppq_position = static_cast<double>(tick) /
                                static_cast<double>(timebase::kTicksPerQuarter);
    }
    channel_.audio_publish_playhead(playhead);
}

playback::TransportError TimelineStepSequencerProcessor::set_playing(bool playing) noexcept {
    return engine_.set_playing(playing);
}
playback::TransportError TimelineStepSequencerProcessor::seek_samples(std::int64_t sample) noexcept {
    return engine_.seek_samples(sample);
}
playback::TransportError TimelineStepSequencerProcessor::set_loop_samples(
    bool enabled, std::int64_t start, std::int64_t end) noexcept {
    return engine_.set_loop_samples(enabled, start, end);
}
const playback::TransportSnapshot& TimelineStepSequencerProcessor::last_transport() const noexcept {
    return engine_.last_transport();
}

std::unique_ptr<format::Processor> create_timeline_step_sequencer() {
    return std::make_unique<TimelineStepSequencerProcessor>();
}

} // namespace pulp::examples::timeline_phase1
