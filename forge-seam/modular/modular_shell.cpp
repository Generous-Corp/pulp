#include "forge/modular_shell.hpp"

#include "forge/module_catalog.hpp"
#include "forge/portmap.hpp"

#include "forge/module_summary.hpp"

#include "forge/project_store.hpp"

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

/// Where the emitter writes each module's panel artwork. Derived from
/// `tools_dir()` rather than spelled out again, so a developer pointing at a
/// checkout gets that checkout's panels and not the installed copy's.
std::string panels_dir() {
    return (std::filesystem::path(tools_dir()).parent_path().parent_path() /
            "examples" / "forge-modular" / "res").string();
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
    // The real launcher is installed HERE, on the way to being a plugin or an
    // app -- never in the shell's constructor. A shell built by a test has
    // none and therefore cannot open anything on anyone's screen.
    shell->set_launcher([](const std::string& command) {
        std::thread([command] {
            std::string out;
            ProcessEngine::run(command + " &", out);
        }).detach();
    });
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
    // The depth tabs belong to patches, and it is the TABS that have to be
    // revealed -- not the group around them.
    //
    // The group also holds Open in Rack, so it stays visible for a module
    // build. Toggling only the group therefore did nothing useful: the tabs
    // were created hidden when the accessory was built (artifact defaults to
    // module), and switching to Patch re-showed a group that was already
    // showing while every tab inside it stayed hidden. Terse/Standard/Learning
    // was reachable in code, covered by tests, and had never once appeared on
    // screen -- with Open in Rack sitting right beside it as proof the group
    // was fine.
    const bool patch_tabs = artifact_ == Artifact::patch;
    if (depth_group_) depth_group_->set_visible(true);
    for (auto* b : depth_tabs_)
        if (b) b->set_visible(patch_tabs);
    refresh_depth_tabs();
    // And the chat column: a module's spec beside a patch's wiring would be
    // two answers to one question.
    show_for_artifact();
}

