#pragma once

// Two distinct editor-less plug-ins for the Linux isolation test. They differ
// only in identity, which is what a host sees through each format's factory.

#include <pulp/format/processor.hpp>

#include <cmath>
#include <memory>

namespace pulp::test_fixtures {

template <char Letter> class IsolationProcessor final : public format::Processor {
  public:
    static constexpr state::ParamID kGain = 1;

    format::PluginDescriptor descriptor() const override {
        static const std::string name = std::string("Pulp Isolation ") + Letter;
        static const std::string id =
            std::string("dev.pulp.test.isolation-") + char(Letter - 'A' + 'a');
        return {
            .name = name,
            .manufacturer = "Pulp",
            .bundle_id = id,
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    // One parameter, so a CLAP validator's parameter and state tests have
    // something to change.
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kGain, .name = "Gain", .unit = "dB", .range = {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                output.channel(c)[i] = c < input.num_channels() ? input.channel(c)[i] * gain : 0.0f;
    }
};

inline std::unique_ptr<format::Processor> create_isolation_a() {
    return std::make_unique<IsolationProcessor<'A'>>();
}
inline std::unique_ptr<format::Processor> create_isolation_b() {
    return std::make_unique<IsolationProcessor<'B'>>();
}

} // namespace pulp::test_fixtures
