// pulp::design implementation — Ink & Signal theme helpers + component catalog.
// See pulp/design/design_system.hpp for the contract.

#include <pulp/design/design_system.hpp>

#include <pulp/view/theme_presets.hpp>

#include <algorithm>

namespace pulp::design {

using pulp::view::Theme;

Theme ink_signal_theme(bool dark) {
    const auto* preset = pulp::view::find_preset("ink-signal");
    // The preset is built-in (theme_presets.cpp); fall back to a derived default
    // only in the impossible case it was stripped, so callers never crash.
    if (preset == nullptr) {
        const auto& all = pulp::view::all_presets();
        if (all.empty()) return Theme{};
        return pulp::view::theme_from_preset(all.front(), dark);
    }
    return pulp::view::theme_from_preset(*preset, dark);
}

void apply_ink_signal(pulp::view::View& root, bool dark) {
    root.set_theme(ink_signal_theme(dark));
}

std::string_view category_name(Category c) {
    switch (c) {
        case Category::controls:   return "Controls";
        case Category::inputs:     return "Inputs";
        case Category::indicators: return "Indicators";
        case Category::navigation: return "Navigation";
        case Category::containers: return "Containers";
        case Category::overlays:   return "Overlays";
        case Category::audio:      return "Audio";
        case Category::feedback:   return "Feedback";
    }
    return "Unknown";
}

namespace {

std::vector<ComponentInfo> build_catalog() {
    using C = Category;
    return {
        // ── Controls ───────────────────────────────────────────────────────
        {"Knob", C::controls, "pulp::view::Knob", "pulp/view/widgets.hpp", "Knob",
         "Rotary control for a continuous parameter",
         {"knob.arc", "knob.arc.bg", "knob.thumb", "focus.ring"}},
        {"Fader", C::controls, "pulp::view::Fader", "pulp/view/widgets.hpp", "Fader",
         "Vertical level control",
         {"slider.track", "slider.fill", "slider.thumb"}},
        {"Slider", C::controls, "pulp::view::RangeSlider", "pulp/view/widgets.hpp", "Slider",
         "Linear / range value control",
         {"slider.track", "slider.fill", "slider.thumb", "focus.ring"}},
        {"Stepper", C::controls, "pulp::view::Stepper", "pulp/view/gap_widgets.hpp", "Stepper",
         "[-] value [+] numeric nudge",
         {"bg.surface", "control.border", "text.primary", "accent.primary"}},
        {"Pan", C::controls, "pulp::view::PanControl", "pulp/view/gap_widgets.hpp", "Pan",
         "Bipolar L/R pan with center detent",
         {"slider.track", "slider.fill", "slider.thumb"}},

        // ── Inputs ─────────────────────────────────────────────────────────
        {"Button", C::inputs, "pulp::view::TextButton", "pulp/view/buttons.hpp", "Button",
         "Primary / secondary / ghost text button",
         {"accent.primary", "text.primary", "bg.surface", "control.border"}},
        {"Toggle", C::inputs, "pulp::view::Toggle", "pulp/view/widgets.hpp", "Toggle",
         "On/off switch",
         {"control.track", "accent.primary", "control.thumb"}},
        {"Checkbox", C::inputs, "pulp::view::Checkbox", "pulp/view/widgets.hpp", "Checkbox",
         "Tri-state checkbox",
         {"accent.primary", "control.border", "text.primary"}},
        {"Input", C::inputs, "pulp::view::TextEditor", "pulp/view/text_editor.hpp", "Input",
         "Single / multi-line text field",
         {"bg.surface", "control.border", "text.primary", "focus.ring", "accent.error"}},
        {"ComboBox", C::inputs, "pulp::view::ComboBox", "pulp/view/ui_components.hpp", "ComboBox",
         "Dropdown selector",
         {"bg.surface", "control.border", "text.primary", "accent.primary"}},

        // ── Indicators ───────────────────────────────────────────────────────
        {"Meter", C::indicators, "pulp::view::Meter", "pulp/view/widgets.hpp", "Meter",
         "Level meter with green/yellow/red zones",
         {"meter.green", "meter.yellow", "meter.red", "control.track"}},
        {"ProgressBar", C::indicators, "pulp::view::ProgressBar", "pulp/view/ui_components.hpp", "ProgressBar",
         "Determinate / indeterminate progress",
         {"progress.track", "progress.fill"}},
        {"Badge", C::indicators, "pulp::view::Badge", "pulp/view/gap_widgets.hpp", "Badge",
         "Compact status / count pill",
         {"bg.surface", "text.secondary", "accent.primary", "accent.error"}},

        // ── Navigation ───────────────────────────────────────────────────────
        {"Tab", C::navigation, "pulp::view::TabPanel", "pulp/view/ui_components.hpp", "Tab",
         "Tab bar with active underline",
         {"tab.active", "tab.inactive", "accent.primary"}},
        {"Toolbar", C::navigation, "pulp::view::Toolbar", "pulp/view/toolbar.hpp", "Toolbar",
         "Action button strip",
         {"bg.secondary", "text.primary", "divider"}},
        {"Breadcrumb", C::navigation, "pulp::view::Breadcrumb", "pulp/view/breadcrumb.hpp", "Breadcrumb",
         "Path navigation trail",
         {"text.secondary", "text.link", "divider"}},
        {"Sidebar", C::navigation, "pulp::view::SidePanel", "pulp/view/side_panel.hpp", "Sidebar",
         "Collapsible side panel",
         {"bg.secondary", "divider", "text.primary"}},
        {"Tree", C::navigation, "pulp::view::TreeView", "pulp/view/tree_view.hpp", "Tree",
         "Hierarchical disclosure list",
         {"text.primary", "text.secondary", "accent.primary"}},

        // ── Containers ───────────────────────────────────────────────────────
        {"Panel", C::containers, "pulp::view::Panel", "pulp/view/widgets.hpp", "Panel",
         "Titled grouping container",
         {"bg.elevated", "modal.border", "text.primary"}},
        {"ChannelStrip", C::containers, "pulp::view::ChannelStrip", "pulp/view/gap_widgets.hpp", "ChannelStrip",
         "Mixer strip: meter + fader + pan",
         {"bg.elevated", "meter.green", "slider.fill", "text.secondary"}},
        {"ScrollBar", C::containers, "pulp::view::ScrollBar", "pulp/view/scroll_bar.hpp", "ScrollBar",
         "Scroll indicator / drag handle",
         {"control.track", "control.thumb"}},
        {"Table", C::containers, "pulp::view::TableListBox", "pulp/view/table.hpp", "Table",
         "Row / column data grid",
         {"bg.surface", "divider", "text.primary"}},

        // ── Overlays ───────────────────────────────────────────────────────
        {"Popover", C::overlays, "pulp::view::Popover", "pulp/view/gap_widgets.hpp", "Popover",
         "Floating panel with a tail",
         {"modal.bg", "modal.border", "text.primary"}},
        {"Dialog", C::overlays, "pulp::view::InCanvasDialog", "pulp/view/gap_widgets.hpp", "Dialog",
         "In-canvas modal alert",
         {"overlay.bg", "modal.bg", "accent.primary", "accent.error"}},
        {"Tooltip", C::overlays, "pulp::view::Tooltip", "pulp/view/ui_components.hpp", "Tooltip",
         "Hover hint bubble",
         {"tooltip.bg", "tooltip.text"}},
        {"Toast", C::overlays, "pulp::view::Toast", "pulp/view/gap_widgets.hpp", "Toast",
         "Transient notification card",
         {"bg.elevated", "accent.primary", "text.primary"}},
        {"Callout", C::overlays, "pulp::view::CallOutBox", "pulp/view/ui_components.hpp", "Callout",
         "Pointered info box",
         {"modal.bg", "modal.border", "accent.primary"}},
        {"PopupMenu", C::overlays, "pulp::view::ContextMenu", "pulp/view/context_menu.hpp", "PopupMenu",
         "Right-click / dropdown menu",
         {"modal.bg", "text.primary", "accent.primary"}},

        // ── Audio ─────────────────────────────────────────────────────────
        {"WaveformEditor", C::audio, "pulp::view::WaveformView", "pulp/view/widgets.hpp", "WaveformEditor",
         "Sample waveform display / edit",
         {"waveform.line", "waveform.fill", "waveform.grid"}},
        {"Spectrum", C::audio, "pulp::view::SpectrumView", "pulp/view/widgets.hpp", "Spectrum",
         "Frequency spectrum analyser",
         {"waveform.line", "waveform.grid", "accent.primary"}},
        {"XYPad", C::audio, "pulp::view::XYPad", "pulp/view/widgets.hpp", "XYPad",
         "2-D parameter control surface",
         {"control.track", "accent.primary", "control.thumb"}},
        {"MIDIKeyboard", C::audio, "pulp::view::MidiKeyboard", "pulp/view/midi_keyboard.hpp", "MIDIKeyboard",
         "Plain MIDI piano input strip (note on/off only). For a playable "
         "computer-typing + piano keyboard with QWERTY play, octave and velocity, "
         "use MusicalTyping instead — NOT this.",
         {"bg.surface", "text.primary", "accent.primary"}},
        {"MusicalTyping", C::audio, "pulp::view::MusicalTypingKeyboard",
         "pulp/view/musical_typing_keyboard.hpp", "Musical Typing Keyboard",
         "THE playable musical-typing keyboard: computer-key letters "
         "(a w s e d f t g y h u j k o l p) play notes, z/x shift octave, plus a "
         "click-to-play piano row; pressed keys light with the accent gradient. "
         "Wire on_note_on/on_note_off. Use this for a \"musical typing keyboard\", "
         "NOT MIDIKeyboard (the plain strip).",
         {"surface.panel", "accent.primary", "accent.text", "key.white", "key.black", "text.secondary"}},
        {"Channel Strip", C::containers, "pulp::view::ChannelStripView",
         "pulp/view/channel_strip_view.hpp", "Channel Strip",
         "Pro channel strip: inserts, sends, routing, pan, dB-tick fader, VU "
         "(faithful Figma render; the lean interactive widget is \"ChannelStrip\")",
         {"surface.panel", "accent.primary", "text.primary"}},
        // Faithful Figma-vector specimens (DesignFrameView), generated by
        // tools/import-design/make_catalog_component.py. Each is the design's
        // composite/states specimen, complementing the lean interactive widgets.
        {"Range Slider", C::controls, "pulp::view::RangeSliderView",
         "pulp/view/range_slider_view.hpp", "Range Slider",
         "Dual-handle range slider — states, vertical, full/collapsed/disabled",
         {"surface.panel", "accent.primary", "text.primary"}},
        {"Inline Value Editor", C::controls, "pulp::view::InlineValueEditorView",
         "pulp/view/inline_value_editor_view.hpp", "Inline Value Editor",
         "Click-to-type readout — under-knob / beside-fader / editing / out-of-range",
         {"surface.panel", "accent.primary", "text.primary"}},
        {"Property Panel", C::containers, "pulp::view::PropertyPanelView",
         "pulp/view/property_panel_view.hpp", "Property Panel",
         "Labeled property rows — slider/choice/toggle/text/button + row states",
         {"surface.panel", "accent.primary", "text.primary"}},
        {"Group Box", C::containers, "pulp::view::GroupBoxView",
         "pulp/view/group_box_view.hpp", "Group Box",
         "Titled container — default / empty / collapsible",
         {"surface.panel", "accent.primary", "text.primary"}},
        {"Number Box", C::inputs, "pulp::view::NumberBoxStatesView",
         "pulp/view/number_box_states_view.hpp", "Number Box",
         "NumberBox states — scrub, chevrons, inline edit, stepping",
         {"surface.panel", "accent.primary", "text.primary"}},
        {"Knob Modulation", C::controls, "pulp::view::KnobModulationView",
         "pulp/view/knob_modulation_view.hpp", "Knob Modulation",
         "Saturn modulation rings — per-source depth, bipolar",
         {"surface.panel", "accent.primary", "text.primary"}},
        {"Waveform Recorder", C::audio, "pulp::view::WaveformRecorderView",
         "pulp/view/waveform_recorder_view.hpp", "Waveform Recorder",
         "Waveform editor + recorder — trim, fades, slice, zoom, record",
         {"surface.panel", "accent.primary", "text.primary"}},

        // ── Feedback ───────────────────────────────────────────────────────
        {"InlineBanner", C::feedback, "pulp::view::InlineBanner", "pulp/view/gap_widgets.hpp", "InlineBanner",
         "Full-width status message",
         {"bg.elevated", "accent.primary", "accent.error", "accent.success"}},
        {"EmptyState", C::feedback, "pulp::view::EmptyState", "pulp/view/gap_widgets.hpp", "EmptyState",
         "Placeholder with a call to action",
         {"bg.surface", "text.secondary", "accent.primary"}},
    };
}

}  // namespace

const std::vector<ComponentInfo>& catalog() {
    static const std::vector<ComponentInfo> kCatalog = build_catalog();
    return kCatalog;
}

const ComponentInfo* find(std::string_view name) {
    const auto& all = catalog();
    auto it = std::find_if(all.begin(), all.end(),
                           [&](const ComponentInfo& c) { return c.name == name; });
    return it == all.end() ? nullptr : &*it;
}

std::vector<const ComponentInfo*> in_category(Category c) {
    std::vector<const ComponentInfo*> out;
    for (const auto& info : catalog()) {
        if (info.category == c) out.push_back(&info);
    }
    return out;
}

}  // namespace pulp::design
