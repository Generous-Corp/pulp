#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_playback_cursor.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/realtime_sample_recorder.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/sample_slot_materializer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace {

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::LoopPlaybackCursor;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopReader;
using pulp::audio::LoopRegion;
using pulp::audio::publish_completed_recording_to_slot;
using pulp::audio::PublishedSampleView;
using pulp::audio::RealtimeSampleRecorder;
using pulp::audio::RealtimeSampleRecorderCommand;
using pulp::audio::RealtimeSampleRecorderCommandType;
using pulp::audio::RealtimeSampleRecorderConfig;
using pulp::audio::RealtimeSampleRecorderEvent;
using pulp::audio::RealtimeSampleRecorderEventType;
using pulp::audio::RealtimeSampleRecorderState;
using pulp::audio::SampleSlotBank;

constexpr double kSampleRate = 48'000.0;
constexpr std::size_t kFrames = 64;

float expected_sample(std::size_t frame) {
    const double phase = static_cast<double>(frame) / 17.0;
    return static_cast<float>(0.2 + 0.35 * std::sin(2.0 * std::numbers::pi * phase));
}

class CaptureNode final : public pulp::format::Processor {
  public:
    explicit CaptureNode(RealtimeSampleRecorder& recorder) : recorder_(recorder) {}

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "G11 capture node",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.g11.recording-publication.capture",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"Input", 1}},
                .output_buses = {{"Output", 1}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(BufferView<float>& output, const BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        recorder_.process(input, static_cast<std::uint32_t>(input.num_samples()));
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            output.channel(0)[frame] = input.channel(0)[frame];
    }

  private:
    RealtimeSampleRecorder& recorder_;
};

class PublishedPlayerNode final : public pulp::format::Processor {
  public:
    explicit PublishedPlayerNode(const SampleSlotBank& bank) : bank_(bank) {}

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "G11 published sample player",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.g11.recording-publication.player",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Instrument,
                .input_buses = {},
                .output_buses = {{"Output", 1}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext&) override {
        view_ = bank_.read_published_view();
        channel_ = bank_.channel_data(view_, 0);
        region_ = {.start_frame = 0,
                   .end_frame = view_.num_frames,
                   .source_sample_rate = view_.sample_rate,
                   .playback_mode = LoopPlaybackMode::OneShot,
                   .interpolation = pulp::audio::LoopInterpolationMode::None};
        ready_ = view_.valid && channel_ != nullptr && region_.end_frame > region_.start_frame &&
                 cursor_.set_region(region_, view_.num_frames);
        cursor_.start();
    }

    void process(BufferView<float>& output, const BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            output.channel(0)[frame] =
                ready_ && cursor_.active()
                    ? LoopReader::read(BufferView<const float>(&channel_, 1, view_.num_frames),
                                       region_, 0, cursor_.position())
                    : 0.0f;
            if (ready_ && cursor_.active())
                cursor_.advance();
        }
    }

  private:
    const SampleSlotBank& bank_;
    PublishedSampleView view_;
    const float* channel_ = nullptr;
    LoopRegion region_;
    LoopPlaybackCursor cursor_;
    bool ready_ = false;
};

bool graph_prepare(pulp::host::SignalGraph& graph, int max_block) {
    graph.set_limits({.max_nodes = 4,
                      .max_connections = 3,
                      .max_ports = 8,
                      .max_block_size = max_block,
                      .max_estimated_work_units = 4096});
    return graph.prepare(kSampleRate, max_block);
}

