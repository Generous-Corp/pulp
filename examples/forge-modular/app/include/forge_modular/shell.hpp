#pragma once

// The one shell, behind every entry point: standalone, AU, VST3 and CLAP.
//
// All four formats are built because Rack Pro ships in all of them, so
// whichever a person's DAW is built around they are served. A plugin with no
// DSP is the cheapest possible thing to ship four times, and Pulp's adapters
// already carry each one -- picking a subset would only save build time at the
// cost of somebody's DAW being unsupported.

#include <pulp/format/processor.hpp>
#include <pulp/view/view.hpp>

#include <memory>

namespace forge_modular {

class Shell : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& ctx) override;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& ctx) override;

    std::unique_ptr<pulp::view::View> create_view() override;

private:
    pulp::state::StateStore* store_ = nullptr;
    double sample_rate_ = 48000.0;
};

/// The factory every format entry point calls.
inline std::unique_ptr<pulp::format::Processor> create_shell() {
    return std::make_unique<Shell>();
}

}  // namespace forge_modular