forge::ChromeCopy ForgeModularShell::chrome_copy() const {
    const bool patch = artifact_ == Artifact::patch;

    // The badge carries the SIZE of what was built, not only its kind. Two
    // words is the cheapest place in the whole window to say how big a thing
    // this is, and every number here is already loaded -- nothing is counted
    // twice or typed in.
    std::string badge = patch ? "PATCH" : "MODULE";
    if (patch && rack_preview_ && !rack_preview_->modules().empty()) {
        const auto mods = rack_preview_->modules().size();
        const auto cables = rack_preview_->connections().size();
        badge += " \u00b7 " + std::to_string(mods) +
                 (mods == 1 ? " MODULE" : " MODULES");
        badge += " \u00b7 " + std::to_string(cables) +
                 (cables == 1 ? " CABLE" : " CABLES");
    } else if (!patch && module_summary_) {
        for (const auto& [key, value] : module_summary_->rows()) {
            if (key != "WIDTH") continue;
            // Just the HP, not the millimetres: the pill is a glance.
            const auto hp = value.substr(0, value.find(" \xC2\xB7"));
            if (!hp.empty()) badge += " \u00b7 " + hp;
            break;
        }
    }

    return {
        .badge = badge,
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
    // The placeholder has to promise the artifact actually being built. A
    // patch materializing as one module panel and then snapping to a rack of
    // ten shows the wrong thing for the whole build, with the header saying
    // PATCH beside it.
    if (auto* c = chrome()) {
        c->set_skeleton_shape(patch ? forge::ForgeChrome::SkeletonShape::rack
                                    : forge::ForgeChrome::SkeletonShape::module);
        // The heading names the artifact too, and it was chosen before this
        // switch: starting on Module and moving to Patch left "Untitled
        // module" beside a PATCH pill for the whole build.
        c->refresh_default_project_title();
    }
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
    // Where the "@" list gets its modules. Nothing set this, so the overlay
    // opened onto an empty list every time -- the feature existed and had
    // never listed anything.
    //
    // Installed modules first, because only those can be wired into a patch
    // that will sound. The rest of the library is offered anyway: being able
    // to name a module you do not own is how you find out it exists.
    mentions_.set_source([](const std::string& query) {
        return search_modules(query);
    });
    // Typing has to reach the list, or it never narrows. This was the whole
    // reason "@braids" appeared to do nothing: handle_text ran ONCE, when the
    // mention button was pressed, and every keystroke after it went only to
    // the text field. The list stayed on whatever the first call produced.
    if (auto* c = chrome()) {
        if (auto* input = c->prompt_input()) {
            input->on_change = [this](const std::string& text) {
                mentions_.handle_text(text, text.size());
            };
            // Arrows and Enter, on the FIELD.
            //
            // They were on root->on_global_key, which the window host calls on
            // ITS root -- and the shell's view is not that root: the standalone
            // wraps the editor, so the hook sat on a child and never fired. In
            // the app the list opened and up/down did nothing until you clicked
            // a row, after which they worked, because the row had focus and
            // handled its own keys. A headless test that called the hook
            // directly passed the whole time, because calling it is not the
            // same as it being reached.
            //
            // The field is the right place regardless: it has focus for as
            // long as somebody is typing a mention, which is exactly when the
            // list is open.
            // Arrows and Enter go on the view the WINDOW dispatches to, which
            // is not ours — see ensure_key_hook(). Installed lazily, because
            // the tree is still being built here and the walk would stop at a
            // partial root.
        }
    }
    // A pick that cannot be honoured says why, and what to do. "GET" alone
    // does not distinguish free-and-installable from paid, and a row that
    // ignores a click reads as broken rather than as refusing.
    // Named, not refused. The pick goes into the prompt either way; this only
    // says the module still has to be installed before a patch using it will
    // load, and it says it BESIDE THE COMPOSER — the status card this used to
    // write to belongs to the run card and does not exist while somebody is
    // typing, so the message reached nobody and the pick looked like it had
    // done nothing at all.
    mentions_.on_refused = [this](const MentionCandidate& what) {
        const std::string who = what.brand.empty()
                                    ? what.name
                                    : what.brand + " " + what.name;
        mentions_.show_notice(
            what.state == MentionCandidate::Availability::paid
                ? who + " is paid — buy it in Rack's Library, then rescan"
                : who + " is not installed yet — get it free in Rack's "
                        "Library, then rescan. The prompt can still name it.");
    };
    mentions_.on_choose = [this](const std::string& slug) {
        if (auto* c = chrome()) {
            if (auto* input = c->prompt_input()) {
                // Replace the token being typed, rather than appending: the user
                // has already typed "@vc" and expects it to become the module,
                // not to sit in front of it.
                std::string text = input->text();
                const auto at = text.rfind('@');
                if (at != std::string::npos) text.erase(at);
                // The '@' STAYS. The field is plain text -- it cannot bold or
                // highlight a range -- so the marker is the only thing that
                // distinguishes a module you picked from a word you typed.
                // It is also what makes the mention deletable as one unit,
                // because it is the only delimiter there is.
                input->set_text(text + "@" + slug + " ");
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

    // What Rack's state actually is, beside the button that depends on it.
    // The app has always known this and only ever said it as the wording of a
    // failure AFTER the button was pressed -- so "Rack is not installed" and
    // "the button did nothing" were the same experience up to that point.
    {
        auto pill = std::make_unique<pulp::view::Label>("");
        pill->set_font_family(forge::design::type::mono);
        pill->set_font_size(10.0f);
        pill->set_text_color(forge::design::color::text_faint);
        pill->set_vertical_align(pulp::canvas::TextVerticalAlign::center);
        pill->flex().preferred_height = 26;
        pill->flex().flex_grow = 0;
        pill->flex().flex_shrink = 0;
        pill->set_hit_testable(false);
        pill->set_visible(false);
        rack_pill_ = pill.get();
        group->add_child(std::move(pill));
    }
    return group;
}

std::unique_ptr<View> ForgeModularShell::stage_accessory() {
    // The stage is the RACK, and nothing else. The explanation used to sit
    // under it in the same pane, which shrank the rack to a stamp and buried
    // the text below it -- the prototype reads the two side by side, because
    // a cable's reason is read WHILE looking at the cable.
    auto preview = std::make_unique<RackPreview>();
    rack_preview_ = preview.get();
    // The pairing from the side a person is actually looking at: point at a
    // cable and the sentence explaining it lights. Wired HERE, on the view
    // that certainly exists, rather than beside the explanation's own hover
    // handler -- the two are built by different functions, and the rail runs
    // first, so a `if (rack_preview_)` guard over there silently wired
    // nothing at all and the feature shipped dead.
    //
    // `explanation_` is read when the hover happens, not now, so this does not
    // care which half is built first.
    //
    // The two do not chase each other: each setter returns early when handed
    // the index it already has.
    preview->on_cable_hover = [this](std::optional<std::size_t> index) {
        if (explanation_) explanation_->hover_line(index);
    };
    preview->set_panel_directory(panels_dir());
    preview->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    preview->flex().flex_grow = 1;
    preview->flex().min_height = 0;
    return preview;
}

/// Point the spec at the module that was most recently generated.
///
/// The manifest is the source for every row, so the newest one IS the module
/// just built. Reading the log for a name instead would be a second place that
/// decides which module we are looking at, and those two would drift.
void ForgeModularShell::refresh_module_summary() {
    if (!module_summary_) return;
    const auto dir = std::filesystem::path(panels_dir()).parent_path() /
                     "modules";
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type best{};
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        if (e.path().extension() != ".json" || name.rfind("_", 0) == 0) continue;
        const auto t = std::filesystem::last_write_time(e, ec);
        if (newest.empty() || t > best) { best = t; newest = e.path(); }
    }
    if (!newest.empty()) module_summary_->set_manifest(newest.string());
}

std::unique_ptr<View> ForgeModularShell::chat_accessory() {
    // BOTH live here, and the artifact decides which is visible. The accessory
    // is asked for once, when the chrome mounts, so returning one or the other
    // would freeze whichever was current at that moment -- and switching to
    // Patch afterwards would leave a module's spec on screen.
    auto column = std::make_unique<View>();
    column->flex().direction = pulp::view::FlexDirection::column;
    column->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    column->flex().flex_shrink = 0;

    auto summary = std::make_unique<ModuleSummary>();
    module_summary_ = summary.get();
    summary->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    summary->flex().flex_shrink = 0;
    column->add_child(std::move(summary));
    refresh_module_summary();

    auto explanation = std::make_unique<PatchExplanation>();
    explanation_ = explanation.get();
    explanation->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
    explanation->flex().flex_shrink = 0;
    // The pairing that makes this worth drawing: point at a sentence, the
    // cable it names lights and the rest recede. It survives the move to the
    // rail unchanged -- the two views were never coupled by their layout.
    explanation->on_hover = [this](std::optional<std::size_t> index) {
        if (rack_preview_) rack_preview_->set_highlight(index);
    };
    // The larger unit: point at a role's heading and every cable carrying it
    // lights at once, so the audio path is read as a shape through the rack
    // rather than as three sentences that happen to be adjacent. The preview
    // could already draw this; nothing had ever asked it to.
    explanation->on_role_hover = [this](std::optional<SignalRole> role) {
        if (rack_preview_) rack_preview_->highlight_role(role);
    };
    column->add_child(std::move(explanation));

    show_for_artifact();
    return column;
}

/// Show the half that belongs to the current artifact.
///
/// A module's spec beside a patch's wiring would be two answers to one
/// question, and the wrong one is the one nobody notices.
void ForgeModularShell::show_for_artifact() {
    const bool patch = artifact_ == Artifact::patch;
    if (module_summary_) module_summary_->set_visible(!patch);
    if (explanation_) explanation_->set_visible(patch);
    if (!patch) refresh_module_summary();
}

void ForgeModularShell::show_rack(std::vector<RackModule> modules,
                                  std::vector<Connection> connections) {
    if (!rack_preview_) return;
    const bool have_rack = !modules.empty();

    // Say what an UNMAPPED badge means and what clears it.
    //
    // The badge is honest — those modules are drawn without their controls
    // because nothing has measured them — but on its own it names a state and
    // no remedy, and the remedy is not guessable: the module has to be ON
    // SCREEN in Rack when SCAN runs, so scanning once leaves every module that
    // was not visible still badged. Somebody would reasonably read that as the
    // scan having failed.
    std::vector<std::string> unmapped;
    for (const auto& m : modules)
        if (!m.controls_measured && m.available)
            unmapped.push_back(m.name.empty() ? m.id : m.name);
    unmapped_note_.clear();
    if (!unmapped.empty()) {
        std::string names = unmapped[0];
        for (std::size_t i = 1; i < unmapped.size() && i < 3; ++i)
            names += ", " + unmapped[i];
        if (unmapped.size() > 3)
            names += " and " + std::to_string(unmapped.size() - 3) + " more";
        // Say WHICH of the two states this is. A map that exists and will not
        // parse produces exactly the same screen as one that was never
        // written — every vendor module bare, with an UNMAPPED badge — and
        // "scan again" is the wrong advice for the first: scanning writes the
        // same broken file. Seen for real: one auto-sizing widget wrote
        // `"w": inf`, which is not JSON, and took the whole map down.
        unmapped_note_ =
            PortMap::shared().unreadable()
                ? names + (unmapped.size() == 1 ? " is drawn" : " are drawn") +
                  " without controls because the port map on disk could not be "
                  "read — it is not valid JSON. Scanning again will not help "
                  "until it is replaced; delete "
                  "~/Library/Application Support/Rack2/forge-portmap.json and "
                  "scan once more."
                : names + (unmapped.size() == 1 ? " is drawn" : " are drawn") +
                  " without controls — nothing has measured them. Put "
                  + (unmapped.size() == 1 ? "it" : "them") +
                  " on screen in Rack and press SCAN on the MAP module.";
    }
    if (explanation_) {
        explanation_->set_request(last_request_);
        explanation_->set_connections(connections, modules);
        explanation_->set_depth(static_cast<ExplainDepth>(depth_));
    }
    rack_preview_->set_rack(std::move(modules), std::move(connections));
    // An empty rack keeps the skeleton up rather than showing a blank stage,
    // which would read as a finished build that produced nothing.
    if (auto* c = chrome()) {
        c->show_stage_accessory(have_rack);
        c->show_chat_accessory(have_rack);
        if (!unmapped_note_.empty()) c->set_status_note(unmapped_note_);
    }
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

void ForgeModularShell::save_project_for(const std::string& artifact) {
    // Put a generated patch in "My projects", so it can be found again.
    //
    // Nothing did this. The shell wrote a .vcv to the patches directory and
    // stopped, so a generation the user watched succeed became unreachable the
    // moment they left the screen -- worse than a missing feature, because the
    // work visibly existed a second ago. The module cards that WERE on the
    // shelf came from Forge's own store, not ours.
    //
    // The .vcv is COPIED INTO the project's own directory rather than
    // referenced from GeneratedSource. A .vcv is JSON, so it would physically
    // fit in `pulpgraph_json` -- and something downstream would eventually try
    // to build a signal graph out of it and fail a long way from here. A file
    // beside the entry needs no new field and cannot be mistaken for DSP.
    if (artifact.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(artifact, ec)) return;

    forge::GeneratedSource src;
    src.valid = true;
    // The title comes from the artifact's own stem, which the generator
    // already derived from the prompt ("design-a-complete-performance-...").
    // Read back as words so the shelf shows a name rather than a slug.
    std::string title = fs::path(artifact).stem().string();
    for (auto& ch : title) if (ch == '-' || ch == '_') ch = ' ';
    while (!title.empty() && (title.back() == ' ' || std::isdigit(
               static_cast<unsigned char>(title.back()))))
        title.pop_back();
    src.title = title.empty() ? std::string("Untitled patch") : title;
    src.chat_history = last_request_;

    forge::ProjectStore store;
    std::string err;
    const auto id = store.save(src, {}, err);
    if (id.empty()) {
        pulp::runtime::log_info("Forge Modular: could not save the project: {}",
                                err);
        return;
    }
    // Beside the entry, under a fixed name, so opening it is a lookup rather
    // than a guess.
    const auto dest = store.base() / id / "patch.vcv";
    fs::copy_file(artifact, dest, fs::copy_options::overwrite_existing, ec);
    if (ec)
        pulp::runtime::log_info("Forge Modular: saved the project but not its "
                                "patch: {}", ec.message());
    else
        pulp::runtime::log_info("Forge Modular: saved project {}", id);
}

bool ForgeModularShell::open_project_entry(const std::string& id,
                                           std::string& err) {
    // Ours carry a Rack patch beside the entry. The base would try to bake and
    // install a signal graph that is not there, so it is not consulted.
    namespace fs = std::filesystem;
    forge::ProjectStore store;
    const auto patch = store.base() / id / "patch.vcv";
    std::error_code ec;
    if (!fs::exists(patch, ec)) {
        // Not one of ours: fall back, so a project made by another Forge shell
        // still opens if it ever lands in this store.
        return forge::ForgeShell::open_project_entry(id, err);
    }
    err = open_patch_file(patch.string());
    return err.empty();
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

    // One generation at a time.
    //
    // Nothing stopped a second Build. Two runs then wrote over each other's
    // log — the one file the app reads for the outcome, the stage and the
    // artifact path — and the result was an explanation describing one patch
    // beside a filename naming another, with Rack opening whichever the log
    // happened to mention first. Reported as "it opened an existing one" and
    // "Rack showed a different patch name".
    //
    // `busy()` alone is not enough: a generation is launched with nohup +
    // setsid so it survives the window closing, so after a restart the shell
    // believes nothing is running while a generator is still writing. The
    // engine is asked about the actual process too.
    // Two questions, because neither answers alone.
    //
    // The engine knows about a generator process whoever started it, including
    // one left over from a previous launch of the app — a generation survives
    // the window closing by design. But there is a moment after submit before
    // the process exists, so a fast second press would slip through.
    //
    // `in_flight_` covers that moment: a build this shell started, on a log it
    // is watching, that has not reported an end. All three parts matter —
    // `busy()` alone reads `running` for a shell that is merely watching a log
    // nothing has written, which refused the FIRST build of every session.
    const bool ours = in_flight_ && watching_ &&
                      monitor_.outcome() == BuildOutcome::running;
    if (ours || engine_->generator_running())
        return "a patch is already building — let it finish first";

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
    if (c->mode() == forge::ForgeChrome::Mode::Home)
        c->begin_new_session();

    // The patch that was open is not the patch being built, whichever screen
    // the request came from. artifact_path() falls back to `open_patch_` when
    // this session's log holds no patch line -- right for a project reopened
    // from the shelf with no build behind it, wrong from the instant a new
    // build starts. Clearing it only on the Home path left a build begun from
    // the Build screen -- where a user already is after one build, and where
    // opening a project puts them -- offering the PREVIOUS patch to Rack for
    // the whole run, and forever if the run failed. Reported as "I pick a
    // prompt I have built before and it shows me the prebuilt one".
    open_patch_.clear();
    // And the rack drawn from it goes with it. The drawing has no reason of its
    // own to change when a different prompt is submitted, so the previous
    // patch stayed on screen for the whole of the next build -- the half of
    // that report a user sees without pressing anything.
    show_rack({}, {});

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
    // Kept so the explanation can show what was asked for, above what was
    // built. The chat rail has it too, but that is a different panel and is
    // usually scrolled away by the time the rack is on screen.
    last_request_ = prompt;
    if (explanation_) explanation_->set_request(last_request_);
    if (input) input->set_text("");
    engine_->submit(prompt, artifact_ == Artifact::patch);
    in_flight_ = true;
    // Follow the file this run actually chose. The engine gives every run its
    // own, so a shell still tailing the previous one would report the previous
    // run's outcome for the whole of this one.
    if (const auto chosen = engine_->log_path();
        !chosen.empty() && chosen != monitor_log_path_) {
        monitor_log_path_ = chosen;
        monitor_.watch(chosen);
        watching_ = true;
    }
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

std::string RackPresence::phrase() const {
    // Say something only when there is a PROBLEM.
    //
    // This reported the good news too -- "Rack is installed", greyed out beside
    // the Open in Rack button -- which is a label that is present exactly when
    // it is not needed and says nothing the button next to it does not already
    // imply. The negative is the only case worth a person's attention, because
    // it is the only one they have to act on.
    if (standalone_running || standalone_installed) return {};
    if (plugin_installed) return "Rack is available as a plugin";
    return "Rack is not installed";
}

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

namespace {

/// A path as one shell word, whatever is in it. Application Support has a
/// space in it and a patch is named after the prompt, so both ends of every
/// command here are user-shaped text.
std::string quoted(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

}  // namespace

std::string rack_open_command(const std::string& app, const std::string& patch,
                              bool already_running) {
    if (already_running) {
        // A live application takes a document; --args would be ignored, and
        // launching a second Rack would put two of them on one audio device.
        return "open -a " + quoted(app) + " " + quoted(patch);
    }
    // Rack's own positional argument. Without it Rack starts by restoring its
    // autosave, and the patch just built arrives -- if at all -- behind
    // whatever was open last time.
    return "open -a " + quoted(app) + " --args " + quoted(patch);
}

void ForgeModularShell::run_detached(const std::string& command) {
    // Recorded either way, so a test can assert WHICH command was chosen.
    launched_.push_back(command);
    if (!launcher_) return;   // nobody wired one: decide, record, launch nothing
    launcher_(command);
}

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

    // Rack BLOCKS on a modal error dialog -- even started headless -- when a
    // patch names a module its installed plugin does not contain. Not a
    // warning, not a gap in the rack: the application comes up, puts a dialog
    // nobody asked for in front of an empty rack, and waits. From inside a DAW
    // that dialog can arrive BEHIND the host, so the visible result of pressing
    // this button is Rack opening with nothing in it and never finishing.
    //
    // The preview already draws these modules struck out, because it reads our
    // manifests while Rack can only create what its installed plugin BINARY
    // contains, and on a machine running an older build the two differ. Drawing
    // the warning and then handing Rack the patch anyway is having the
    // information and using it for nothing.
    //
    // Checked against the file being handed over rather than against whatever
    // the preview happens to be showing, because those are two different
    // claims and only one of them is about to be opened.
    {
        std::vector<std::string> missing;
        for (const auto& m : load_patch(path).modules) {
            if (m.available) continue;
            const auto& shown = m.display.empty() ? m.name : m.display;
            if (std::find(missing.begin(), missing.end(), shown) == missing.end())
                missing.push_back(shown);
        }
        if (!missing.empty()) {
            std::string names;
            for (std::size_t i = 0; i < missing.size() && i < 4; ++i)
                names += (i ? ", " : "") + missing[i];
            if (missing.size() > 4)
                names += " and " + std::to_string(missing.size() - 4) + " more";
            return "the VCV Rack on this machine has no " + names +
                   " \u2014 it would open on an error dialog with an empty "
                   "rack. Install the current Forge Modular plugin first.";
        }
    }

    const auto rack = look_for_rack();

    auto launch = [this, path, &rack] {
        run_detached(rack_open_command("/Applications/VCV Rack 2 Free.app",
                                       path, rack.standalone_running));
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
    run_detached("open -R '" + path + "'");
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

    // Point at the picture, not only at the words. A question that names a
    // module lights that module's cable while the answer is read; an answer
    // naming a connection the reader then has to hunt for in a rack of ten
    // modules teaches less than the same words beside a glowing cable.
    //
    // Nothing is lit when the question names no module in this patch --
    // pointing at an arbitrary cable would be worse than pointing at none.
    if (rack_preview_) {
        const auto lit = cable_for_question(prompt, rack_preview_->connections(),
                                            rack_preview_->modules());
        rack_preview_->set_highlight(lit);
        if (explanation_) explanation_->hover_line(lit);
    }

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
    // A way to ask for a build from OUTSIDE the window, for proving that a
    // generation works when it is spawned from inside a host.
    //
    // That claim is the one no headless test can make: the generator is
    // spawned BY the plugin, inheriting the host's environment, and a plugin
    // whose editor draws perfectly can still never reach it -- which is
    // exactly what happened when the module builder could not find `claude`
    // with no PATH to search. Proving it previously meant driving synthetic
    // clicks at screen coordinates, and that typed a prompt into somebody's
    // terminal twice: every guess about what is on screen finds a new way to
    // be wrong, and the cost lands on whoever is using the machine.
    //
    // So the host-side proof asks through a file instead. Inert unless the
    // environment names one -- nothing in a shipped plugin reads this.
    if (const char* trigger = std::getenv("FORGE_MODULAR_TEST_PROMPT");
        trigger && *trigger) {
        std::error_code ec;
        if (std::filesystem::exists(trigger, ec)) {
            std::string prompt;
            {
                std::ifstream f(trigger);
                std::getline(f, prompt);
            }
            // Removed BEFORE submitting, so a build that takes minutes cannot
            // be started again on the next tick.
            std::filesystem::remove(trigger, ec);
            // "patch: …" or "module: …" chooses WHICH artifact, because the
            // shell defaults to whichever it was last on -- a request for a
            // patch built a module, and the run failed for asking the wrong
            // question rather than for anything being broken.
            if (prompt.rfind("patch:", 0) == 0) {
                set_artifact(Artifact::patch);
                prompt = prompt.substr(6);
            } else if (prompt.rfind("module:", 0) == 0) {
                set_artifact(Artifact::module);
                prompt = prompt.substr(7);
            }
            while (!prompt.empty() && prompt.front() == ' ')
                prompt.erase(prompt.begin());
            if (!prompt.empty()) {
                pulp::runtime::log_info(
                    "Forge Modular: build requested through the test seam");
                submit_own(prompt);
            }
        }
    }

    // A re-wrap the layout asked for and could not schedule. A hosted plugin
    // has no dispatcher, so without this the explanation stays laid out for
    // the width it was first built at and its text runs over what is below.
    if (explanation_) explanation_->apply_pending_rewrap();

    // Read what the generator has written FIRST. Evaluating the outcome before
    // pumping meant judging lines that had not been read yet, so every verdict
    // arrived a tick late -- and in a test that polls once, never.
    // Only when there is a log to read. This ran on EVERY poll, which the
    // display link drives at up to 120Hz, so an idle app with no build in
    // sight was opening and reading a file a hundred times a second. Nothing
    // downstream uses the result unless `watching_` is set, so the read was
    // pure cost.
    if (watching_) pump_build_log();

    // Keep the stage card in step with what the generator is actually doing.
    if (watching_) {
        // A run that has printed NOTHING is still a run.
        //
        // stage() reads the generator's log, and starts at -1 until a line
        // matches. Everything below used to be gated on `stage >= 0`, so a
        // run whose output had not reached the file yet drove no chip, no
        // clock and no status: five identical grey rows, for as long as the
        // silence lasted. Observed at seven minutes, with a healthy model
        // call in flight and a 0-byte log -- and it is indistinguishable, on
        // screen, from a generator that died before its first line.
        //
        // The log is the GENERATOR's report. That we submitted is the APP's
        // own knowledge, and it does not need confirming by the thing it
        // launched. So a silent run shows the first stage and counts from
        // submission; the log refines that as soon as it says anything.
        const int reported = monitor_.stage();
        // Silence only counts as "stage 0" for a run THIS shell started and is
        // still waiting on. `watching_` alone is true whenever a log is being
        // tailed, including at startup against a finished run's file -- and
        // treating that as a live stage 0 logged a phantom Thinking before any
        // build was pressed, and left reported_stage_ already sitting at 0 so
        // the real build's transition never fired. A card that lights for
        // nothing is the same defect as one that stays dark for something.
        const bool silent = reported < 0 && in_flight_;
        const int stage = silent ? 0 : reported;
        if (stage != reported_stage_) {
            reported_stage_ = stage;
            stage_started_ = std::chrono::steady_clock::now();
            if (auto* c = chrome()) c->set_active_stage(stage);
            // Written down, because "the card is grey" is a symptom with at
            // least four causes -- not watching, no chrome, the monitor
            // reporting nothing, or the outcome already terminal -- and
            // telling them apart from a screenshot is impossible. Each one
            // cost a round trip through somebody else's machine.
            pulp::runtime::log_info(
                "Forge Modular: stage {} (log reported {}, chrome {}, "
                "outcome {})",
                stage, reported,
                chrome() ? "attached" : "MISSING",
                monitor_.outcome() == BuildOutcome::running ? "running"
                                                            : "terminal");
        }
        // A live clock on the active chip, and a cumulative one for the whole
        // run. A model call takes minutes; without something moving, "asking
        // the model" is indistinguishable from a wedged process, which is
        // exactly how it read.
        // in_flight_, not just `watching_ && running`: a clock that ticks when
        // nothing is building is not merely wrong on screen, it is expensive.
        // Rewriting the elapsed label every poll marks the view dirty every
        // poll, and the host's repaint gate honours that -- so an idle app
        // repainted the whole scene at vsync forever, for a number nobody was
        // waiting on. `watching_` is true whenever a log is being tailed,
        // including at rest against a finished run's file, and an empty log
        // reads as `running`, so the pair of them is true at idle.
        if (auto* c = chrome();
            c && in_flight_ && monitor_.outcome() == BuildOutcome::running) {
            c->set_active_stage_elapsed(format_elapsed(stage_started_));
            if (stage == 0) {
                std::string note = "asking the model \u00b7 " +
                                   format_elapsed(run_started_) + " elapsed";
                // Say the quiet part once it is worth saying. A first line
                // normally lands in seconds, so a minute of nothing is a fact
                // about the run, and the person watching should not have to
                // read a log to learn it.
                if (silent &&
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - run_started_).count() > 60)
                    note += " \u00b7 no output yet";
                c->set_status_activity(note);
            }
        }
    }

    // Report the end of a run once. Left alone, the stage kept animating
    // "materializing…" under a transcript that had already printed
    // "gave up after 3 attempts" -- the screen contradicting itself.
    const auto outcome = monitor_.outcome();
    if (watching_ && outcome != BuildOutcome::running)
        in_flight_ = false;
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
                    save_project_for(artifact);
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
        // Settled means "no build is running", which is not the same as "the
        // log does not say running". An empty log has no success line in it,
        // so outcome() reads `running` for a shell that has never watched
        // anything -- and a project reopened from the shelf is exactly that.
        // artifact_path() goes out of its way to return the patch on disk for
        // that case, and this hid the button and the pill anyway, so the
        // intent was stated in one function and undone in the next.
        const bool settled = !busy();
        const bool ready = have && settled;
        if (ready != open_button_->visible()) {
            open_button_->set_visible(ready);
            open_button_->request_repaint();
            // Probe the moment the button appears, so the pill is right the
            // first time it is read rather than up to a throttle late.
            if (ready) rack_probed_ = {};
        }
        if (ready) refresh_rack_presence();
        ensure_key_hook();
        if (rack_pill_ && rack_pill_->visible() != ready) {
            rack_pill_->set_visible(ready);
            rack_pill_->request_repaint();
        }
    }
}

bool ForgeModularShell::handle_prompt_key(const pulp::view::KeyEvent& event) {
    // Backspace removes a whole mention, not one letter of it.
    //
    // A picked module is "@VCO" -- a single thing the user chose, not five
    // characters they typed -- and rubbing it out a letter at a time leaves
    // "@VC", which is a half-name the generator would take literally. The
    // field is plain text, so this is what "delete the whole thing and mention
    // it again" looks like.
    if (!event.is_down || event.key != pulp::view::KeyCode::backspace)
        return false;
    if (mentions_.is_open()) return false;   // typing narrows; leave it alone
    auto* c = chrome();
    auto* input = c ? c->prompt_input() : nullptr;
    if (!input) return false;

    std::string text = input->text();
    // Only at the very end, because that is the only place the caret position
    // is known here. Editing mid-prompt behaves normally rather than
    // surprisingly.
    while (!text.empty() && text.back() == ' ') text.pop_back();
    const auto at = text.rfind('@');
    if (at == std::string::npos) return false;
    // The marker must start the token: "a@b" is not a mention.
    if (at > 0 && text[at - 1] != ' ') return false;
    if (text.find(' ', at) != std::string::npos) return false;   // not the last

    text.erase(at);
    input->set_text(text);
    return true;
}

std::unique_ptr<pulp::view::View> ForgeModularShell::create_view() {
    auto root = forge::ForgeShell::create_view();
    if (root) {
        // Arrows and Enter belong to the LIST while it is open. The hook has
        // to be on the ROOT to run before normal dispatch; on the overlay's
        // own view it never fired at all, and the text field kept the arrows
        // for its caret.
        // Chained, never replaced: the chrome installs Escape-closes-modals on
        // this same hook inside the base create_view(), and assigning over it
        // took that away silently.
        auto prior = std::move(root->on_global_key);
        root->on_global_key = [this, prior = std::move(prior)](
                                  const pulp::view::KeyEvent& e) {
            if (mentions_.handle_key_event(e)) return true;
            if (handle_prompt_key(e)) return true;
            return prior ? prior(e) : false;
        };
    }
    return root;
}

void ForgeModularShell::on_view_closed(pulp::view::View& view) {
    // Every pointer below is BORROWED from the editor's view tree, and this
    // shell outlives that tree -- a host closes and reopens a plugin window
    // freely while the processor stays loaded. Kept across the close they name
    // freed memory, and the next thing the shell does with them is not a read:
    // a build that finishes on the following poll runs `show_rack`, whose
    // `set_connections` move-assigns over the destroyed view's `connections_`
    // and hands libmalloc a pointer that was never allocated.
    //
    // Cleared BEFORE the base runs. The base destroys the chrome, and the
    // chrome OWNS some of these views rather than merely containing them --
    // `clear_chat_rail` moves the chat accessory into its retired list -- so
    // they are freed during that call, not with the tree afterwards.
    //
    // Every use site already guards on null, so forgetting them turns the
    // whole window between one editor and the next into a no-op. The next
    // `create_view` supplies fresh ones through the accessory hooks.
    rack_preview_ = nullptr;
    explanation_ = nullptr;
    module_summary_ = nullptr;
    depth_tabs_.clear();
    depth_labels_.clear();
    depth_group_ = nullptr;
    open_button_ = nullptr;
    rack_pill_ = nullptr;
    tab_labels_.clear();
    tab_module_ = nullptr;
    tab_patch_ = nullptr;
    mentions_.forget_views();
    forge::ForgeShell::on_view_closed(view);
}

void ForgeModularShell::ensure_key_hook() {
    // The window host calls `rootView->on_global_key` on ITS root. For the
    // standalone that root is an outer chrome wrapping the editor, so a hook
    // on the shell's own view is never reached — which is why the mention
    // list ignored the arrows until a row had been clicked.
    //
    // Done once, from the poll, because the tree is not attached when the
    // composer is built and the walk would stop short.
    auto* c = chrome();
    if (!c) return;
    auto* input = c->prompt_input();
    if (!input || !input->parent()) return;
    pulp::view::View* top = input;
    while (top->parent()) top = top->parent();

    // Re-checked every poll, NOT latched on a bool.
    //
    // The window host dispatches to whatever view was handed to
    // WindowHost::create — for the standalone that is the chrome's
    // window_root, which WRAPS the editor. The editor's tree is built first
    // and inserted into it after, so a hook installed at the first poll where
    // the field has a parent lands on the INNER root and stays there: the
    // window never calls it and the arrows do nothing, which is exactly what
    // was reported after the first attempt at this.
    //
    // Comparing the view we installed on costs a pointer and survives the tree
    // being re-parented at any time.
    // The field first, the root second.
    //
    // AppKit offers every key-down to performKeyEquivalent: before keyDown:,
    // and that path asks the FOCUSED view before the root's global handler.
    // The composer is multi-line, so it claims Up and Down for line movement
    // and returns consumed -- the root hook was never reached for an arrow
    // while somebody was typing, which is the only time the mention list is
    // up. Measured, not reasoned: with the list open and 35 candidates, every
    // arrow arrived at the root hook with is_down=false only, i.e. as the
    // key-UP after a key-down the field had already eaten. It is also why
    // clicking a row made the arrows start working -- that moves focus off the
    // field.
    //
    // The root hook stays: it is what carries the keys when focus is anywhere
    // else, and dropping it would trade one half of the problem for the other.
    if (auto* c2 = chrome())
        c2->set_prompt_key_filter([this](const pulp::view::KeyEvent& e) {
            return mentions_.handle_key_event(e);
        });

    if (top == key_hook_root_) return;
    key_hook_root_ = top;
    auto prior = std::move(top->on_global_key);
    top->on_global_key = [this, prior = std::move(prior)](
                             const pulp::view::KeyEvent& e) {
        if (mentions_.handle_key_event(e)) return true;
        if (handle_prompt_key(e)) return true;
        return prior ? prior(e) : false;
    };
}

void ForgeModularShell::refresh_rack_presence() {
    // `pgrep` is a process spawn, and the poll runs several times a second for
    // a state that changes when somebody launches an application.
    const auto now = std::chrono::steady_clock::now();
    if (rack_probed_ != std::chrono::steady_clock::time_point{} &&
        now - rack_probed_ < std::chrono::seconds(2))
        return;
    rack_probed_ = now;
    auto phrase = look_for_rack().phrase();
    if (phrase == rack_phrase_) return;
    rack_phrase_ = std::move(phrase);
    if (rack_pill_) {
        rack_pill_->set_text(rack_phrase_);
        rack_pill_->request_repaint();
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