bool capture_schedule(RealtimeSampleRecorder& recorder, std::span<const int> blocks,
                      std::vector<float>& passthrough) {
    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto capture =
        graph.add_processor_node(std::make_unique<CaptureNode>(recorder), "capture");
    const auto output = graph.add_output_node(1, "output");
    if (!input || !capture || !output || !graph.connect(input, 0, capture, 0) ||
        !graph.connect(capture, 0, output, 0) || !graph_prepare(graph, 32))
        return false;

    passthrough.clear();
    passthrough.reserve(kFrames);
    std::size_t cursor = 0;
    for (const int block : blocks) {
        if (block <= 0 || cursor + static_cast<std::size_t>(block) > kFrames)
            return false;
        Buffer<float> input_buffer(1, static_cast<std::size_t>(block));
        Buffer<float> output_buffer(1, static_cast<std::size_t>(block));
        for (int frame = 0; frame < block; ++frame)
            input_buffer.channel(0)[static_cast<std::size_t>(frame)] =
                expected_sample(cursor + frame);
        const auto& input_const = input_buffer;
        auto input_view = input_const.view();
        auto output_view = output_buffer.view();
        graph.process(output_view, input_view, block);
        for (const float sample : output_buffer.channel(0))
            passthrough.push_back(sample);
        cursor += static_cast<std::size_t>(block);
    }
    return cursor == kFrames && recorder.state() == RealtimeSampleRecorderState::Completed;
}

bool capture_to_bank(std::span<const int> schedule, SampleSlotBank& bank,
                     std::vector<float>& passthrough) {
    RealtimeSampleRecorder recorder;
    if (!recorder.prepare({.num_channels = 1, .max_frames = kFrames, .sample_rate = kSampleRate}))
        return false;
    RealtimeSampleRecorderCommand start;
    start.sequence_id = 1;
    start.type = RealtimeSampleRecorderCommandType::Start;
    start.max_frames = kFrames;
    if (!recorder.enqueue_command(start) || !capture_schedule(recorder, schedule, passthrough))
        return false;
    const float* scratch[1] = {};
    if (!publish_completed_recording_to_slot(recorder, bank, std::span(scratch), 0))
        return false;
    return bank.read_published_view().valid;
}

std::vector<float> playback(const SampleSlotBank& bank, std::span<const int> schedule) {
    pulp::host::SignalGraph graph;
    const auto player =
        graph.add_processor_node(std::make_unique<PublishedPlayerNode>(bank), "player");
    const auto output = graph.add_output_node(1, "output");
    if (!player || !output || !graph.connect(player, 0, output, 0) || !graph_prepare(graph, 32))
        return {};
    std::vector<float> rendered;
    rendered.reserve(kFrames);
    std::size_t cursor = 0;
    const float* no_input[1] = {nullptr};
    for (const int block : schedule) {
        Buffer<float> output_buffer(1, static_cast<std::size_t>(block));
        BufferView<const float> empty_input(no_input, 0, static_cast<std::size_t>(block));
        auto output_view = output_buffer.view();
        graph.process(output_view, empty_input, block);
        rendered.insert(rendered.end(), output_buffer.channel(0).begin(),
                        output_buffer.channel(0).end());
        cursor += static_cast<std::size_t>(block);
    }
    return cursor == kFrames ? rendered : std::vector<float>{};
}

// The oracle: every sample must equal expected_sample(frame) * scale. Planted
// controls never change this function; they corrupt the data it is given.
bool matches(std::span<const float> samples, float scale = 1.0f) {
    if (samples.size() != kFrames)
        return false;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const double expected = static_cast<double>(expected_sample(frame) * scale);
        if (!std::isfinite(samples[frame]) ||
            std::abs(static_cast<double>(samples[frame]) - expected) > 1.0e-5)
            return false;
    }
    return true;
}

// Reads a published view the way a consumer would. Returns an empty vector
// when the bank refuses the view (stale generation or null channel data).
std::vector<float> read_view(const SampleSlotBank& bank, const PublishedSampleView& view) {
    if (!view.valid || view.num_frames != kFrames || !bank.slot_view_valid(view))
        return {};
    const float* channel = bank.channel_data(view, 0);
    if (channel == nullptr)
        return {};
    return std::vector<float>(channel, channel + kFrames);
}

// Planted controls exit 0 only after the oracle has rejected the planted
// defect, and print this marker, which the CTest registration requires.
int detected(std::string_view control) {
    std::cout << "G11 planted defect detected: " << control << '\n';
    return 0;
}

// Exit code for a planted control the oracle failed to reject.
constexpr int kUndetected = 42;

