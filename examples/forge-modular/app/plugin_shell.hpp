#pragma once

// Forge Modular as a plugin, so a module or a patch can be made without
// leaving the sequencer.
//
// It is an **audio effect**, passing signal through untouched. Rack Pro wants
// the instrument slot, and if we take one too a whole track is spent on a chat
// window; an insert puts both on the same track, Rack Pro above and this
// below. `MidiEffect` would be more honest -- it makes no sound and touches no
// signal -- but that placement is Logic-shaped and reads as odd everywhere
// else.
//
// It is a **thin client**, not a second copy of the engine. Generating a
// module spawns `claude` and runs `clang++`, and Logic sandboxes AUs
// out-of-process, which may block spawning a compiler from inside a plugin.
// So the work happens in the app or a small helper and this is the same chat
// talking to it. One implementation, no sandbox exposure, and a plugin small
// enough to sit in a session beside real instruments.
//
// **This cannot instantiate Rack Pro, and must not appear to.** No plugin can
// create another plugin in its host, or tell its host to open a file. From a
// DAW we prepare the artifact and say where it is; the last step is the user's.
// A button that silently did nothing would be worse than saying so.

#include <pulp/format/processor.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace forge_modular {

/// Where the generation actually runs.
///
/// The plugin never compiles anything itself. It hands a prompt to the helper
/// and renders what comes back, which is what keeps it out of the host's
/// sandbox and stops the engine existing twice.
class EngineClient {
public:
    virtual ~EngineClient() = default;

    /// True when the helper is reachable. False means it is not running and
    /// has not been started yet, not that anything has failed.
    virtual bool available() const = 0;

    /// Start the helper if it is not up. Returns false when it could not be
    /// found at all -- which is a real failure worth reporting, unlike being
    /// merely not-yet-running.
    virtual bool ensure_running() = 0;

    virtual void submit(const std::string& prompt, bool patch_mode) = 0;
};

/// A plugin that makes no sound.
///
/// Declares itself an effect with a stereo pass-through so a host has
/// something coherent to route, and reports no latency and no tail so nothing
/// adds delay compensation for a plugin that does not touch the signal.
class ForgeModularShell : public pulp::format::Processor {
public:
    explicit ForgeModularShell(EngineClient* engine = nullptr)
        : engine_(engine) {}

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Forge Modular";
        d.manufacturer = "Generous";
        d.bundle_id = "com.generous.forgemodular";
        d.version = "0.1.0";
        d.category = pulp::format::PluginCategory::Effect;
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {
        // None. There is nothing here a host should automate: the artifact is
        // a file on disk, not a sound, and a parameter that did nothing would
        // only invite someone to draw an envelope on it.
    }

    void prepare(const pulp::format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
    }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        // Pass through untouched. Being in the audio path at all is the cost
        // of an insert slot; changing the signal is not, and a plugin that
        // quietly altered a track it was only meant to sit on would be a
        // genuinely nasty surprise.
        const std::size_t chans =
            std::min(out.num_channels(), in.num_channels());
        const std::size_t frames =
            std::min(out.num_samples(), in.num_samples());
        for (std::size_t c = 0; c < chans; ++c) {
            const float* src = in.channel_ptr(c);
            float* dst = out.channel_ptr(c);
            if (src != dst) std::copy(src, src + frames, dst);
        }
    }

    /// Zero, and truthfully so: nothing here delays or tails a signal.
    int latency_samples() const { return 0; }
    double tail_seconds() const { return 0.0; }

    /// What the shell may offer, given where it is running.
    ///
    /// From a DAW this is always false for launching: the Rack Pro instance
    /// beside us has to be added by the user, because no plugin can create
    /// another. The UI reads this rather than assuming, so it never shows a
    /// button that cannot work.
    struct Reach {
        bool rack_installed = false;
        bool can_launch_rack = false;   ///< only ever true from the standalone app
        bool engine_available = false;
        std::string note;
    };

    Reach reach() const {
        Reach r;
        r.engine_available = engine_ && engine_->available();
        r.can_launch_rack = false;
        r.note = "prepared here; open it in the Rack instance on this track";
        return r;
    }

private:
    EngineClient* engine_ = nullptr;
    double sample_rate_ = 48000.0;
};

}  // namespace forge_modular
