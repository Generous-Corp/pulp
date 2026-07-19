#pragma once

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

[[maybe_unused]] format::PrepareContext prepare_context(int maximum_block = 128) {
    return {.sample_rate = 48'000.0,
            .max_buffer_size = maximum_block,
            .input_channels = 0,
            .output_channels = 2};
}

[[maybe_unused]] void process_direct(format::Processor& processor, StereoBlock& block) {
    auto output = block.output();
    auto input = block.input();
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    processor.process(output, input, midi_in, midi_out, {});
}

[[maybe_unused]] bool submit_pitch_edit(TimelineStepSequencerProcessor& processor,
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

[[maybe_unused]] bool cells_equal(const state::StepCell& left, const state::StepCell& right) {
    return left.flags == right.flags && left.velocity == right.velocity &&
           left.probability == right.probability &&
           left.pitch_offset == right.pitch_offset &&
           left.gate_ticks == right.gate_ticks && left.ratchet == right.ratchet &&
           left.reserved == right.reserved;
}

[[maybe_unused]] state::StepEditCommand length_command(state::ClientSequence sequence,
                                                       std::uint8_t length) {
    state::StepEditCommand command;
    command.client_sequence = sequence;
    command.kind = state::StepEditKind::SetPatternLength;
    command.payload.set_pattern_length.pattern = 0;
    command.payload.set_pattern_length.length = length;
    return command;
}

[[maybe_unused]] state::StepEditCommand clear_command(state::ClientSequence sequence,
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

[[maybe_unused]] std::optional<timeline::Project>
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

[[maybe_unused]] bool replace_once(std::string& text, std::string_view before,
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
