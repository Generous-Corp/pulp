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
constexpr std::size_t kTwoTakeFrames = 96;
constexpr double kTakeAFrequency = 440.0;
constexpr double kTakeBFrequency = 660.0;

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

float tone_sample(double frequency, std::size_t frame) {
    return static_cast<float>(0.35 * std::sin(2.0 * std::numbers::pi * frequency *
                                              static_cast<double>(frame) / kSampleRate));
}

class TwoTakeNode final : public pulp::format::Processor {
  public:
    explicit TwoTakeNode(SampleSlotBank& bank) : bank_(bank) {}

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "G11 two-take capture/player",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.g11.recording-publication.two-take",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Instrument,
                .input_buses = {{"Input", 1}},
                .output_buses = {{"Output", 1}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void capture(RealtimeSampleRecorder* recorder) noexcept {
        capture_ = recorder;
    }

    bool note_on_a() noexcept {
        return start_voice(voice_a_);
    }
    bool note_on_b() noexcept {
        return start_voice(voice_b_);
    }

    bool note_off_a() noexcept {
        if (!voice_a_.active)
            return false;
        voice_a_.cursor.stop();
        voice_a_.active = false;
        voice_a_.channel = nullptr;
        voice_a_.view = {};
        const auto current = bank_.read_published_view();
        return current.valid && bank_.acknowledge_audio_generation(current.generation);
    }
    bool note_off_b() noexcept {
        return release_voice(voice_b_);
    }

    void process(BufferView<float>& output, const BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        if (capture_ != nullptr)
            capture_->process(input, static_cast<std::uint32_t>(input.num_samples()));
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            output.channel(0)[frame] = read_voice(voice_a_) + read_voice(voice_b_);
        }
    }

  private:
    struct Voice {
        PublishedSampleView view{};
        const float* channel = nullptr;
        LoopRegion region{};
        LoopPlaybackCursor cursor;
        bool active = false;
    };

    bool start_voice(Voice& voice) noexcept {
        const auto view = bank_.read_published_view();
        if (!view.valid || !bank_.slot_view_valid(view) || view.num_frames != kTwoTakeFrames)
            return false;
        const auto* channel = bank_.channel_data(view, 0);
        if (channel == nullptr)
            return false;
        voice.view = view;
        voice.channel = channel;
        voice.region = {.start_frame = 0,
                        .end_frame = view.num_frames,
                        .source_sample_rate = view.sample_rate,
                        .playback_mode = LoopPlaybackMode::Forward,
                        .interpolation = pulp::audio::LoopInterpolationMode::None};
        voice.active = voice.cursor.set_region(voice.region, view.num_frames);
        voice.cursor.start();
        return voice.active;
    }

    bool release_voice(Voice& voice) noexcept {
        if (!voice.active)
            return false;
        voice.cursor.stop();
        const auto generation = voice.view.generation;
        voice.active = false;
        voice.channel = nullptr;
        voice.view = {};
        (void)generation;
        return true;
    }

    float read_voice(Voice& voice) noexcept {
        if (!voice.active || !voice.cursor.active() || voice.channel == nullptr)
            return 0.0f;
        const float value =
            LoopReader::read(BufferView<const float>(&voice.channel, 1, voice.view.num_frames),
                             voice.region, 0, voice.cursor.position());
        voice.cursor.advance();
        return value;
    }

    SampleSlotBank& bank_;
    RealtimeSampleRecorder* capture_ = nullptr;
    Voice voice_a_;
    Voice voice_b_;
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

bool matches_tone(std::span<const float> samples, double frequency, std::size_t first_frame = 0) {
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        const auto expected = tone_sample(frequency, first_frame + frame);
        if (!std::isfinite(samples[frame]) ||
            std::abs(static_cast<double>(samples[frame]) - expected) > 1.0e-5)
            return false;
    }
    return true;
}

bool matches_sum(std::span<const float> samples, double frequency_a, double frequency_b,
                 std::size_t first_frame = 0) {
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        const auto expected = tone_sample(frequency_a, first_frame + frame) +
                              tone_sample(frequency_b, first_frame + frame);
        if (!std::isfinite(samples[frame]) ||
            std::abs(static_cast<double>(samples[frame]) - expected) > 1.0e-5)
            return false;
    }
    return true;
}

