#include "forge/modular_shell.hpp"

#include <forge/chrome.hpp>
#include <forge/design_tokens.hpp>

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

namespace color = forge::design::color;

// The prototype names --accent-soft but never defines it, so its selected pill
// falls back to transparent and reads only as accent-coloured text. A faint
// accent wash makes the selection unambiguous. Kept local rather than added to
// the shared palette: no Rack-specific token belongs in Forge's tokens.
constexpr auto kAccentSoft = pulp::canvas::Color::rgba8(0x16, 0xDA, 0xC2, 0x1F);
constexpr auto kTransparent = pulp::canvas::Color::rgba8(0, 0, 0, 0);

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
        .primary = false,
        .on_click = [this] { offer_random(); },
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

    tab_labels_.clear();
    const auto add_tab = [&](const char* label, Artifact which) -> TextButton* {
        auto b = std::make_unique<TextButton>();
        b->set_access_label(label);
        auto* ptr = b.get();
        // The colour lives on a child label, as Forge's own toolbar buttons do
        // -- TextButton has no per-instance text colour, and without one the
        // ghost style paints accent, so the UNSELECTED tab reads as the
        // brighter of the two.
        auto lbl = std::make_unique<pulp::view::Label>(label);
        lbl->set_font_family(forge::design::type::display);
        lbl->set_font_size(14.0f);
        lbl->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        lbl->flex().dim_height = {100, pulp::view::DimensionUnit::percent};
        lbl->set_text_align(pulp::view::LabelAlign::center);
        lbl->set_vertical_align(pulp::canvas::TextVerticalAlign::center);
        lbl->set_hit_testable(false);
        tab_labels_.push_back(lbl.get());
        b->add_child(std::move(lbl));
        // Measured: without an explicit width each tab took the FULL row width
        // (1100), so centring two of them put one at x=-554 and the other at
        // +554. justify_content was never the problem -- the children were.
        b->flex().preferred_width = 104;
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
    // The ACTIVE tab is the raised one. This read backwards in the shipped app
    // -- Module was selected while Patch wore the pill -- which is the
    // "both tabs highlighted at once" a user reported.
    const bool patch = artifact_ == Artifact::patch;
    TextButton* const buttons[] = {tab_module_, tab_patch_};
    const bool active[] = {!patch, patch};
    for (std::size_t i = 0; i < 2; ++i) {
        if (!buttons[i]) continue;
        buttons[i]->set_style(active[i] ? TextButton::Style::secondary
                                        : TextButton::Style::ghost);
        if (i < tab_labels_.size() && tab_labels_[i])
            tab_labels_[i]->set_text_color(active[i] ? color::text
                                                     : color::text_muted);
        buttons[i]->request_repaint();
    }
}

std::unique_ptr<View> ForgeModularShell::overlay_accessory() {
    // The mention list. Its candidates come from a source the app installs --
    // the 4,705-module library index -- so this file never learns where that
    // index lives.
    auto v = mentions_.build();
    mentions_.on_choose = [this](const std::string& slug) {
        if (auto* c = chrome()) {
            if (auto* input = c->prompt_input()) {
                // Replace the token being typed, rather than appending: the user
                // has already typed "@vc" and expects it to become the module,
                // not to sit in front of it.
                std::string text = input->text();
                const auto at = text.rfind('@');
                if (at != std::string::npos) text.erase(at);
                input->set_text(text + slug + " ");
            }
        }
    };
    return v;
}

std::unique_ptr<View> ForgeModularShell::build_accessory() {
    // Patches only. A module build has one artifact and nothing to narrate at
    // three depths, and a dead control is worse than no control.
    if (artifact_ != Artifact::patch) return nullptr;

    depth_tabs_.clear();
    depth_labels_.clear();

    // Built to the same recipe as the Code | Preview toggle it sits beside:
    // raised trough, 3 of padding, 2 of gap, radius 8, 26-tall buttons whose
    // colour lives on a centred child label. Matching that exactly is the
    // point -- a second segmented control with its own geometry is how two
    // products start looking like two products.
    auto group = std::make_unique<View>();
    group->flex().direction = FlexDirection::row;
    group->flex().align_items = FlexAlign::center;
    group->flex().padding = 3;
    group->flex().gap = 2;
    group->flex().flex_shrink = 0;
    group->set_background_color(color::surface_raised);
    group->set_border_radius(8);

    struct Tab { const char* label; Depth depth; float width; };
    static constexpr Tab kTabs[] = {
        {"Terse", Depth::terse, 54},
        {"Standard", Depth::standard, 72},
        {"Learning", Depth::learning, 70},
    };
    for (const auto& t : kTabs) {
        auto b = std::make_unique<TextButton>();
        b->set_access_label(t.label);
        b->flex().preferred_width = t.width;
        b->flex().preferred_height = 26;
        b->flex().flex_grow = 0;
        b->flex().flex_shrink = 0;
        b->set_overflow(View::Overflow::hidden);

        auto label = std::make_unique<pulp::view::Label>(t.label);
        label->set_font_family(forge::design::type::display);
        label->set_font_size(14.0f);
        label->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        label->flex().dim_height = {100, pulp::view::DimensionUnit::percent};
        label->set_text_align(pulp::view::LabelAlign::center);
        label->set_vertical_align(pulp::canvas::TextVerticalAlign::center);
        label->set_hit_testable(false);
        depth_labels_.push_back(label.get());
        b->add_child(std::move(label));

        const auto depth = t.depth;
        b->on_click = [this, depth]() { set_depth(depth); };
        depth_tabs_.push_back(b.get());
        group->add_child(std::move(b));
    }
    refresh_depth_tabs();
    return group;
}

