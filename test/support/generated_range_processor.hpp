#pragma once

/// A processor whose parameter range, default and value texts are computed in
/// code rather than listed, so a format adapter test can check that the host
/// sees the generated values.
///
/// The "Pitch" parameter starts from a base range of 0..127 and widens it to
/// 0..2400 (cents) by a formula; its default is the midpoint and its text is
/// generated from the value: 1200 reads "12.0 st". `widen = false` builds the
/// same parameter with the unwidened 0..127 range and the same generators, the
/// control that shows a test can tell a snapshot of the base range from the
/// generated one.

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace pulp::test {

inline constexpr state::ParamID kGeneratedPitchId = 1;
inline constexpr float kGeneratedPitchMax = 2400.0f;
inline constexpr float kGeneratedPitchMid = 1200.0f;
inline constexpr float kBasePitchMax = 127.0f;

class GeneratedRangeProcessor final : public format::Processor {
  public:
    static inline bool widen = true;

    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "GeneratedRange";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.generated.range";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        return d;
    }

    void define_parameters(state::StateStore& store) override {
        // Generated, not listed: 24 steps of 100 cents.
        float top = kBasePitchMax;
        if (widen) {
            top = 0.0f;
            for (int step = 0; step < 24; ++step)
                top += 100.0f;
        }
        state::ParamInfo pitch{.id = kGeneratedPitchId,
                               .name = "Pitch",
                               .unit = "st",
                               .range = {0.0f, top, top * 0.5f, 0.0f}};
        pitch.to_string = [](float cents) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f st", cents / 100.0f);
            return std::string(buf);
        };
        pitch.from_string = [](const std::string& text) {
            return static_cast<float>(std::atof(text.c_str()) * 100.0);
        };
        store.add_parameter(pitch);
    }

    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch) {
            auto ic = in.channel(ch);
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i)
                oc[i] = ic[i];
        }
    }
};

inline std::unique_ptr<format::Processor> make_generated_range_processor() {
    return std::make_unique<GeneratedRangeProcessor>();
}

} // namespace pulp::test
