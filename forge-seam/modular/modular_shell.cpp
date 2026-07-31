#include "forge/modular_shell.hpp"

#include <forge/chrome.hpp>
#include <forge/design_tokens.hpp>
#include <forge/patch_loader.hpp>
#include <forge/process_engine.hpp>

#include <pulp/runtime/log.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#include <cstdlib>
#include <filesystem>

#include <pulp/canvas/canvas.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cstddef>

namespace forge_modular {

namespace {

/// Where the generator lives. A source checkout wins when present so a
/// developer's edits are what runs; the bundle's own copy is the fallback.
std::string tools_dir() {
    if (const char* env = std::getenv("FORGE_MODULAR_TOOLS"); env && *env) return env;
    const char* home = std::getenv("HOME");
    // Application Support first, and deliberately NOT a source checkout on an
    // external volume. macOS gates removable-volume access behind a MODAL
    // consent dialog; touching such a path from the UI thread parks the whole
    // app behind that modal, which is what read as a freeze on Build. Nothing
    // under Application Support is gated.
    const std::string installed = std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/tools/rack";
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(installed) / "patch.py", ec))
        return installed;
    // A developer with no installed copy can still point at their checkout,
    // consent once, and carry on.
    return "/Volumes/Workshop/Code/pulp-modular-rack/tools/rack";
}

std::string build_log_path() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") +
           "/Library/Application Support/Forge Modular/last-run.log";
}

/// One engine per process. The shell holds a raw pointer, so it must outlive
/// every editor a host opens and closes.
ProcessEngine& shared_engine() {
    static ProcessEngine instance(tools_dir(), build_log_path());
    return instance;
}

}  // namespace

std::unique_ptr<pulp::format::Processor> create_forge_modular() {
    auto shell = std::make_unique<ForgeModularShell>();
    shell->set_engine(&shared_engine());
    shell->watch_build_log(build_log_path());
    return shell;
}


namespace {

using pulp::view::FlexAlign;
using pulp::view::FlexDirection;
using pulp::view::TextButton;
using pulp::view::View;

namespace color = forge::design::color;

/// "12s", "1m 04s". Seconds below a minute, because a model call that has run
/// for eleven seconds and one that has run for eleven minutes should not look
/// alike at a glance.
std::string format_elapsed(std::chrono::steady_clock::time_point since) {
    using namespace std::chrono;
    const auto secs = duration_cast<seconds>(steady_clock::now() - since).count();
    if (secs < 60) return std::to_string(secs) + "s";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lldm %02llds",
                  static_cast<long long>(secs / 60),
                  static_cast<long long>(secs % 60));
    return buf;
}