std::vector<float> run_graph_schedule(pulp::host::SignalGraph& graph, std::span<const int> blocks,
                                      double frequency, std::size_t frame_count) {
    std::vector<float> rendered;
    rendered.reserve(frame_count);
    std::size_t cursor = 0;
    const float* no_input[1] = {nullptr};
    for (const int block : blocks) {
        if (block <= 0 || cursor + static_cast<std::size_t>(block) > frame_count)
            return {};
        Buffer<float> input_buffer(1, static_cast<std::size_t>(block));
        Buffer<float> output_buffer(1, static_cast<std::size_t>(block));
        for (int frame = 0; frame < block; ++frame)
            input_buffer.channel(0)[static_cast<std::size_t>(frame)] =
                tone_sample(frequency, cursor + static_cast<std::size_t>(frame));
        const auto& input_const = input_buffer;
        auto input_view = input_const.view();
        auto output_view = output_buffer.view();
        graph.process(output_view, input_view, block);
        rendered.insert(rendered.end(), output_buffer.channel(0).begin(),
                        output_buffer.channel(0).end());
        cursor += static_cast<std::size_t>(block);
    }
    return cursor == frame_count ? rendered : std::vector<float>{};
}

int run_two_take() {
    const std::array<int, 4> regular{24, 24, 24, 24};
    const std::array<int, 7> irregular{11, 7, 19, 5, 23, 13, 18};

    // Refusal control: an idle recorder cannot accept Stop.
    RealtimeSampleRecorder refusal;
    if (!refusal.prepare(
            {.num_channels = 1, .max_frames = kTwoTakeFrames, .sample_rate = kSampleRate}))
        return 31;
    RealtimeSampleRecorderCommand stop;
    stop.sequence_id = 99;
    stop.type = RealtimeSampleRecorderCommandType::Stop;
    if (!refusal.enqueue_command(stop))
        return 32;
    Buffer<float> empty(1, 0);
    std::vector<const float*> empty_ptrs(1);
    refusal.process(BufferView<const float>(empty_ptrs.data(), 1, 0), 0);
    RealtimeSampleRecorderEvent refusal_event;
    if (refusal.state() != RealtimeSampleRecorderState::Idle || !refusal.pop_event(refusal_event) ||
        refusal_event.type != RealtimeSampleRecorderEventType::CommandRejected)
        return 33;

    SampleSlotBank bank;
    RealtimeSampleRecorder take_a;
    RealtimeSampleRecorder take_b;
    if (!bank.prepare(2, 1, kTwoTakeFrames) ||
        !take_a.prepare(
            {.num_channels = 1, .max_frames = kTwoTakeFrames, .sample_rate = kSampleRate}) ||
        !take_b.prepare(
            {.num_channels = 1, .max_frames = kTwoTakeFrames, .sample_rate = kSampleRate}))
        return 34;

    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    auto node = std::make_unique<TwoTakeNode>(bank);
    auto* node_ptr = node.get();
    const auto player = graph.add_processor_node(std::move(node), "two-take");
    const auto output = graph.add_output_node(1, "output");
    if (!input || !player || !output || !graph.connect(input, 0, player, 0) ||
        !graph.connect(player, 0, output, 0) || !graph_prepare(graph, 32))
        return 35;

    RealtimeSampleRecorderCommand start_a;
    start_a.sequence_id = 1;
    start_a.type = RealtimeSampleRecorderCommandType::Start;
    start_a.max_frames = kTwoTakeFrames;
    if (!take_a.enqueue_command(start_a))
        return 36;
    node_ptr->capture(&take_a);
    const auto silence = run_graph_schedule(graph, regular, kTakeAFrequency, kTwoTakeFrames);
    node_ptr->capture(nullptr);
    if (silence.size() != kTwoTakeFrames ||
        take_a.state() != RealtimeSampleRecorderState::Completed)
        return 37;
    const float* scratch[1] = {};
    if (!publish_completed_recording_to_slot(take_a, bank, std::span(scratch), 0))
        return 38;
    const auto view_a = bank.read_published_view();
    const auto* data_a = bank.channel_data(view_a, 0);
    if (!view_a.valid || data_a == nullptr ||
        !matches_tone(std::span(data_a, kTwoTakeFrames), kTakeAFrequency))
        return 39;
    if (!node_ptr->note_on_a())
        return 40;

    RealtimeSampleRecorderCommand start_b;
    start_b.sequence_id = 2;
    start_b.type = RealtimeSampleRecorderCommandType::Start;
    start_b.max_frames = kTwoTakeFrames;
    if (!take_b.enqueue_command(start_b))
        return 41;
    node_ptr->capture(&take_b);
    const auto a_while_recording_b =
        run_graph_schedule(graph, irregular, kTakeBFrequency, kTwoTakeFrames);
    node_ptr->capture(nullptr);
    if (!matches_tone(a_while_recording_b, kTakeAFrequency) ||
        take_b.state() != RealtimeSampleRecorderState::Completed)
        return 42;

    // Publish B while A's view is still held by the prepared graph.
    if (!publish_completed_recording_to_slot(take_b, bank, std::span(scratch), 0))
        return 43;
    const auto view_b = bank.read_published_view();
    if (!view_b.valid || view_b.generation == view_a.generation || !bank.slot_view_valid(view_a))
        return 44;
    // Both slots are occupied (A retired but still held, B published), so a
    // third publication is refused until the audio generation is acknowledged.
    Buffer<float> refused(1, kTwoTakeFrames);
    if (bank.reserve_slot(1, kTwoTakeFrames, kSampleRate) >= 0)
        return 45;
    if (!node_ptr->note_on_b())
        return 46;
    const std::array<int, 2> overlap_blocks{7, 9};
    const auto overlap = run_graph_schedule(graph, overlap_blocks, kTakeBFrequency, 16);
    if (!matches_sum(overlap, kTakeAFrequency, kTakeBFrequency))
        return 47;

    // Release A only after B is sounding. Its retired generation must then be
    // refused by generation-aware readers.
    const bool released_a = node_ptr->note_off_a();
    if (!released_a || bank.slot_view_valid(view_a) || bank.channel_data(view_a, 0) != nullptr) {
        return 48;
    }
    const auto b_only = run_graph_schedule(graph, overlap_blocks, kTakeBFrequency, 16);
    if (!matches_tone(b_only, kTakeBFrequency, 16))
        return 49;
    if (!node_ptr->note_off_b())
        return 50;

    std::cout << "G11 two-take graph passed: source=440Hz/660Hz, regular+irregular, "
                 "A-held-through-B-publication, generations "
              << view_a.generation << " -> " << view_b.generation << '\n';
    return 0;
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
    if (mode == "two-take")
        return run_two_take();
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

    // Until the audio side acknowledges the new generation, the retired view
    // stays readable and still carries the original, immutable take.
    if (!matches(read_view(regular_bank, old_view)))
        return 14;
    if (!regular_bank.acknowledge_audio_generation(new_view.generation))
        return 15;

    if (wrong_buffer) {
        // Planted defect: the consumer reads the replacement buffer where it
        // expects the recorded take.
        return matches(read_view(regular_bank, new_view)) ? kUndetected : detected("wrong-buffer");
    }
    if (stale) {
        // Planted defect: the consumer keeps reading through the retired view
        // after the replacement was published and acknowledged.
        return matches(read_view(regular_bank, old_view), kReplacementScale)
                   ? kUndetected
                   : detected("stale-publication");
    }
    if (planted)
        return 13;

    // Positive path: the new generation carries the replacement and, once
    // acknowledged, the retired view is refused.
    if (!matches(read_view(regular_bank, new_view), kReplacementScale))
        return 16;
    if (regular_bank.slot_view_valid(old_view) || regular_bank.channel_data(old_view, 0) != nullptr)
        return 17;

    std::cout << "G11 capture/publication graph passed: 64 frames, regular/irregular blocks, "
                 "immutable generation "
              << old_view.generation << " retired by generation " << new_view.generation << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2)
        return 64;
    const std::string_view mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    if (!mode.empty() && mode != "--two-take" && mode != "--planted-wrong-buffer" &&
        mode != "--planted-timing" && mode != "--planted-stale-publication")
        return 64;
    return run(mode == "--two-take"                    ? "two-take"
               : mode == "--planted-wrong-buffer"      ? "wrong"
               : mode == "--planted-timing"            ? "timing"
               : mode == "--planted-stale-publication" ? "stale"
                                                       : "positive");
}
