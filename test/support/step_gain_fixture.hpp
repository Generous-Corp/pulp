#pragma once

// A gain processor that applies automation exactly at each point's sample
// offset, and the output that schedule must produce. The VST3, AU v2 and CLAP
// tests each drive it with the same steps through their own adapter and
// require that exact output, so the three formats are proven to agree to the
// bit; a schedule moved one sample later must not.

#include <pulp/format/processor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::test {

inline constexpr state::ParamID kStepGainParam = 1;
inline constexpr int kStepGainFrames = 64;

// (sample offset, gain) points within one block; the gain is 1 before the first.
inline constexpr std::array<std::pair<int, float>, 4> kStepGainPoints{
    {{0, 0.5f}, {13, 0.25f}, {37, 1.0f}, {50, 0.75f}}};

class StepGainProcessor final : public format::Processor {
  public:
    static inline int process_calls = 0;
    bool wants_sample_accurate_parameters() const override {
        return true;
    }
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "StepGainTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.step-gain",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kStepGainParam, .name = "Gain", .range = {0.0f, 1.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {
        gain_ = 1.0f;
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        ++process_calls;
        const auto* queue = param_events();
        std::size_t next = 0;
        for (std::size_t n = 0; n < out.num_samples(); ++n) {
            while (queue && next < queue->size() &&
                   queue->events()[next].sample_offset <= static_cast<int32_t>(n)) {
                if (queue->events()[next].param_id == kStepGainParam)
                    gain_ = queue->events()[next].value;
                ++next;
            }
            for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch)
                out.channel(ch)[n] = in.channel(ch)[n] * gain_;
        }
    }

  private:
    float gain_ = 1.0f;
};

inline std::unique_ptr<format::Processor> make_step_gain_processor() {
    return std::make_unique<StepGainProcessor>();
}

// Deterministic input, different per channel.
inline float step_gain_input(std::size_t channel, std::size_t frame) {
    std::uint32_t x = static_cast<std::uint32_t>(channel * 7919u + frame * 104729u + 1u);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<float>(x >> 8) / 16777216.0f - 0.5f;
}

// What the processor must output for `points` shifted by `shift` samples.
inline float step_gain_expected(std::size_t channel, std::size_t frame, int shift = 0) {
    float gain = 1.0f;
    for (const auto& [offset, value] : kStepGainPoints)
        if (offset + shift <= static_cast<int>(frame))
            gain = value;
    return step_gain_input(channel, frame) * gain;
}

} // namespace pulp::test
