// design-system.ts — the "Ink & Signal" component catalog for @pulp/react.
// This mirrors the native C++ catalog in `pulp/design/design_system.hpp`:
// rich, queryable metadata bridging each Figma component to a JS-usable widget.
// JS authors and tooling use it to discover what's available, which JSX
// intrinsic renders it, and which theme tokens a reskin touches without
// hardcoding the list.
//
// `jsxTag` is the @pulp/react intrinsic name when one exists (import it from
// '@pulp/react'); `null` means the component is native-only today (no JSX
// intrinsic yet — see the native catalog / import vocabulary).

export type DesignCategory =
    | 'controls'
    | 'inputs'
    | 'indicators'
    | 'navigation'
    | 'containers'
    | 'overlays'
    | 'audio'
    | 'feedback';

export interface DesignComponent {
    /** Display + catalog key, matches the Figma component-set name. */
    readonly name: string;
    readonly category: DesignCategory;
    /** @pulp/react intrinsic to render it, or null if native-only today. */
    readonly jsxTag: string | null;
    /** Fully-qualified native C++ class. */
    readonly nativeClass: string;
    /** One-line "what it's for". */
    readonly usage: string;
    /** Theme tokens this component paints through (a reskin retints these). */
    readonly reskinTokens: readonly string[];
}

/** The Figma file hosting the authored library (designer-facing editor). */
export const FIGMA_FILE_KEY = 'q9iDYZzg86YrOQKr6I3bY0';

/**
 * The Ink & Signal component catalog. Kept in lockstep with the native catalog
 * (`pulp/design/design_system.hpp`) — the C++ catalog is the source of truth;
 * this is its JS-facing mirror plus the JSX-intrinsic column.
 */
