#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/midi/tuning.hpp>

#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr std::size_t kFrames = 96;
constexpr double kTunedA4 = 442.0;

// The tuning lookup is owned by authoring/prepare, never by the audio callback.
class ToneProcessor final : public pulp::format::Processor {
  public:
    explicit ToneProcessor(double frequency_hz) : frequency_hz_(frequency_hz) {}

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "Graph tuning cookbook tone",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.graph-tuning-cookbook",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Instrument,
                .input_buses = {},
                .output_buses = {{"Tone", 1}},
                .accepts_midi = false,
                .produces_midi = false};
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext& context) override {
        sample_rate_ = context.sample_rate;
        phase_ = 0.0;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            output.channel(0)[frame] =
                static_cast<float>(std::sin(2.0 * std::numbers::pi * phase_));
            phase_ += frequency_hz_ / sample_rate_;
            if (phase_ >= 1.0)
                phase_ -= 1.0;
        }
    }

  private:
    double frequency_hz_ = 0.0;
    double sample_rate_ = kSampleRate;
    double phase_ = 0.0;
};

std::vector<float> render(const pulp::midi::TuningProvider& tuning, std::size_t block_frames,
                          bool force_failure = false) {
    if (force_failure)
        return {};
    const auto note = tuning.note_to_frequency(69);
    if (!note.valid || note.should_filter_note || !std::isfinite(note.frequency_hz) ||
        note.frequency_hz <= 0.0 || note.frequency_hz >= kSampleRate / 2.0)
        return {};

    pulp::host::SignalGraph graph;
    const auto tone =
        graph.add_processor_node(std::make_unique<ToneProcessor>(note.frequency_hz), "Tuned A4");
    const auto output = graph.add_output_node(1, "Mono output");
    if (!tone || !output || !graph.connect(tone, 0, output, 0))
        return {};

    pulp::host::OfflineSignalGraphHost host(graph);
    if (!host.prepare({.sample_rate = kSampleRate,
                       .block_frames = static_cast<int>(block_frames),
                       .input_channels = 1,
                       .output_channels = 1}))
        return {};

    std::vector<float> silence(kFrames, 0.0f);
    float* input_ptrs[] = {silence.data()};
    pulp::audio::BufferView<const float> input(input_ptrs, 1, kFrames);
    const auto result = host.render({.frame_count = kFrames, .input = input});
    if (!result.ok || result.audio.num_channels() != 1 || result.audio.num_samples() != kFrames)
        return {};
    return {result.audio.channel(0).begin(), result.audio.channel(0).end()};
}

bool matches_oracle(const std::vector<float>& audio, double frequency_hz) {
    if (audio.size() != kFrames)
        return false;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const auto expected = std::sin(2.0 * std::numbers::pi * frequency_hz *
                                       static_cast<double>(frame) / kSampleRate);
        if (!std::isfinite(audio[frame]) ||
            std::abs(static_cast<double>(audio[frame]) - expected) > 2.0e-5)
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const bool failed_render = argc == 2 && std::string_view(argv[1]) == "--test-failed-render";
    const bool planted =
        argc == 2 && (std::string_view(argv[1]) == "--planted-default-a4" || failed_render);
    if (argc > 1 && !planted)
        return 2;

    pulp::midi::EqualTemperamentTuningProvider tuning(planted ? 440.0 : kTunedA4);
    if (tuning.note_to_frequency(-1).valid ||
        std::abs(tuning.note_to_frequency(69).frequency_hz - (planted ? 440.0 : kTunedA4)) > 1.0e-9)
        return 3;

    const auto blocks32 = render(tuning, 32, failed_render);
    const auto blocks17 = render(tuning, 17, failed_render);
    if (blocks32.size() != kFrames || blocks17.size() != kFrames)
        return 7;
    if (planted) {
        if (!matches_oracle(blocks32, 440.0) || !matches_oracle(blocks17, 440.0) ||
            matches_oracle(blocks32, kTunedA4) || matches_oracle(blocks17, kTunedA4))
            return 4;
        std::cerr << "planted-default-a4 rejected by tuned audio oracle\n";
        return 42;
    }
    if (!matches_oracle(blocks32, kTunedA4) || !matches_oracle(blocks17, kTunedA4) ||
        blocks32.size() != blocks17.size())
        return 5;
    for (std::size_t frame = 0; frame < blocks32.size(); ++frame)
        if (std::abs(blocks32[frame] - blocks17[frame]) > 1.0e-6f)
            return 6;
    std::cout << "tuned graph A4=442 Hz, 96 frames, block 32/17 parity\n";
    return 0;
}
