# Design-System Naming — Figma ↔ SDK

The **Ink & Signal** design system is authored in Figma and mirrored by
native `pulp::view` widgets. This page documents how a Figma primitive name
maps to the SDK class that renders it, so you can go from a frame title in the
Figma file to the C++ type you instantiate (and back).

## The rule

A Figma primitive is identified by the title under each example card on the
Figma **Overview** page (e.g. `Knob`, `ComboBox`, `Sidebar`). For each one:

1. **Names already match** → use the SDK class directly. Most primitives are
   in this group (`Knob` → `pulp::view::Knob`, `ComboBox` →
   `pulp::view::ComboBox`, and so on).
2. **Names differ** → the canonical SDK class keeps its existing framework
   name, and a `using` **alias** with the Figma name is declared in the same
   header. Both names resolve to the same type, so design-system code can use
   the Figma name with no breaking rename of the underlying class.

Aliases are deliberately conservative: one is added **only** when no existing
symbol of that name already lives in `pulp::view`, so an alias can never shadow
or collide with an unrelated type. The alias sits in the canonical class's own
header, next to the definition it points at.

## Aliases (Figma name differs from SDK class)

| Figma primitive | SDK alias (`pulp::view::`) | Canonical class | Header |
|-----------------|----------------------------|-----------------|--------|
| `Sidebar`   | `Sidebar`   | `SidePanel`    | `side_panel.hpp`   |
| `PopupMenu` | `PopupMenu` | `ContextMenu`  | `context_menu.hpp` |
| `Table`     | `Table`     | `TableListBox` | `table.hpp`        |

```cpp
#include <pulp/view/side_panel.hpp>
auto rail = std::make_unique<pulp::view::Sidebar>();   // == SidePanel
```

## Direct matches (Figma name == SDK class)

`Knob`, `Knob Modulation` (`Knob` + `set_modulation_rings`), `Slider`
(`RangeSlider`), `Fader`, `Pan` (`PanControl`), `Toggle`, `Checkbox`,
`Stepper`, `XY Pad` (`XYPad`), `Button` (`TextButton`), `Input` / `Search` /
`TextArea` (`TextEditor` modes), `NumberBox`, `ComboBox`, `Meter`,
`ProgressBar`, `Badge`, `InlineBanner`, `Toast`, `EmptyState`, `Spinner`,
`Tooltip`, `Tab` (`TabPanel`), `Toolbar`, `Breadcrumb`, `ScrollBar`, `Tree`
(`TreeView`), `Spectrum` (`SpectrumView`), `MidiKeyboard`, `WaveformEditor`
(`WaveformView`), `ChannelStrip`, `Dialog` (`InCanvasDialog`), `Popover`,
`Callout` (`CallOutBox`).

Where a parenthesised class is shown, the Figma title is realised by that SDK
type (sometimes a configured mode of a more general widget rather than a
dedicated class).

## Collision hygiene

The reason aliases (not renames) are used:

- A rename would ripple through every caller of `SidePanel` / `ContextMenu` /
  `TableListBox` — a breaking change for a cosmetic, design-system-facing goal.
- An alias is additive and local. Because each is gated on "no existing symbol
  of this name", it cannot introduce an ambiguous overload or hide an unrelated
  type.

One pre-existing hazard to be aware of: `table.hpp` and `table_model.hpp` both
declare `TableModel` and `TableColumn` in `pulp::view`. They are **not**
co-included today, and must not be — `table.hpp` (with `TableListBox` / the
`Table` alias) is the widget header; `table_model.hpp` is a separate data layer.
Keep them apart in any single translation unit.

## Where the names come from

The authoritative list of primitives is the Figma **Overview** page of the
Ink & Signal file, grouped into sections (`Controls`, `Buttons & inputs`,
`Status & feedback`, `Navigation`, `Data`, `Audio`, `Overlays`). Each example
card carries the primitive's title. The
`examples/ink-signal-showcase` app is the SDK-side mirror of that page: every
Overview primitive appears there, in both the dark and light theme, wired up
interactively.