export const inkSignalCatalog: readonly DesignComponent[] = [
    // controls
    { name: 'Knob', category: 'controls', jsxTag: 'Knob', nativeClass: 'pulp::view::Knob', usage: 'Rotary control for a continuous parameter', reskinTokens: ['knob.arc', 'knob.arc.bg', 'knob.thumb', 'focus.ring'] },
    { name: 'Fader', category: 'controls', jsxTag: 'Fader', nativeClass: 'pulp::view::Fader', usage: 'Vertical level control', reskinTokens: ['slider.track', 'slider.fill', 'slider.thumb'] },
    { name: 'Slider', category: 'controls', jsxTag: null, nativeClass: 'pulp::view::RangeSlider', usage: 'Linear / range value control', reskinTokens: ['slider.track', 'slider.fill', 'slider.thumb', 'focus.ring'] },
    { name: 'Stepper', category: 'controls', jsxTag: 'Stepper', nativeClass: 'pulp::view::Stepper', usage: '[-] value [+] numeric nudge', reskinTokens: ['bg.surface', 'control.border', 'text.primary', 'accent.primary'] },
    { name: 'Pan', category: 'controls', jsxTag: 'Pan', nativeClass: 'pulp::view::PanControl', usage: 'Bipolar L/R pan with center detent', reskinTokens: ['slider.track', 'slider.fill', 'slider.thumb'] },
    // inputs
    { name: 'Button', category: 'inputs', jsxTag: 'Button', nativeClass: 'pulp::view::TextButton', usage: 'Primary / secondary / ghost text button', reskinTokens: ['accent.primary', 'text.primary', 'bg.surface', 'control.border'] },
    { name: 'Toggle', category: 'inputs', jsxTag: 'Toggle', nativeClass: 'pulp::view::Toggle', usage: 'On/off switch', reskinTokens: ['control.track', 'accent.primary', 'control.thumb'] },
    { name: 'Checkbox', category: 'inputs', jsxTag: 'Checkbox', nativeClass: 'pulp::view::Checkbox', usage: 'Tri-state checkbox', reskinTokens: ['accent.primary', 'control.border', 'text.primary'] },
    { name: 'Input', category: 'inputs', jsxTag: 'TextEditor', nativeClass: 'pulp::view::TextEditor', usage: 'Single / multi-line text field', reskinTokens: ['bg.surface', 'control.border', 'text.primary', 'focus.ring', 'accent.error'] },
    { name: 'ComboBox', category: 'inputs', jsxTag: 'Combo', nativeClass: 'pulp::view::ComboBox', usage: 'Dropdown selector', reskinTokens: ['bg.surface', 'control.border', 'text.primary', 'accent.primary'] },
    // indicators
    { name: 'Meter', category: 'indicators', jsxTag: 'Meter', nativeClass: 'pulp::view::Meter', usage: 'Level meter with green/yellow/red zones', reskinTokens: ['meter.green', 'meter.yellow', 'meter.red', 'control.track'] },
    { name: 'ProgressBar', category: 'indicators', jsxTag: 'Progress', nativeClass: 'pulp::view::ProgressBar', usage: 'Determinate / indeterminate progress', reskinTokens: ['progress.track', 'progress.fill'] },
    { name: 'Badge', category: 'indicators', jsxTag: 'Badge', nativeClass: 'pulp::view::Badge', usage: 'Compact status / count pill', reskinTokens: ['bg.surface', 'text.secondary', 'accent.primary', 'accent.error'] },
    // navigation
    { name: 'Tab', category: 'navigation', jsxTag: null, nativeClass: 'pulp::view::TabPanel', usage: 'Tab bar with active underline', reskinTokens: ['tab.active', 'tab.inactive', 'accent.primary'] },
    { name: 'Toolbar', category: 'navigation', jsxTag: null, nativeClass: 'pulp::view::Toolbar', usage: 'Action button strip', reskinTokens: ['bg.secondary', 'text.primary', 'divider'] },
    { name: 'Breadcrumb', category: 'navigation', jsxTag: null, nativeClass: 'pulp::view::Breadcrumb', usage: 'Path navigation trail', reskinTokens: ['text.secondary', 'text.link', 'divider'] },
    { name: 'Sidebar', category: 'navigation', jsxTag: null, nativeClass: 'pulp::view::SidePanel', usage: 'Collapsible side panel', reskinTokens: ['bg.secondary', 'divider', 'text.primary'] },
    { name: 'Tree', category: 'navigation', jsxTag: null, nativeClass: 'pulp::view::TreeView', usage: 'Hierarchical disclosure list', reskinTokens: ['text.primary', 'text.secondary', 'accent.primary'] },
    // containers
    { name: 'Panel', category: 'containers', jsxTag: 'Panel', nativeClass: 'pulp::view::Panel', usage: 'Titled grouping container', reskinTokens: ['bg.elevated', 'modal.border', 'text.primary'] },
    { name: 'ChannelStrip', category: 'containers', jsxTag: null, nativeClass: 'pulp::view::ChannelStrip', usage: 'Mixer strip: meter + fader + pan', reskinTokens: ['bg.elevated', 'meter.green', 'slider.fill', 'text.secondary'] },
    { name: 'ScrollBar', category: 'containers', jsxTag: null, nativeClass: 'pulp::view::ScrollBar', usage: 'Scroll indicator / drag handle', reskinTokens: ['control.track', 'control.thumb'] },
    { name: 'Table', category: 'containers', jsxTag: null, nativeClass: 'pulp::view::TableListBox', usage: 'Row / column data grid', reskinTokens: ['bg.surface', 'divider', 'text.primary'] },
    // overlays
    { name: 'Popover', category: 'overlays', jsxTag: null, nativeClass: 'pulp::view::Popover', usage: 'Floating panel with a tail', reskinTokens: ['modal.bg', 'modal.border', 'text.primary'] },
    { name: 'Dialog', category: 'overlays', jsxTag: null, nativeClass: 'pulp::view::InCanvasDialog', usage: 'In-canvas modal alert', reskinTokens: ['overlay.bg', 'modal.bg', 'accent.primary', 'accent.error'] },
    { name: 'Tooltip', category: 'overlays', jsxTag: null, nativeClass: 'pulp::view::Tooltip', usage: 'Hover hint bubble', reskinTokens: ['tooltip.bg', 'tooltip.text'] },
    { name: 'Toast', category: 'overlays', jsxTag: null, nativeClass: 'pulp::view::Toast', usage: 'Transient notification card', reskinTokens: ['bg.elevated', 'accent.primary', 'text.primary'] },
    { name: 'Callout', category: 'overlays', jsxTag: null, nativeClass: 'pulp::view::CallOutBox', usage: 'Pointered info box', reskinTokens: ['modal.bg', 'modal.border', 'accent.primary'] },
    { name: 'PopupMenu', category: 'overlays', jsxTag: null, nativeClass: 'pulp::view::ContextMenu', usage: 'Right-click / dropdown menu', reskinTokens: ['modal.bg', 'text.primary', 'accent.primary'] },
    // audio
    { name: 'WaveformEditor', category: 'audio', jsxTag: 'Waveform', nativeClass: 'pulp::view::WaveformView', usage: 'Sample waveform display / edit', reskinTokens: ['waveform.line', 'waveform.fill', 'waveform.grid'] },
    { name: 'Spectrum', category: 'audio', jsxTag: 'Spectrum', nativeClass: 'pulp::view::SpectrumView', usage: 'Frequency spectrum analyser', reskinTokens: ['waveform.line', 'waveform.grid', 'accent.primary'] },
    { name: 'XYPad', category: 'audio', jsxTag: 'XYPad', nativeClass: 'pulp::view::XYPad', usage: '2-D parameter control surface', reskinTokens: ['control.track', 'accent.primary', 'control.thumb'] },
    { name: 'MIDIKeyboard', category: 'audio', jsxTag: null, nativeClass: 'pulp::view::MidiKeyboard', usage: 'Piano keyboard input surface', reskinTokens: ['bg.surface', 'text.primary', 'accent.primary'] },
    // feedback
    { name: 'InlineBanner', category: 'feedback', jsxTag: null, nativeClass: 'pulp::view::InlineBanner', usage: 'Full-width status message', reskinTokens: ['bg.elevated', 'accent.primary', 'accent.error', 'accent.success'] },
    { name: 'EmptyState', category: 'feedback', jsxTag: null, nativeClass: 'pulp::view::EmptyState', usage: 'Placeholder with a call to action', reskinTokens: ['bg.surface', 'text.secondary', 'accent.primary'] },
];

/** Look up a catalog component by its name (case-sensitive). */
export function findComponent(name: string): DesignComponent | undefined {
    return inkSignalCatalog.find((c) => c.name === name);
}

/** All components in a category, in catalog order. */
export function componentsByCategory(category: DesignCategory): DesignComponent[] {
    return inkSignalCatalog.filter((c) => c.category === category);
}
