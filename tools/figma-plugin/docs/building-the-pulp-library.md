# Phase 0 — Building the Pulp Figma Library file

This is the spec for a Figma file you (a human) build in Figma desktop. The Pulp plugin then recognizes instances of components from this file when designers consume it in their own designs.

**This is not code work.** It's design work that produces a Figma file whose component keys we record in `library-manifest.json`.

---

## What you're building

A single Figma file titled **"Pulp Library v0.1"** containing ONE component (for the v0.1 slice) — `Pulp / Knob` — that will be published as a Figma Community library or as a private team library. The file is sized at ~512×512 with a single test page named "Components."

### Component: `Pulp / Knob`

Name the component set exactly `Pulp / Knob` (the `/` creates Figma's component-grouping convention; the name prefix is what the plugin falls back to when the component key isn't in `library-manifest.json` yet).

**Variants (`variantProperties`):**

| Axis | Values | Notes |
|---|---|---|
| `size` | `sm`, `md`, `lg` | sm = 28×41, md = 48×64, lg = 64×88 (visual sizing only; doesn't change the audio model) |
| `state` | `default`, `active`, `disabled` | Used by the plugin to skip recognition of disabled states later if we want |

**Component properties (`componentPropertyDefinitions`):**

| Property | Type | Default | Notes |
|---|---|---|---|
| `label` | TEXT | `"VALUE"` | Designer fills with the widget label. Plugin reads → `audio.label` in export. |
| `value` | TEXT | `"50%"` | Designer fills with the displayed value (free-form). Plugin captures but doesn't currently use. |
| `min` | TEXT | `"0"` | Range hint (parsed as number). |
| `max` | TEXT | `"1"` | Range hint (parsed as number). |
| `units` | TEXT | `""` | Optional units string ("Hz", "%", "dB"). |
| `binding` | TEXT | `""` | Designer types the audio param name here (e.g. `"filter_cutoff"`). Blank means TODO_BIND. Plugin reads → `binding.key` in export. |

### Visual design

The visual design of the knob can be anything you want — the plugin doesn't care about the visual fidelity of the library component itself. It reads the metadata (component key, variants, properties) and emits a Pulp `knob` node. The Pulp importer then renders Pulp's native knob widget at runtime, not the Figma visual.

For our test purposes a simple grey circle with a tick mark and a label below it is enough. Match the size for each variant.

---

## Publishing

You have two options, pick whichever you can do:

### Option A — Private team library (recommended for testing)

1. In Figma: **Assets panel → Publish library**.
2. Title: "Pulp Library v0.1 (test)".
3. After publish, **right-click the Knob component set → Copy ComponentSet key** (the long hex string).
4. Paste that key into `tools/figma-plugin/library-manifest.json` at `widgets.knob.component_set_key`.
5. Done. The plugin can now identify instances of this component in any file in your team that has the library enabled.

### Option B — Figma Community publish (recommended for public release)

Same as Option A, but published to the Figma Community at `figma.com/community`. Adds Figma review (5-10 business days) but makes the library discoverable. Save for after the plugin works end-to-end.

---

## Acceptance check

You've completed Phase 0 when:

- [ ] A Figma file exists titled "Pulp Library v0.1" containing the `Pulp / Knob` component set
- [ ] The component set has `size` and `state` variants
- [ ] The component set has the six text properties (`label`, `value`, `min`, `max`, `units`, `binding`)
- [ ] The library is published (Option A or B)
- [ ] The component set key is pasted into `library-manifest.json`
- [ ] You can drop a Knob instance into a fresh Figma file, fill in `label = "CUTOFF"` and `binding = "filter_cutoff"`, and the plugin's "Refresh selection" reports it back with the correct type and name

When all six boxes are checked, Phase 0 is done and Phase 2a (walker) can start.

---

## Why not just generate this in code?

Figma doesn't expose a "create components programmatically from outside the editor" API. You can author components from inside a plugin, but bootstrapping the library file itself from outside Figma isn't possible. So the spec exists here as docs; the actual file is built by hand in Figma.

The plugin can read the file's metadata via the API once it's authored. The library file is "the design system spec made into Figma artifacts" rather than something we author in code.
