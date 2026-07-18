#include "timeline_step_sequencer.hpp"

#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

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

std::shared_ptr<const timeline::Project> make_pattern_project(const state::Snapshot& pattern) {
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
            notes.push_back({{next_note_id++},
                             {static_cast<std::int64_t>(step) * kStepTicks},
                             {std::max<std::int64_t>(1, kStepTicks / 2)},
                             static_cast<std::uint16_t>(
                                 static_cast<std::uint16_t>(cell.velocity) * 0x0202u),
                             static_cast<std::uint8_t>(60 + lane * 4 + cell.pitch_offset),
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

} // namespace

TimelineStepSequencerProcessor::TimelineStepSequencerProcessor() {
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
    auto project = make_pattern_project(pattern_);
    if (!project)
        return;
    const auto integer_rate = static_cast<std::uint64_t>(std::llround(context.sample_rate));
    if (integer_rate == 0)
        return;
    const std::array points{timebase::TempoPoint{{0}, 120.0}};
    auto map = std::make_shared<const timebase::CompiledTempoMap>(
        points, timebase::RationalRate{integer_rate, 1});
    auto assets = playback::DecodedAudioAssetPool::create({});
    if (!assets)
        return;
    playback::ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {3};
    request.tempo_map = std::move(map);
    request.document_revision = 1;
    request.audio_assets = std::move(assets).value();
    request.audio_limits.max_channels = 2;
    request.audio_limits.max_block_frames =
        static_cast<std::uint32_t>(context.max_buffer_size);
    if (!engine_.prepare(std::move(request), context.sample_rate,
                         static_cast<std::uint32_t>(context.max_buffer_size), true))
        return;

    pattern_.epoch = ++epoch_;
    channel_.audio_publish_snapshot(pattern_);
    channel_.audio_mark_resync_required(pattern_.epoch);
    const auto bar_end = engine_.last_transport().tempo_map->ticks_to_samples({kBarTicks}).value;
    engine_.set_loop_samples(true, 0, bar_end);
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
    playhead.active_pattern = pattern_.active_pattern;
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
