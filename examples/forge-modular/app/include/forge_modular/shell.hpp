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
#include <string>

namespace forge_modular {

/// Where the generation actually runs.
///
/// The shell never compiles anything itself: it hands a prompt to the helper
/// and renders what comes back. That is what keeps a compiler out of a host's
/// plugin sandbox and stops the engine existing in two places.
class EngineClient {
public:
    virtual ~EngineClient() = default;
    virtual bool available() const = 0;
    /// Start the helper if it is not up. False means it could not be found at
    /// all, which is a real failure -- unlike merely not running yet.
    virtual bool ensure_running() = 0;
    virtual void submit(const std::string& prompt, bool patch_mode) = 0;
};

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

    /// Set before the editor opens. Without one the shell still renders; the
    /// buttons simply do nothing, which is the honest behaviour when there is
    /// no engine to reach.
    void set_engine(EngineClient* engine);

private:
    EngineClient* engine_ = nullptr;
    pulp::state::StateStore* store_ = nullptr;
    double sample_rate_ = 48000.0;
};

/// The engine this build was compiled to reach.
std::unique_ptr<EngineClient> make_engine();

/// The factory every format entry point calls.
inline std::unique_ptr<pulp::format::Processor> create_shell() {
    auto shell = std::make_unique<Shell>();
    // The engine outlives the shell: a generation started from one editor
    // should not die because that editor closed.
    static auto engine = make_engine();
    shell->set_engine(engine.get());
    return shell;
}

}  // namespace forge_modular