/// Leading whitespace off, and long lines cut. The status card is one or two
/// lines tall; a 200-character compiler path pushes everything else out of it.
std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    text = text.substr(first);
    constexpr std::size_t kMax = 120;
    if (text.size() > kMax) text = text.substr(0, kMax) + "\u2026";
    return text;
}

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
    if (auto* c = chrome()) { c->refresh_copy(); c->refresh_composer_row(); }
    // The depth tabs belong to patches; revealing them here is what makes
    // switching artifact mid-session work at all.
    if (depth_group_) depth_group_->set_visible(artifact_ == Artifact::patch);
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
        // Neutral until the generator names it. A leftover example name on a
        // session building something else reads as another project's work.
        .default_build_title = patch ? "Untitled patch" : "Untitled module",
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
        .primary = false,
        .on_click = [this] { begin_mention(); },
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
        .primary = false,
        .on_click = [this] { ask(); },
    });
    row.right.push_back({
        .label = patch ? "Create patch" : "Build module",
        .access_label = patch ? "Build the patch" : "Build the module",
        .icon = forge::ComposerAction::Icon::arrow_up,
        .primary = true,
        .on_click = [this] { start_build(); },
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
    // Always mounted, shown only for patches. Returning nullptr for a module
    // meant the title bar was built without the control and switching to Patch
    // afterwards could never reveal it -- the bar is built once, when the
    // editor opens.
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
    // Open in Rack, beside the depth tabs. Hidden until something has actually
    // been built: a button that cannot work yet teaches people to distrust it.
    auto open_btn = std::make_unique<TextButton>();
    open_btn->set_access_label("Open in VCV Rack");
    open_btn->set_style(TextButton::Style::primary);
    open_btn->flex().preferred_width = 118;
    open_btn->flex().preferred_height = 26;
    open_btn->flex().flex_grow = 0;
    open_btn->flex().flex_shrink = 0;
    {
        auto lbl = std::make_unique<pulp::view::Label>("Open in Rack");
        lbl->set_font_family(forge::design::type::display);
        lbl->set_font_size(13.0f);
        // Dark ink on the accent fill. A primary button's background is light,
        // so a label left at its default colour renders white on mint and is
        // barely readable -- the same treatment Forge's own primary buttons
        // use for exactly this reason.
        lbl->set_text_color(forge::design::color::accent_text);
        lbl->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        lbl->flex().dim_height = {100, pulp::view::DimensionUnit::percent};
        lbl->set_text_align(pulp::view::LabelAlign::center);
        lbl->set_vertical_align(pulp::canvas::TextVerticalAlign::center);
        lbl->set_hit_testable(false);
        open_btn->add_child(std::move(lbl));
    }
    open_btn->on_click = [this] {
        const auto why = open_in_rack();
        if (!why.empty())
            if (auto* c = chrome()) c->narrate(why, /*alarming=*/true);
    };
    open_button_ = open_btn.get();
    open_btn->set_visible(false);

    refresh_depth_tabs();
    depth_group_ = group.get();
    // A module build has one artifact and nothing to narrate at three depths,
    // and a dead control is worse than no control.
    // The tabs are patch-only; the group itself must stay visible for a
    // module build so Open in Rack can appear there too.
    for (auto* b : depth_tabs_)
        if (b) b->set_visible(artifact_ == Artifact::patch);
    // Mounted INSIDE the tab group rather than in a wrapper around it.
    // Wrapping the group in an extra row crashed the renderer; adding a
    // sibling to a container that already lays out a row of buttons does not.
    // Hidden until something exists to open: a button that cannot work yet
    // teaches people to distrust it.
    group->add_child(std::move(open_btn));
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

ForgeModularShell::ModelRoles ForgeModularShell::roles_for(Artifact artifact) {
    switch (artifact) {
        // C++ for the DSP, and a panel to draw.
        case Artifact::module: return {true, true};
        // No DSP is written at all: the modules already exist, and what is
        // produced is a wiring plan plus the prose that explains it.
        case Artifact::patch:  return {false, true};
    }
    return {true, true};
}

void ForgeModularShell::begin_mention() {
    auto* c = chrome();
    if (!c) return;
    auto* input = c->prompt_input();
    if (!input) return;
    // Type the '@' rather than opening the list behind the composer's back, so
    // the prompt and the list can never disagree about what was typed.
    auto text = input->text();
    if (!text.empty() && text.back() != ' ') text += ' ';
    text += '@';
    input->set_text(text);
    // The overlay is told the caret is at the end, which is where set_text
    // leaves it, so the next keystroke narrows the list.
    mentions_.handle_text(text, text.size());
}

std::string ForgeModularShell::submit_own(const std::string& prompt) {
    // Every route into a build lands here -- the Build button, Enter in the
    // prompt, a follow-up in the transcript -- so none of them can reach
    // Forge's plugin pipeline by a path this shell forgot about.
    return start_build_with(prompt);
}

std::string ForgeModularShell::start_build() {
    auto* c = chrome();
    if (!c) return "no editor is open";
    auto* input = c->prompt_input();
    const auto prompt = input ? input->text() : std::string{};
    // An empty prompt is a no-op with a reason, not a silent nothing.
    if (prompt.find_first_not_of(" \t\n") == std::string::npos)
        return "type what you want first";
    return start_build_with(prompt);
}

std::string ForgeModularShell::start_build_with(const std::string& prompt) {
    auto* c = chrome();
    if (!c) return "no editor is open";
    auto* input = c->prompt_input();
    if (!engine_) return "the generator is not connected";
    if (!engine_->available())
        return "the generator is not installed";
    if (!engine_->ensure_running()) {
        auto why = engine_->last_error();
        return why.empty() ? "the generator would not start" : why;
    }

    // A prompt typed on Home starts a NEW project. Continuing the last one
    // makes two unrelated builds share a transcript, and the second inherits
    // the first one's stage -- reported after seeing one composer hold two
    // different module requests. Only opening an existing project should show
    // its old conversation.
    // EVERY build starts its own clock and its own card. The run clock was set
    // once when the editor opened, so a second build showed the time since the
    // app launched -- "asking the model · 5m 27s elapsed" beside a Thinking
    // chip reading 25s, two numbers for the same thing disagreeing.
    run_started_ = std::chrono::steady_clock::now();
    stage_started_ = run_started_;
    reported_outcome_ = BuildOutcome::running;
    reported_stage_ = -2;

    // Forget the previous run's output. The monitor only resets when the log
    // SHRINKS, and a longer new run never trips that -- so at the first tick
    // the old "installed" line was still there, the outcome already read done,
    // and the stage loaded the PREVIOUS artifact: a patch build showing one
    // module named SLEWRF. Worse, the once-only guard then refused to report
    // the real result when it arrived.
    if (watching_) monitor_.watch(monitor_log_path_);
    c->set_status_note({});
    c->set_status_activity({});
    c->set_active_stage(-1);

    // A prompt typed on Home starts a NEW project, so the transcript goes too.
    // A follow-up refines the project already open and keeps its conversation;
    // only the clock and the card restart.
    if (c->mode() == forge::ForgeChrome::Mode::Home) {
        c->begin_new_session();
        open_patch_.clear();
    }

    // Move to the Build screen BEFORE submitting: a user who presses Build and
    // stays on Home cannot tell whether anything happened, which is exactly
    // what was reported.
    c->enter_build();
    // Name it from the request until the generator reports the real name.
    c->set_project_title_from_prompt(prompt);
    pulp::runtime::log_info("Forge Modular: Build pressed; mode is now {}",
                            c->mode() == forge::ForgeChrome::Mode::Build
                                ? "Build" : "NOT Build");
    c->narrate(prompt.substr(0, 200));
    if (input) input->set_text("");
    engine_->submit(prompt, artifact_ == Artifact::patch);
    return engine_->last_error();
}

std::string ForgeModularShell::open_patch_file(const std::string& path) {
    auto loaded = load_patch(path);
    if (!loaded.ok())
        return loaded.error.empty() ? "that patch has no modules in it"
                                    : loaded.error;
    open_patch_ = path;
    // Opening a patch implies the patch view: the depth tabs are patch-only,
    // and a loaded patch with no way to change its explanation depth is the
    // control missing exactly when it is wanted.
    set_artifact(Artifact::patch);
    show_rack(std::move(loaded.modules), std::move(loaded.connections));
    return {};
}

std::string ForgeModularShell::artifact_path() const {
    // Read back from what the generator said rather than kept as state: the
    // log is the record, and a second copy could disagree with it.
    //
    // The line is:  open it with:  "<rack binary>" <patch path>
    // Both paths contain spaces, so the patch cannot be found by scanning back
    // to a separator -- doing that returned "/dualatt.vcv", the last segment
    // only, and Open in Rack reported a file that was not there. Anchor on the
    // closing quote of the quoted binary instead: everything after it is the
    // patch, spaces and all.
    for (const auto& line : monitor_.lines()) {
        if (line.text.find(".vcv") == std::string::npos) continue;

        // Two generators, two formats. patch.py says
        //   built 8 modules, 10 cables -> /tmp/forge-patch.vcv
        // with no quoted binary, so the quote-anchored parse below finds
        // nothing and the button never appears -- while the screen says
        // "Built. Open it in Rack." Reading only one format is what made a
        // patch build claim something it could not do.
        const auto arrow = line.text.find("\xE2\x86\x92");
        if (arrow != std::string::npos && line.text.rfind('"') == std::string::npos) {
            auto path = line.text.substr(arrow + 3);
            const auto first = path.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            const auto last = path.find_last_not_of(" \t\r\n");
            path = path.substr(first, last - first + 1);
            if (path.size() > 4 && path.substr(path.size() - 4) == ".vcv") return path;
            continue;
        }

        const auto quote = line.text.rfind('"');
        if (quote == std::string::npos) continue;
        auto path = line.text.substr(quote + 1);
        // Trim the single space the generator puts between them, and anything
        // trailing.
        const auto first = path.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const auto last = path.find_last_not_of(" \t\r\n");
        path = path.substr(first, last - first + 1);
        if (path.size() > 4 && path.substr(path.size() - 4) == ".vcv") return path;
    }

    // Nothing in this session's log. A project reopened from the shelf has no
    // build behind it, and offering no way into Rack for work that already
    // exists is the wrong answer -- the artifact is on disk either way.
    if (!open_patch_.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(open_patch_, ec)) return open_patch_;
    }
    return {};
}

namespace {

/// Where a Rack artifact can be opened on this machine.
struct RackPresence {
    bool standalone_installed = false;
    bool standalone_running = false;
    bool plugin_installed = false;     ///< Rack Pro as AU/VST3/CLAP
};

RackPresence look_for_rack() {
    RackPresence r;
    std::error_code ec;
    for (const char* app : {"/Applications/VCV Rack 2 Free.app",
                            "/Applications/VCV Rack 2 Pro.app",
                            "/Applications/VCV Rack 2.app"}) {
        if (std::filesystem::exists(app, ec)) { r.standalone_installed = true; break; }
    }
    std::string out;
    // -x so "Rack" does not match "Rack SDK" or a path that merely mentions it.
    r.standalone_running = ProcessEngine::run("pgrep -x Rack >/dev/null 2>&1", out) == 0;

    const char* home = std::getenv("HOME");
    const std::string h = home ? home : ".";
    for (const auto& p : {h + "/Library/Audio/Plug-Ins/VST3/VCV Rack 2.vst3",
                          h + "/Library/Audio/Plug-Ins/CLAP/VCV Rack 2.clap",
                          h + "/Library/Audio/Plug-Ins/Components/VCV Rack 2.component",
                          std::string("/Library/Audio/Plug-Ins/VST3/VCV Rack 2.vst3")}) {
        if (std::filesystem::exists(p, ec)) { r.plugin_installed = true; break; }
    }
    return r;
}

}  // namespace

std::string ForgeModularShell::open_in_rack() {
    // Re-checked at click time, not just at paint time. A generation can start
    // between the two, and opening a file being rewritten is worse than
    // refusing.
    if (monitor_.outcome() == BuildOutcome::running)
        return "a build is running \u2014 the patch is being rewritten";
    const auto path = artifact_path();
    if (path.empty()) return "nothing has finished building yet";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return "the generator named a file that is not there: " + path;

    const auto rack = look_for_rack();

    auto launch = [path] {
        // A running Rack takes the file into the instance already open rather
        // than starting a second one, which is what `open` does with a live
        // application -- and two Racks fighting for one audio device is the
        // silence this button exists to avoid.
        std::thread([path] {
            std::string out;
            ProcessEngine::run("open -a \"/Applications/VCV Rack 2 Free.app\" '" +
                               path + "' &", out);
        }).detach();
    };

    // Already open: hand it the patch, wherever we are running. `open` on a
    // live application routes the file into THAT instance rather than starting
    // a second one.
    if (rack.standalone_running) {
        launch();
        // Hosted always says something. From inside a DAW the Rack window may
        // come up behind the host, and silence there is indistinguishable from
        // a button that did nothing.
        return is_standalone() ? std::string{}
                               : std::string("sent to the VCV Rack window "
                                             "already open");
    }

    // Hosted, with Rack available as a plugin: the patch belongs in THIS
    // session, not in a second application competing for the audio device.
    // We cannot insert a plugin into a running host from outside -- no host
    // exposes that -- so say where the patch is and let the user place Rack.
    if (!is_standalone() && rack.plugin_installed) {
        return "add VCV Rack to a track and open this patch in it: " + path;
    }

    if (rack.standalone_installed) {
        launch();
        return is_standalone()
                   ? std::string{}
                   : std::string("opening VCV Rack \u2014 it will take an "
                                 "audio device");
    }

    // No Rack at all. Show the file rather than describing it.
    std::thread([path] {
        std::string out;
        ProcessEngine::run("open -R '" + path + "' &", out);
    }).detach();
    return "VCV Rack is not installed \u2014 showing the file in Finder instead";
}

std::string ForgeModularShell::ask() {
    auto* c = chrome();
    if (!c) return "no editor is open";
    auto* input = c->prompt_input();
    const auto prompt = input ? input->text() : std::string{};
    if (prompt.find_first_not_of(" \t\n") == std::string::npos)
        return "type a question first";
    // Deliberately never generates: Ask must not rewrite the artifact, and the
    // difference between the two is carried, not inferred.
    c->enter_build();
    c->narrate(prompt.substr(0, 200));
    if (input) input->set_text("");

    // Answer from the patch itself. Derived from the file rather than a model,
    // so it costs nothing to re-ask and cannot claim a cable the patch does
    // not contain -- a confident answer about a connection that does not exist
    // would be worse than no answer.
    if (engine_ && !open_patch_.empty()) {
        auto answer = engine_->explain(open_patch_);
        if (!answer.empty()) {
            for (const auto& line : PatchExplanation::wrap(answer, 118))
                c->narrate(line);
            return {};
        }
    }
    if (open_patch_.empty())
        c->narrate("Build or open a patch first and I can explain it.");
    return {};
}

void ForgeModularShell::on_poll() {
    // Read what the generator has written FIRST. Evaluating the outcome before
    // pumping meant judging lines that had not been read yet, so every verdict
    // arrived a tick late -- and in a test that polls once, never.
    pump_build_log();

    // Keep the stage card in step with what the generator is actually doing.
    if (watching_) {
        const int stage = monitor_.stage();
        if (stage != reported_stage_) {
            reported_stage_ = stage;
            stage_started_ = std::chrono::steady_clock::now();
            if (auto* c = chrome()) c->set_active_stage(stage);
        }
        // A live clock on the active chip, and a cumulative one for the whole
        // run. A model call takes minutes; without something moving, "asking
        // the model" is indistinguishable from a wedged process, which is
        // exactly how it read.
        if (auto* c = chrome();
            c && monitor_.outcome() == BuildOutcome::running && stage >= 0) {
            c->set_active_stage_elapsed(format_elapsed(stage_started_));
            if (stage == 0)
                c->set_status_activity("asking the model \u00b7 " +
                                       format_elapsed(run_started_) + " elapsed");
        }
    }

    // Report the end of a run once. Left alone, the stage kept animating
    // "materializing…" under a transcript that had already printed
    // "gave up after 3 attempts" -- the screen contradicting itself.
    const auto outcome = monitor_.outcome();
    if (watching_ && outcome != BuildOutcome::running &&
        outcome != reported_outcome_) {
        reported_outcome_ = outcome;
        if (auto* c = chrome()) {
            // The verdict is the one line worth keeping, so it goes to both:
            // the stage for someone watching, the transcript for someone
            // coming back later.
            switch (outcome) {
                case BuildOutcome::done: {
                    c->set_status_activity({});
                    // Show what was built. A finished patch that leaves the
                    // materializing skeleton up has produced something the
                    // user cannot see, and the preview is the whole reason
                    // the stage exists.
                    const auto artifact = artifact_path();
                    const bool shown =
                        !artifact.empty() && open_patch_file(artifact).empty();
                    if (!shown) c->set_skeleton_caption("built");
                    // Only offer to open it if there is something to open.
                    c->narrate(artifact.empty()
                                   ? std::string("Built.")
                                   : std::string("Built. Open it in Rack to play it."));
                    break;
                }
                case BuildOutcome::refused:
                    c->set_skeleton_caption("stopped");
                    c->set_status_activity({});
                    break;   // the refusal itself already went to the chat
                case BuildOutcome::failed: {
                    c->set_skeleton_caption("build failed");
                    c->set_status_activity({});
                    // Say what stopped it, once, rather than the whole log.
                    auto why = monitor_.headline();
                    c->narrate(why.empty()
                                   ? std::string("The build failed and nothing "
                                                 "was installed.")
                                   : trimmed(why),
                               /*alarming=*/true);
                    break;
                }
                case BuildOutcome::running:
                    break;
            }
        }
    }

    // Open in Rack shows only when opening is actually safe.
    //
    //   * There must be an artifact. On Home, and before any generation, there
    //     is nothing to open and the button would be a lie.
    //   * A run must not be in flight. A generation rewrites the pack and the
    //     patch it named, so the path is stale the moment the next build
    //     starts -- opening it mid-rewrite gets a half-written file or the
    //     PREVIOUS module wearing the new one's name.
    //
    // It also lives in the Build title bar, so it cannot appear on Home at
    // all.
    if (open_button_) {
        const bool have = !artifact_path().empty();
        const bool settled = monitor_.outcome() != BuildOutcome::running;
        const bool ready = have && settled;
        if (ready != open_button_->visible()) {
            open_button_->set_visible(ready);
            open_button_->request_repaint();
        }
    }
}

bool ForgeModularShell::is_standalone() const {
    // Set by the standalone entry point; every plugin format leaves it false.
    return standalone_;
}

bool ForgeModularShell::busy() const {
    return watching_ && monitor_.outcome() == BuildOutcome::running;
}

void ForgeModularShell::watch_build_log(const std::string& path) {
    monitor_log_path_ = path;
    monitor_.watch(path);
    watching_ = true;
    run_started_ = std::chrono::steady_clock::now();
    stage_started_ = run_started_;
    // Clear the card. Its note outlived the run that wrote it, so a fresh
    // build opened showing the previous one's gate rejection.
    if (auto* c = chrome()) {
        c->set_status_note({});
        c->set_status_activity({});
        c->set_active_stage(-1);
    }
    reported_outcome_ = BuildOutcome::running;
    reported_stage_ = -2;
}

int ForgeModularShell::pump_build_log() {
    const auto added = monitor_.poll();
    auto* c = chrome();
    if (!c) return static_cast<int>(added.size());

    for (const auto& line : added) {
        switch (line.kind) {
            // A refusal is the one thing a person must not scroll past: the
            // run stopped, and the reason names something they can act on.
            // It earns a place in the transcript.
            case BuildLine::Kind::refusal:
                c->narrate(line.text, /*alarming=*/true);
                break;

            // Everything else belongs on the status card. A gate rejection is
            // the pipeline working; a retry is it recovering. Both were being
            // pushed into the chat as separate bubbles, so a build produced
            // dozens of low-value messages and buried whatever mattered. The
            // note line shows the latest and replaces itself.
            case BuildLine::Kind::gate:
                c->set_status_note(trimmed(line.text));
                break;
            case BuildLine::Kind::retry:
                c->set_status_activity(trimmed(line.text));
                break;
            case BuildLine::Kind::progress:
            case BuildLine::Kind::success:
                // The "open it with: <rack> <patch>" line is two long paths.
                // Trimmed to fit a two-line card it becomes an unusable stub
                // that cannot even be copied, and the Open in Rack button
                // already does the thing it describes. Skip it.
                if (line.text.find("open it with") == std::string::npos)
                    c->set_status_activity(trimmed(line.text));
                break;

            // A traceback is a defect in the toolchain rather than in what was
            // asked for. Its first line goes to the note so it is visible, and
            // the whole thing stays in the log for whoever debugs it.
            case BuildLine::Kind::error:
                c->set_status_note(trimmed(line.text));
                break;
        }
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
    // Not a failure. A Rack module is installed by the generator into Rack's
    // own plugin folder, which Forge's plugin-install path knows nothing
    // about. Saying "not wired yet" at the end of a build that succeeded reads
    // as the build having failed.
    err = "installed into VCV Rack, not as a Forge plugin \u2014 open it in Rack";
    return false;
}

}  // namespace forge_modular
