#pragma once

// An instrument at product scale, for proving the format adapters there.
//
// It declares 2,000 parameters (1,984 with dense IDs and 16 with sparse IDs
// above the store's dense index, so both lookup paths run), in 20 groups of
// 100, with mixed ranges and units. Its buses are one stereo audio input and
// six stereo outputs: "Main Out" and five optional aux outputs. Output bus k
// carries a sine at bus_frequency(k), so a host test can tell every bus apart
// and see a swap. The input's peak is reported through a read-only parameter,
// written only when it changes, so a block with silent input writes nothing
// to the store.
//
// Command 1 (the Command parameter) makes process() allocate once per block.
// It exists only so a test can prove its allocation probe sees allocations
// made inside the hosted plug-in.

#include <pulp/format/processor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace pulp::test_fixtures {

class ScaleProbe final : public format::Processor {
  public:
    static constexpr int kParamCount = 2000;
    static constexpr int kDenseParams = 1984;
    static constexpr int kGroups = 20;
    static constexpr int kOutputBuses = 6;
    static constexpr float kToneLevel = 0.25f;

    // Index 0 and 1 are the two parameters the probe itself reads or writes.
    static constexpr int kCommandIndex = 0;
    static constexpr int kInputPeakIndex = 1;

    /// The host-visible ID of parameter `index` (0 to kParamCount - 1).
    static constexpr state::ParamID param_id(int index) noexcept {
        return index < kDenseParams
                   ? static_cast<state::ParamID>(index + 1)
                   : static_cast<state::ParamID>(
                         0x00010000u + static_cast<std::uint32_t>(index - kDenseParams) * 4099u);
    }

    static std::string param_name(int index) {
        if (index == kCommandIndex)
            return "Command";
        if (index == kInputPeakIndex)
            return "Input Peak";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Param %04d", index);
        return buf;
    }

    /// The tone output bus `bus` carries, in Hz.
    static constexpr double bus_frequency(int bus) noexcept {
        return 220.0 * static_cast<double>(bus + 1);
    }

    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Pulp Scale Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.scale-probe";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Instrument;
        d.input_buses = {{"Main In", 2, false, format::BusRole::Main}};
        d.output_buses = {{"Main Out", 2, false, format::BusRole::Main}};
        for (int k = 1; k < kOutputBuses; ++k)
            d.output_buses.push_back({"Aux " + std::to_string(k), 2, true, format::BusRole::Aux});
        d.accepts_midi = true;
        return d;
    }

    void define_parameters(state::StateStore& store) override {
        for (int g = 0; g < kGroups; ++g)
            store.add_group({.id = g + 1, .name = "Group " + std::to_string(g + 1)});
        for (int i = 0; i < kParamCount; ++i) {
            state::ParamInfo info;
            info.id = param_id(i);
            info.name = param_name(i);
            info.group_id = 1 + i / (kParamCount / kGroups);
            if (i == kCommandIndex) {
                info.range = {0.0f, 1.0f, 0.0f, 1.0f};
            } else if (i == kInputPeakIndex) {
                info.range = {0.0f, 1.0f, 0.0f};
            } else {
                switch (i % 4) {
                case 0:
                    info.range = {0.0f, 1.0f, 0.5f};
                    break;
                case 1:
                    info.unit = "dB";
                    info.range = {-60.0f, 12.0f, 0.0f};
                    break;
                case 2:
                    info.unit = "Hz";
                    info.range = {20.0f, 20000.0f, 1000.0f, 0.0f, 0.3f};
                    break;
                default:
                    info.range = {0.0f, 127.0f, 64.0f, 1.0f};
                    break;
                }
            }
            store.add_parameter(info);
        }
    }

    void prepare(const format::PrepareContext& context) override {
        sample_rate_ = context.sample_rate > 0.0 ? context.sample_rate : 48000.0;
        phases_.fill(0.0);
        scratch_.clear();
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        report_input(&input);
        write_tone(output, 0);
        maybe_allocate();
    }

    void process(format::ProcessBuffers& audio, midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        report_input(audio.main_input());
        for (std::size_t k = 0; k < audio.outputs.size() && k < kOutputBuses; ++k) {
            auto& bus = audio.outputs[k];
            if (bus.active())
                write_tone(bus.buffer, static_cast<int>(k));
        }
        maybe_allocate();
    }

  private:
    void write_tone(audio::BufferView<float>& out, int bus) {
        const double step = 2.0 * kPi * bus_frequency(bus) / sample_rate_;
        double phase = phases_[static_cast<std::size_t>(bus)];
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            const float v = kToneLevel * static_cast<float>(std::sin(phase));
            for (std::size_t c = 0; c < out.num_channels(); ++c)
                out.channel(c)[i] = v;
            phase += step;
        }
        phases_[static_cast<std::size_t>(bus)] = std::fmod(phase, 2.0 * kPi);
    }

    void report_input(const audio::BufferView<const float>* input) {
        float peak = 0.0f;
        if (input)
            for (std::size_t c = 0; c < input->num_channels(); ++c)
                for (std::size_t i = 0; i < input->num_samples(); ++i)
                    peak = std::max(peak, std::abs(input->channel(c)[i]));
        peak = std::min(peak, 1.0f);
        // Only a change writes: a silent block must leave the store untouched.
        if (peak != reported_peak_) {
            reported_peak_ = peak;
            state().set_value(param_id(kInputPeakIndex), peak);
        }
    }

    void maybe_allocate() {
        if (state().get_value(param_id(kCommandIndex)) >= 0.5f)
            scratch_.emplace_back(std::make_unique<int>(1));
    }

    static constexpr double kPi = 3.14159265358979323846;
    double sample_rate_ = 48000.0;
    std::array<double, kOutputBuses> phases_{};
    float reported_peak_ = 0.0f;
    std::vector<std::unique_ptr<int>> scratch_;
};

inline std::unique_ptr<format::Processor> create_scale_probe() {
    return std::make_unique<ScaleProbe>();
}

} // namespace pulp::test_fixtures
