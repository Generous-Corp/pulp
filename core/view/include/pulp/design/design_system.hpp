#pragma once

// pulp::design — the "Ink & Signal" design system, ingested into Pulp code so
// the components a designer sees in the Figma library are directly usable when
// building a real UI (a sampler, an effect, an app). This is the umbrella
// include + the queryable component catalog.
//
// Two things live here:
//   1. The umbrella include — pull in <pulp/design/design_system.hpp> and you
//      have every native widget the design system covers, plus a one-call theme
//      helper (apply_ink_signal) and the token-true light/dark themes.
//   2. The component CATALOG — rich, queryable metadata bridging each Figma
//      component to its native C++ class: category, native class, source header,
//      Figma component-set name, a one-line usage note, and the theme tokens it
//      restyles through. The catalog is what lets tooling (design-import,
//      JS/React bindings, docs) reason about the system without hardcoding it.
//
// The reskin contract still holds: every cataloged widget paints through
// View::resolve_color, so applying a different theme restyles all of them with
// no code change (see docs/guides/design-tokens.md).

#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>

// Umbrella component surface — everything the catalog references.
#include <pulp/view/buttons.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/scroll_bar.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/view/table.hpp>
#include <pulp/view/toolbar.hpp>
#include <pulp/view/side_panel.hpp>
#include <pulp/view/breadcrumb.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/channel_strip_view.hpp>
#include <pulp/view/range_slider_view.hpp>
#include <pulp/view/inline_value_editor_view.hpp>
#include <pulp/view/property_panel_view.hpp>
#include <pulp/view/group_box_view.hpp>
#include <pulp/view/number_box_states_view.hpp>
#include <pulp/view/knob_modulation_view.hpp>
#include <pulp/view/waveform_recorder_view.hpp>
#include <pulp/view/text_editor.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

// ── Theme helpers ───────────────────────────────────────────────────────────

/// The "Ink & Signal" theme for the requested appearance, token-true to the
/// vendored bundle (assets/design-system/ink-signal/). Equivalent to
/// theme_from_preset(*find_preset("ink-signal"), dark) but without the caller
/// needing to know the preset id.
pulp::view::Theme ink_signal_theme(bool dark = true);

/// Apply the Ink & Signal theme to a view tree root. Descendants resolve their
/// colors up the parent chain, so one call restyles the whole subtree.
void apply_ink_signal(pulp::view::View& root, bool dark = true);

// ── Component catalog ─────────────────────────────────────────────────────────

/// The Figma file that hosts the authored library (designer-facing editor). The
/// in-repo tokens + native widgets are the buildable source of truth; this is
/// the cross-reference for round-tripping a reskin back to design.
inline constexpr std::string_view kFigmaFileKey = "q9iDYZzg86YrOQKr6I3bY0";

/// Coarse grouping, mirrored from the Figma Overview page's 7 sections.
enum class Category {
    controls,    // knobs, faders, sliders, steppers, pan
    inputs,      // text fields, combo boxes, toggles, checkboxes
    indicators,  // meters, progress, spinners, badges
    navigation,  // tabs, toolbars, breadcrumbs, sidebars, trees
    containers,  // panels, channel strips, scroll views, tables
    overlays,    // popovers, dialogs, tooltips, context menus, toasts
    audio,       // waveform, spectrum, XY pad, MIDI keyboard
    feedback,    // banners, empty states
};

std::string_view category_name(Category c);

/// One cataloged component: the bridge between a Figma component and its native
/// C++ widget, plus enough metadata for tooling and docs to use it well.
struct ComponentInfo {
    std::string name;             // Display + catalog key, matches Figma set name ("Knob")
    Category category;
    std::string native_class;     // Fully-qualified C++ class ("pulp::view::Knob")
    std::string header;           // Include needed to use it ("pulp/view/widgets.hpp")
    std::string figma_component;  // Figma component-set / page name in kFigmaFileKey
    std::string usage;            // One-line "what it's for"
    std::vector<std::string> reskin_tokens;  // Theme tokens it paints through
};

/// All cataloged components, in a stable display order.
const std::vector<ComponentInfo>& catalog();

/// Look up a component by its catalog name (case-sensitive, e.g. "Knob").
/// Returns nullptr if absent.
const ComponentInfo* find(std::string_view name);

/// All components in a category, in catalog order.
std::vector<const ComponentInfo*> in_category(Category c);

}  // namespace pulp::design