int run(std::string_view mode) {
    const bool wrong_buffer = mode == "wrong";
    const bool timing = mode == "timing";
    const bool stale = mode == "stale";
    const bool planted = wrong_buffer || timing || stale;
    const std::array<int, 4> regular{16, 16, 16, 16};
    const std::array<int, 7> irregular{7, 13, 5, 19, 3, 11, 6};

    RealtimeSampleRecorder refusal;
    if (refusal.prepare({.num_channels = 0, .max_frames = 0, .sample_rate = kSampleRate}))
        return 2;
    RealtimeSampleRecorderCommand rejected;
    rejected.type = RealtimeSampleRecorderCommandType::Stop;
    if (!refusal.prepare({.num_channels = 1, .max_frames = kFrames, .sample_rate = kSampleRate}) ||
        !refusal.enqueue_command(rejected))
        return 3;
    Buffer<float> empty(1, 0);
    std::vector<const float*> empty_ptrs(1);
    refusal.process(BufferView<const float>(empty_ptrs.data(), 1, 0), 0);
    RealtimeSampleRecorderEvent refusal_event;
    if (refusal.state() != RealtimeSampleRecorderState::Idle || !refusal.pop_event(refusal_event) ||
        refusal_event.type != RealtimeSampleRecorderEventType::CommandRejected)
        return 4;
    refusal.reset();

    SampleSlotBank regular_bank;
    SampleSlotBank irregular_bank;
    if (!regular_bank.prepare(2, 1, kFrames) || !irregular_bank.prepare(2, 1, kFrames))
        return 5;
    std::vector<float> regular_capture;
    std::vector<float> irregular_capture;
    if (!capture_to_bank(regular, regular_bank, regular_capture) ||
        !capture_to_bank(irregular, irregular_bank, irregular_capture))
        return 6;
    if (regular_capture != irregular_capture)
        return 7;

    auto rendered = playback(irregular_bank, irregular);
    if (!matches(rendered))
        return 8;
    if (!matches(playback(regular_bank, regular)))
        return 9;

    if (timing) {
        // Planted defect: the rendered stream is shifted by one frame.
        std::rotate(rendered.begin(), rendered.begin() + 1, rendered.end());
        return matches(rendered) ? kUndetected : detected("timing");
    }

    const auto old_view = regular_bank.read_published_view();
    if (!regular_bank.slot_view_valid(old_view))
        return 10;

    // Republish a different (half-scale) buffer into the regular bank.
    constexpr float kReplacementScale = 0.5f;
    Buffer<float> replacement(1, kFrames);
    for (std::size_t frame = 0; frame < kFrames; ++frame)
        replacement.channel(0)[frame] = expected_sample(frame) * kReplacementScale;
    if (!regular_bank.publish_from_buffer(replacement.view(), kFrames, kSampleRate,
                                          old_view.generation))
        return 11;
    const auto new_view = regular_bank.read_published_view();
    if (!new_view.valid || new_view.generation == old_view.generation)
        return 12;

    if (wrong_buffer) {
        // Planted defect: the consumer reads the replacement buffer where it
        // expects the recorded take.
        return matches(read_view(regular_bank, new_view)) ? kUndetected
                                                          : detected("wrong-buffer");
    }
    if (stale) {
        // Planted defect: the consumer keeps reading through the retired view
        // after the replacement was published.
        return matches(read_view(regular_bank, old_view), kReplacementScale)
                   ? kUndetected
                   : detected("stale-publication");
    }
    if (planted)
        return 13;

    // Positive path: the new generation carries the replacement and the
    // retired view is refused.
    if (!matches(read_view(regular_bank, new_view), kReplacementScale))
        return 14;
    if (regular_bank.slot_view_valid(old_view) || regular_bank.channel_data(old_view, 0) != nullptr)
        return 15;

    std::cout << "G11 capture/publication graph passed: 64 frames, regular/irregular blocks, "
                 "immutable generation "
              << old_view.generation << " retired by generation " << new_view.generation
              << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2)
        return 64;
    const std::string_view mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    if (!mode.empty() && mode != "--planted-wrong-buffer" && mode != "--planted-timing" &&
        mode != "--planted-stale-publication")
        return 64;
    return run(mode == "--planted-wrong-buffer"        ? "wrong"
               : mode == "--planted-timing"            ? "timing"
               : mode == "--planted-stale-publication" ? "stale"
                                                       : "positive");
}
