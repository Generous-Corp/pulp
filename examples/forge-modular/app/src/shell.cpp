// Forge Modular's shell, behind every entry point: standalone, AU, VST3, CLAP.
//
// One implementation, four ways in. The formats differ in how a host loads
// them and in nothing that matters here -- the editor, the chat and the engine
// client are the same objects either way.
//
// It makes no sound. The audio path is a pass-through because an insert slot
// is where this belongs: Rack Pro wants the instrument slot, and a chat window
// should not cost a whole track. Why not an instrument, and why not (yet) a
// MIDI effect, is in DECISIONS.md.

#include "forge_modular/shell.hpp"

#include <pulp/view/scripted_ui.hpp>

#include <algorithm>
#include <filesystem>

namespace forge_modular {
namespace {

/// A root that owns its scripted session, so the JS tree lives exactly as long
/// as the editor does. A host may open and close the editor repeatedly; the
/// session must not outlive the view it built.
class ShellRoot : public pulp::view::View {
public:
    ShellRoot(pulp::state::StateStore& store, const std::filesystem::path& ui_dir) {
        pulp::view::ScriptedUiOptions opts;
        opts.script_path = ui_dir / "main.js";
        opts.theme_path = ui_dir / "theme.json";
        opts.asset_roots = {ui_dir};
        // On during development so ui/main.js can be edited and reloaded
        // without a rebuild; the packaged build turns it off.
        opts.enable_hot_reload = FORGE_MODULAR_UI_HOT_RELOAD;
        session_ = std::make_unique<pulp::view::ScriptedUiSession>(
            *this, store, std::move(opts));
        std::string error;
        if (!session_->load(&error)) {
            // A UI that failed to load must say so rather than presenting an
            // empty window, which reads as a hung app rather than a broken
            // script.
            load_error_ = error.empty() ? "the interface script did not load"
                                        : error;
        }
    }

    const std::string& load_error() const { return load_error_; }

private:
    std::unique_ptr<pulp::view::ScriptedUiSession> session_;
    std::string load_error_;
};

}  // namespace

pulp::format::PluginDescriptor Shell::descriptor() const {
    pulp::format::PluginDescriptor d;
    d.name = "Forge Modular";
    d.manufacturer = "Generous";
    d.bundle_id = "com.generous.forgemodular";
    d.version = "0.1.0";
    d.category = pulp::format::PluginCategory::Effect;
    return d;
}

void Shell::define_parameters(pulp::state::StateStore& store) {
    // Deliberately none. What this produces is a file on disk, not a sound, so
    // there is nothing a host should automate -- and a parameter that did
    // nothing would only invite somebody to draw an envelope on it.
    store_ = &store;
}

void Shell::prepare(const pulp::format::PrepareContext& ctx) {
    sample_rate_ = ctx.sample_rate;
}

void Shell::process(pulp::audio::BufferView<float>& out,
                    const pulp::audio::BufferView<const float>& in,
                    pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                    const pulp::format::ProcessContext&) {
    // Through, untouched. Sitting in the audio path is the price of an insert;
    // altering a track we were only meant to sit on would be a nasty surprise.
    const std::size_t chans = std::min(out.num_channels(), in.num_channels());
    const std::size_t frames = std::min(out.num_samples(), in.num_samples());
    for (std::size_t c = 0; c < chans; ++c) {
        const float* src = in.channel_ptr(c);
        float* dst = out.channel_ptr(c);
        if (src != dst) std::copy(src, src + frames, dst);
    }
}

std::unique_ptr<pulp::view::View> Shell::create_view() {
    if (!store_) return nullptr;
    return std::make_unique<ShellRoot>(*store_,
                                       std::filesystem::path(FORGE_MODULAR_UI_DIR));
}

}  // namespace forge_modular
