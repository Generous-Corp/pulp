#include "forge/modular_shell.hpp"

#include <forge/chrome.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cstddef>

namespace forge_modular {

namespace {

using pulp::view::FlexAlign;
using pulp::view::FlexDirection;
using pulp::view::TextButton;
using pulp::view::View;

// Suggestions the Random button offers. Deliberately concrete: "a filter" is not
// a prompt anybody can judge, and the point of showing it before building is
// that it can be read and edited.
const char* const kRandomModule[] = {
    "a 12 HP wavefolder with drive and symmetry, plus a CV input for the fold amount",
    "an 8 HP slew limiter with separate rise and fall, and an end-of-rise gate",
    "a 6 HP sample and hold with an internal noise source and a track mode",
    "a 10 HP chaotic modulation source with rate, character and two decorrelated outputs",
    "an 8 HP resonant lowpass gate with a vactrol-style decay",
};
const char* const kRandomPatch[] = {
    "an ambient generative drone that never repeats",
    "a bouncing-ball rhythm that slows down as it settles",
    "a krell patch where each note chooses the next one's length",
    "an acid line with accent and slide from a single sequencer",
};

}  // namespace

ForgeModularShell::ForgeModularShell()
    : forge::ForgeShell(forge::ShellKind::effect) {
    // ShellKind::effect rather than a new enum value. Forge Modular needs none of
    // the kind-specific behaviour left in the chrome -- everything it varies now
    // comes through chrome_copy(), composer_row() and home_accessory() -- so
    // adding a fourth value would mean editing all 13 remaining switch sites for
    // no gain, and putting a Rack-shaped name in Forge's core enum. The badge
    // does not come from the kind any more, so nothing reads "FORGE FX".
}

pulp::format::PluginDescriptor ForgeModularShell::descriptor() const {
    pulp::format::PluginDescriptor d;
    d.name = std::string(forge::brand::kProductName) + " Modular";
    d.manufacturer = std::string(forge::brand::kManufacturer);
    d.bundle_id = std::string(forge::brand::kBundlePrefix) + ".modular";
    d.version = std::string(forge::brand::kVersion);
    d.category = pulp::format::PluginCategory::Effect;
    return d;
}

void ForgeModularShell::prepare(const pulp::format::PrepareContext& ctx) {
    sample_rate_ = ctx.sample_rate;
    block_size_ = ctx.max_buffer_size;
}

void ForgeModularShell::set_artifact(Artifact a) {
    if (artifact_ == a) return;
    artifact_ = a;
    style_tabs();
    // Every string on the home screen depends on this -- the hero, the badge,
    // the Build label -- so the chrome is told rather than left to disagree
    // with the tab the user just pressed.
    if (auto* c = chrome()) c->refresh_copy();
}

forge::ChromeCopy ForgeModularShell::chrome_copy() const {
    const bool patch = artifact_ == Artifact::patch;
    return {
        .badge = patch ? "PATCH" : "MODULE",
        .prompt_placeholder =
            patch ? "an ambient generative drone that never repeats"
                  : "a 12 HP wavefolder with drive and symmetry, plus a CV input "
                    "for the fold amount",
        .followup_placeholder =
            patch ? "refine it \xE2\x80\x94 e.g. give the drone a slower filter sweep"
                  : "refine it \xE2\x80\x94 e.g. add a second CV input for symmetry",
        .hero_eyebrow = std::string(forge::brand::kProductNameUpper) +
                        " MODULAR \u00b7 FOR VCV RACK",
        .hero_title = patch ? "What should the patch do?"
                            : "What should the module do?",
        .default_build_title = patch ? "Ambient Drone" : "Wavefolder",
    };
}

forge::ComposerRow ForgeModularShell::composer_row() {
    const bool patch = artifact_ == Artifact::patch;
    forge::ComposerRow row;

    // Mention: types an @ into the composer. Icon-only, like Forge's attach tile.
    row.left.push_back({
        .label = "",
        .access_label = "Mention a module from the library",
        .icon = forge::ComposerAction::Icon::search,
    });

    // Random fills the composer rather than building. A suggestion you cannot
    // read before committing to it is a dice roll, not a prompt.
    row.left.push_back({
        .label = "Random",
        .access_label = patch ? "Random patch idea" : "Random module idea",
        .icon = forge::ComposerAction::Icon::dice,
    });

    // Ask and Build differ in one bit and it is carried, not inferred: an Ask
    // that could rewrite the artifact would destroy work on a misread intent.
    row.right.push_back({
        .label = "Ask",
        .access_label = "Ask a question without changing anything",
        .icon = forge::ComposerAction::Icon::none,
    });
    row.right.push_back({
        .label = patch ? "Build patch" : "Build module",
        .access_label = patch ? "Build the patch" : "Build the module",
        .icon = forge::ComposerAction::Icon::arrow_up,
        .primary = true,
    });
    return row;
}

std::unique_ptr<View> ForgeModularShell::home_accessory() {
    // The Module | Patch tabs. Forge makes one kind of thing and needs no
    // equivalent, which is why this arrives through a hook instead of living in
    // the chrome.
    //
    // TextButton rather than a hand-rolled row: it already sets
    // pointer_events(box_only), so a label centred inside cannot swallow the
    // click. That exact bug cost this project a shell where nothing was
    // clickable; the widget had solved it all along.
    auto tabs = std::make_unique<View>();
    tabs->flex().direction = FlexDirection::row;
    tabs->flex().align_items = FlexAlign::center;
    // Full width, contents centred. Relying on the parent to centre a
    // content-sized row did not hold -- the hero stretches its children, so the
    // row spanned the width and threw the two tabs to opposite edges.
    tabs->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    tabs->flex().justify_content = pulp::view::FlexJustify::center;
    tabs->flex().gap = 8;

    tab_module_ = nullptr;
    tab_patch_ = nullptr;

    const auto add_tab = [&](const char* label, Artifact which) -> TextButton* {
        auto b = std::make_unique<TextButton>(label);
        auto* ptr = b.get();
        b->flex().preferred_height = 30;
        b->flex().flex_grow = 0;
        b->flex().flex_shrink = 0;
        b->flex().padding_left = 16;
        b->flex().padding_right = 16;
        b->on_click = [this, which] {
            if (artifact_ == which) return;   // clicking the active tab is a no-op
            set_artifact(which);
        };
        tabs->add_child(std::move(b));
        return ptr;
    };

    tab_module_ = add_tab("Module", Artifact::module);
    tab_patch_ = add_tab("Patch", Artifact::patch);
    style_tabs();
    return tabs;
}

void ForgeModularShell::style_tabs() {
    // Only one reads as selected. Two toggles that look exclusive have to BE
    // exclusive, or the mode becomes whichever was clicked last rather than the
    // one being shown.
    const bool patch = artifact_ == Artifact::patch;
    if (tab_module_)
        tab_module_->set_style(patch ? TextButton::Style::secondary
                                     : TextButton::Style::ghost);
    if (tab_patch_)
        tab_patch_->set_style(patch ? TextButton::Style::ghost
                                    : TextButton::Style::secondary);
}

void ForgeModularShell::process_audio(
    pulp::audio::BufferView<float>& audio_output,
    const pulp::audio::BufferView<const float>& audio_input,
    pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
    const pulp::format::ProcessContext& context) {
    sample_rate_ = context.sample_rate > 0 ? context.sample_rate : sample_rate_;
    block_size_ = static_cast<int>(audio_output.num_samples());

    // Pass through untouched. Being in the audio path at all is the cost of an
    // insert slot; changing the signal is not, and a plugin that quietly altered
    // a track it was only meant to sit on would be a genuinely nasty surprise.
    const std::size_t chans =
        std::min(audio_output.num_channels(), audio_input.num_channels());
    const std::size_t frames =
        std::min(audio_output.num_samples(), audio_input.num_samples());
    for (std::size_t c = 0; c < chans; ++c) {
        const float* src = audio_input.channel_ptr(c);
        float* dst = audio_output.channel_ptr(c);
        if (src != dst) std::copy(src, src + frames, dst);
    }
    // Any output channel the input does not reach is silence, not stale memory.
    for (std::size_t c = chans; c < audio_output.num_channels(); ++c) {
        float* dst = audio_output.channel_ptr(c);
        std::fill(dst, dst + audio_output.num_samples(), 0.0f);
    }
}

bool ForgeModularShell::install_generated_bundle(
    const forge::gen::Bundle&, double sample_rate, int block_size,
    const std::function<void(forge::gen::GenStage, const std::string&)>&,
    BundleInstallResult&, std::string& err,
    std::span<const forge::gen::EffectAssetBinding>,
    const std::function<bool()>&, const std::function<bool()>&) {
    sample_rate_ = sample_rate;
    block_size_ = block_size;

    // Not yet wired. The other shells lower a bundle onto their DSP and hot-swap
    // it; Forge Modular's artifact is a .vcvplugin that Rack loads once at
    // startup, so "install" means packaging and placing a file, and a new module
    // needs a Rack restart before it exists. That path lives in the generator
    // and is joined to this hook in Phase 7.
    //
    // Reporting false with a reason rather than true-and-nothing: a success that
    // installed nothing is the failure mode this project has hit most often.
    err = "installing a generated Rack artifact is not wired yet";
    return false;
}

}  // namespace forge_modular