std::unique_ptr<View> ForgeModularShell::stage_accessory() {
    auto column = std::make_unique<View>();
    column->flex().direction = FlexDirection::column;
    column->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    column->flex().flex_grow = 1;
    column->flex().min_height = 0;
    column->flex().gap = 10;

    auto preview = std::make_unique<RackPreview>();
    rack_preview_ = preview.get();
    // Fills the stage: a rack shown in a postage stamp cannot be read, and
    // reading it is the point.
    preview->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    preview->flex().flex_grow = 1;
    preview->flex().min_height = 0;
    column->add_child(std::move(preview));

    auto explanation = std::make_unique<PatchExplanation>();
    explanation_ = explanation.get();
    explanation->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    explanation->flex().flex_grow = 0;
    explanation->flex().flex_shrink = 0;
    // The pairing that makes this worth drawing: point at a sentence, the
    // cable it names lights and the rest recede.
    explanation->on_hover = [this](std::optional<std::size_t> index) {
        if (rack_preview_) rack_preview_->set_highlight(index);
    };
    column->add_child(std::move(explanation));
    return column;
}

void ForgeModularShell::show_rack(std::vector<RackModule> modules,
                                  std::vector<Connection> connections) {
    if (!rack_preview_) return;
    const bool have_rack = !modules.empty();
    if (explanation_) {
        explanation_->set_connections(connections, modules);
        explanation_->set_depth(static_cast<ExplainDepth>(depth_));
    }
    rack_preview_->set_rack(std::move(modules), std::move(connections));
    // An empty rack keeps the skeleton up rather than showing a blank stage,
    // which would read as a finished build that produced nothing.
    if (auto* c = chrome()) c->show_stage_accessory(have_rack);
}

void ForgeModularShell::set_depth(Depth d) {
    if (d == depth_) return;
    depth_ = d;
    refresh_depth_tabs();
    // The tab is not a preference stored for later: the explanation on screen
    // rewrites itself now, or the control looks broken.
    if (explanation_) explanation_->set_depth(static_cast<ExplainDepth>(d));
}

void ForgeModularShell::refresh_depth_tabs() {
    for (std::size_t i = 0; i < depth_tabs_.size(); ++i) {
        auto* b = depth_tabs_[i];
        if (!b) continue;
        const bool on = static_cast<int>(depth_) == static_cast<int>(i);
        // Selected reads as raised-and-bright, unselected as quiet. NOT accent:
        // an accent-coloured sibling looks more chosen than the chosen one,
        // which is exactly how two tabs came to look highlighted at once.
        b->set_style(on ? TextButton::Style::secondary : TextButton::Style::ghost);
        if (i < depth_labels_.size() && depth_labels_[i])
            depth_labels_[i]->set_text_color(on ? color::text : color::text_muted);
        b->request_repaint();
    }
}

void ForgeModularShell::watch_build_log(const std::string& path) {
    monitor_.watch(path);
}

int ForgeModularShell::pump_build_log() {
    const auto added = monitor_.poll();
    auto* c = chrome();
    if (!c) return static_cast<int>(added.size());

    for (const auto& line : added) {
        // A refusal and an error are the two a person must not miss, so they
        // are the two the chat marks. A gate rejection is deliberately NOT an
        // error: it is the pipeline working, and marking it red teaches people
        // to ignore the colour.
        const bool alarming = line.kind == BuildLine::Kind::refusal ||
                              line.kind == BuildLine::Kind::error;
        c->narrate(line.text, alarming);
    }
    return static_cast<int>(added.size());
}

void ForgeModularShell::offer_random() {
    // Fills the composer rather than building. A suggestion you cannot read
    // before committing to it is a dice roll, not a prompt.
    const bool patch = artifact_ == Artifact::patch;
    const auto& pool = patch ? kRandomPatch : kRandomModule;
    const std::size_t count = patch ? std::size(kRandomPatch) : std::size(kRandomModule);
    if (count == 0) return;

    // Never the same suggestion twice running: drawing the prompt just
    // dismissed reads as a broken button, which is how this was reported.
    std::size_t pick = next_random_ % count;
    if (count > 1 && pool[pick] == last_random_) pick = (pick + 1) % count;
    next_random_ = pick + 1;
    last_random_ = pool[pick];

    if (auto* c = chrome()) {
        if (auto* input = c->prompt_input()) input->set_text(last_random_);
    }
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
